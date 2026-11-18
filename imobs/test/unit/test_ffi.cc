// C ABI 剧本（实施计划 P6.1 DoD）：以宿主的姿势使用纯 C 接口——
// 注册→启动→跨线程打点→通道收批（字段级解码验证）→自愈 C 桥→句柄世代→防火墙熔断。
#include "imobs/imobs_c.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imobs/records.h"
#include "imobs/wire.h"
#include "imobs_test.h"

namespace {

std::string TmpDir() {
  char tmpl[] = "/tmp/imobs_ffi_XXXXXX";
  return std::string(mkdtemp(tmpl));
}

struct CapturedBatch {
  uint8_t pri;
  uint64_t id;
  std::vector<uint8_t> payload;
};

struct ChannelCtx {
  std::mutex mu;
  std::vector<CapturedBatch> batches;  // 工作线程写；Stop/join 后主线程读
};

void CSend(void* c, uint8_t pri, uint64_t id, const uint8_t* p, size_t n,
           imobs_send_done_fn done, void* dctx) {
  auto* ctx = static_cast<ChannelCtx*>(c);
  {
    std::lock_guard<std::mutex> lk(ctx->mu);
    ctx->batches.push_back({pri, id, std::vector<uint8_t>(p, p + n)});
  }
  done(dctx, 1);
}

// 拆批取记录（与内核测试同一手法）
std::vector<std::vector<uint8_t>> Records(const CapturedBatch& b) {
  std::vector<std::vector<uint8_t>> out;
  imobs::WireReader r(b.payload.data(), b.payload.size());
  uint64_t id = 0, wall = 0, mono = 0, n = 0;
  const uint8_t* magic = nullptr;
  uint8_t pri = 0;
  if (!r.Fixed64(&id) || !r.Bytes(&magic, 4) || !r.U8(&pri) || !r.Fixed64(&wall) ||
      !r.Fixed64(&mono) || !r.Varint(&n)) {
    return out;
  }
  for (uint64_t i = 0; i < n; ++i) {
    uint64_t len = 0;
    const uint8_t* p = nullptr;
    if (!r.Varint(&len) || !r.Bytes(&p, len)) break;
    out.emplace_back(p, p + len);
  }
  return out;
}

struct HealCtx {
  std::atomic<int> executed{0};
};

void CHealExec(void* c, uint64_t, const char*, imobs_heal_done_fn done, void* dctx) {
  static_cast<HealCtx*>(c)->executed.fetch_add(1);
  done(dctx, 1);
}
void CHealVerify(void*, uint64_t, imobs_heal_done_fn done, void* dctx) { done(dctx, 1); }

imobs_tuning_t FastTuning() {
  imobs_tuning_t t{};
  t.snapshot_interval_ms = 50;
  t.trace_chunk_max_age_ms = 20;
  t.poll_interval_ms = 1;
  t.p1_flush_bytes = 1;
  t.p2_flush_interval_ms = 1;
  return t;
}

}  // namespace

TEST(Ffi, FullChainThroughPureCApi) {
  imobs_debug_reset_firewall();
  ChannelCtx ch;
  const std::string dir = TmpDir();
  const imobs_handle_t h = imobs_create(dir.c_str(), &CSend, &ch);
  ASSERT_TRUE(h != 0);

  const imobs_tuning_t t = FastTuning();
  ASSERT_EQ(imobs_set_tuning(h, &t), IMOBS_OK);

  // 注册期：白名单 schema 经 C 数组进入；基数红线在 C 边界同样生效
  const char* net_vals[] = {"wifi", "cell", nullptr};
  const imobs_label_def_t defs[] = {{"net", net_vals}};
  uint64_t key = 0;
  ASSERT_EQ(imobs_register_counter(h, "c.abi", defs, 1, &key), IMOBS_OK);
  ASSERT_TRUE(key != 0);
  const char* uid_vals[] = {"x", nullptr};
  const imobs_label_def_t bad_defs[] = {{"uid", uid_vals}};
  uint64_t bad_key = 0;
  EXPECT_EQ(imobs_register_counter(h, "bad.abi", bad_defs, 1, &bad_key),
            IMOBS_E_SCHEMA_REJECTED);

  const char* ks[] = {"net"};
  const char* vs[] = {"cell"};
  uint16_t idx = 0;
  ASSERT_EQ(imobs_resolve_labels(h, key, ks, vs, 1, &idx), IMOBS_OK);
  EXPECT_EQ(idx, 1);

  ASSERT_EQ(imobs_start(h), IMOBS_OK);
  EXPECT_EQ(imobs_register_counter(h, "late", nullptr, 0, &key), IMOBS_E_BAD_STATE);

  // 跨线程热路径：宿主任意线程打点
  std::thread producer([&] {
    for (int i = 0; i < 3; ++i) imobs_counter_add(h, key, idx, 2);
    imobs_trace_stage(h, 0x5A5A, 4, 0, 7, 42);
    imobs_emit_event(h, imobs_name_hash("evt.p0"), 0, 1, 2);
  });
  producer.join();
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  ASSERT_EQ(imobs_stop(h), IMOBS_OK);

  bool saw_p0 = false, saw_trace = false, saw_counter = false;
  for (const auto& b : ch.batches) {
    for (const auto& rec : Records(b)) {
      if (rec.empty()) continue;
      if (rec[0] == imobs::kRecEvent && b.pri == 0) saw_p0 = true;
      if (rec[0] == imobs::kRecTrace) saw_trace = true;
      if (rec[0] == imobs::kRecMetrics) {
        std::vector<imobs::MetricsRowView> rows;
        if (DecodeMetricsChunk(rec.data(), rec.size(), &rows)) {
          for (const auto& row : rows) {
            if (row.key == key && row.count == 3 && row.sum == 6 && row.label_idx == 1) {
              saw_counter = true;
            }
          }
        }
      }
    }
  }
  EXPECT_TRUE(saw_p0);
  EXPECT_TRUE(saw_trace);
  EXPECT_TRUE(saw_counter);

  imobs_self_stats_t stats{};
  ASSERT_EQ(imobs_get_self_stats(h, &stats), IMOBS_OK);
  EXPECT_EQ(stats.ring_dropped, 0u);
  EXPECT_TRUE(stats.drained >= 5);
  ASSERT_EQ(imobs_destroy(h), IMOBS_OK);
}

TEST(Ffi, HealSceneBridgeViaC) {
  imobs_debug_reset_firewall();
  ChannelCtx ch;
  HealCtx heal;
  const std::string dir = TmpDir();
  const imobs_handle_t h = imobs_create(dir.c_str(), &CSend, &ch);
  ASSERT_TRUE(h != 0);
  const imobs_tuning_t t = FastTuning();
  ASSERT_EQ(imobs_set_tuning(h, &t), IMOBS_OK);

  const uint64_t sig = imobs_name_hash("sig.conn_dead");
  imobs_action_spec_t action{};
  action.name = "reconnect";
  action.damage_level = 0;
  action.cooldown_ms = 0;
  action.max_per_day = 100;
  action.needs_precondition = 0;
  action.timeout_ms = 2'000;
  imobs_scene_spec_t scene{};
  scene.scene_id = 0xC0;
  scene.signals_all_of = &sig;
  scene.n_signals = 1;
  scene.debounce_ms = 30;
  scene.debounce_jitter_ms = 0;
  scene.diagnose_ms = 20;
  scene.ladder = &action;
  scene.n_actions = 1;
  scene.verify_timeout_ms = 2'000;
  scene.breaker_threshold = 3;
  scene.muted_ttl_ms = 600'000;
  ASSERT_EQ(imobs_register_heal_scene(h, &scene, &CHealExec, &CHealVerify, &heal), IMOBS_OK);

  ASSERT_EQ(imobs_start(h), IMOBS_OK);
  imobs_health_signal(h, sig, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  imobs_health_signal(h, sig, 0);
  ASSERT_EQ(imobs_stop(h), IMOBS_OK);

  EXPECT_TRUE(heal.executed.load() >= 1);  // C 桥执行器被真实调用
  bool saw_heal_record = false;
  for (const auto& b : ch.batches) {
    for (const auto& rec : Records(b)) {
      if (!rec.empty() && rec[0] == imobs::kRecHeal) saw_heal_record = true;
    }
  }
  EXPECT_TRUE(saw_heal_record);  // 自愈全程留痕经 C 通道送达
  ASSERT_EQ(imobs_destroy(h), IMOBS_OK);
}

TEST(Ffi, InvalidAndStaleHandlesAreSafe) {
  imobs_debug_reset_firewall();
  // 无效句柄：热路径 no-op 不崩，状态接口报 INVALID_HANDLE
  imobs_counter_add(0, 1, 0, 1);
  imobs_trace_stage(0xDEAD, 1, 2, 3, 4, 5);
  EXPECT_EQ(imobs_start(0), IMOBS_E_INVALID_HANDLE);
  EXPECT_EQ(imobs_stop(0xDEAD), IMOBS_E_INVALID_HANDLE);

  ChannelCtx ch;
  const std::string dir = TmpDir();
  const imobs_handle_t h = imobs_create(dir.c_str(), &CSend, &ch);
  ASSERT_TRUE(h != 0);
  ASSERT_EQ(imobs_destroy(h), IMOBS_OK);
  // 悬空句柄：世代不符 → 解析失败而非 UAF
  EXPECT_EQ(imobs_start(h), IMOBS_E_INVALID_HANDLE);
  imobs_counter_add(h, 1, 0, 1);  // no-op

  // 槽位复用后旧句柄依然无效（世代递增）
  const imobs_handle_t h2 = imobs_create(dir.c_str(), &CSend, &ch);
  ASSERT_TRUE(h2 != 0);
  EXPECT_TRUE(h2 != h);
  EXPECT_EQ(imobs_stop(h), IMOBS_E_INVALID_HANDLE);
  ASSERT_EQ(imobs_destroy(h2), IMOBS_OK);

  // 参数防御
  EXPECT_TRUE(imobs_create(nullptr, &CSend, &ch) == 0);
  EXPECT_TRUE(imobs_create("", &CSend, &ch) == 0);
  EXPECT_TRUE(imobs_create("/tmp/x", nullptr, &ch) == 0);
  EXPECT_EQ(imobs_name_hash(nullptr), 0u);
}

TEST(Ffi, FirewallDisablesAfterThreshold) {  // 须为本文件最后一个用例（污染进程级状态）
  imobs_debug_reset_firewall();
  ChannelCtx ch;
  const std::string dir = TmpDir();
  const imobs_handle_t h = imobs_create(dir.c_str(), &CSend, &ch);
  ASSERT_TRUE(h != 0);
  imobs_self_stats_t stats{};
  ASSERT_EQ(imobs_get_self_stats(h, &stats), IMOBS_OK);
  EXPECT_EQ(stats.internal_errors, 0u);

  for (int i = 0; i < 8; ++i) imobs_debug_force_internal_error();

  // 熔断后：观测 SDK 全面闭嘴——新建拒绝、状态接口报 DISABLED、热路径 no-op 不崩
  EXPECT_TRUE(imobs_create(dir.c_str(), &CSend, &ch) == 0);
  EXPECT_EQ(imobs_start(h), IMOBS_E_DISABLED);
  imobs_counter_add(h, 1, 0, 1);
  imobs_emit_event(h, 2, 0, 0, 0);

  imobs_debug_reset_firewall();  // 复位，不污染后续（destroy 需防火墙放行）
  ASSERT_EQ(imobs_destroy(h), IMOBS_OK);
}

IMOBS_TEST_MAIN
