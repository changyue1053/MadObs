// C ABI 实现（实施计划 P6.1）：异常防火墙 + 自我熔断 + 句柄世代 + C 回调桥。
#include "imobs/imobs_c.h"
#include "madobs/runtime.h"

#include <cstdio>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "imobs/crash_sentry.h"
#include "imobs/obs_kernel.h"

namespace {

using imobs::IActionExecutor;
using imobs::IChannel;
using imobs::KernelOptions;
using imobs::LabelDef;
using imobs::ObsKernel;
using imobs::Priority;
using imobs::SceneSpec;
using imobs::SchemaRegistry;

constexpr uint32_t kMaxInstances = 8;
constexpr uint64_t kDisableThreshold = 8;

// 进程级防火墙账本：观测 SDK 自己反复出错时，第一要务是闭嘴而不是坚持工作。
std::atomic<uint64_t> g_internal_errors{0};
std::atomic<bool> g_disabled{false};

void RecordInternalError() {
  if (g_internal_errors.fetch_add(1, std::memory_order_relaxed) + 1 >= kDisableThreshold) {
    g_disabled.store(true, std::memory_order_release);
  }
}

// —— 通道 C 桥：done 回调经蜡纸（trampoline）转回 std::function ——
struct CChannel final : IChannel {
  imobs_channel_send_fn send_fn = nullptr;
  void* ctx = nullptr;

  struct DoneCtx {
    SendCallback cb;
  };
  static void DoneTrampoline(void* dctx, int ok) {
    auto* d = static_cast<DoneCtx*>(dctx);
    d->cb({ok != 0});
    delete d;  // 契约：宿主恰好调用一次 done；违约 = 泄漏一个 DoneCtx（可观测于泄漏审计）
  }
  void Send(Priority pri, uint64_t batch_id, std::string_view payload,
            SendCallback cb) override {
    send_fn(ctx, static_cast<uint8_t>(pri), batch_id,
            reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), &DoneTrampoline,
            new DoneCtx{std::move(cb)});  // audit-allow: cold-path 批级上报回调蜡纸（消费线程，秒级/批），非打点 per-event 路径
  }
};

// —— 自愈执行器 C 桥 ——
struct CExecutor final : IActionExecutor {
  imobs_heal_execute_fn exec_fn = nullptr;
  imobs_heal_verify_fn verify_fn = nullptr;
  void* ctx = nullptr;

  struct DoneCtx {
    DoneFn fn;
  };
  static void DoneTrampoline(void* dctx, int ok) {
    auto* d = static_cast<DoneCtx*>(dctx);
    d->fn(ok != 0);
    delete d;
  }
  void Execute(uint64_t scene_id, const std::string& action, DoneFn done) override {  // audit-allow: cold-path 自愈动作执行（分钟级）
    exec_fn(ctx, scene_id, action.c_str(), &DoneTrampoline, new DoneCtx{std::move(done)});  // audit-allow: cold-path 自愈动作回调蜡纸（分钟级）
  }
  void Verify(uint64_t scene_id, DoneFn done) override {
    verify_fn(ctx, scene_id, &DoneTrampoline, new DoneCtx{std::move(done)});  // audit-allow: cold-path 自愈验证回调蜡纸（分钟级）
  }
};

struct Instance {
  uint32_t generation = 0;
  std::atomic<bool> running{false};
  std::string dir;  // audit-allow: cold-path 实例配置字段，create 期一次
  SchemaRegistry schema;
  KernelOptions opt;
  std::vector<uint8_t> log_key;  // 日志加密钥的实例级拷贝（opt.log_key32 指向它）  // audit-allow: cold-path 配置期字段拷贝
  std::unique_ptr<CChannel> channel;
  std::vector<std::unique_ptr<CExecutor>> executors;  // audit-allow: cold-path 注册期容器，start 后只读
  std::unique_ptr<ObsKernel> kernel;  // start 后非空
};

struct Slot {
  std::atomic<Instance*> inst{nullptr};
  uint32_t next_gen = 1;
};
Slot g_slots[kMaxInstances];
std::mutex g_lifecycle_mu;  // create/start/stop/destroy 冷路径互斥  // audit-allow: cold-path 生命周期互斥，打点函数不经过

imobs_handle_t MakeHandle(uint32_t idx, uint32_t gen) {
  return (static_cast<uint64_t>(gen) << 32) | (idx + 1);
}

Instance* Resolve(imobs_handle_t h) {
  if (h == 0) return nullptr;
  const uint32_t idx = static_cast<uint32_t>(h & 0xFFFFFFFFu) - 1;
  const uint32_t gen = static_cast<uint32_t>(h >> 32);
  if (idx >= kMaxInstances) return nullptr;
  Instance* inst = g_slots[idx].inst.load(std::memory_order_acquire);
  if (inst == nullptr || inst->generation != gen) return nullptr;  // 悬空句柄：拒绝而非 UAF
  return inst;
}

bool TranslateLabels(const imobs_label_def_t* labels, size_t n, std::vector<LabelDef>* out) {  // audit-allow: cold-path 注册期 label 翻译（start 前）
  out->clear();
  for (size_t i = 0; i < n; ++i) {
    if (labels[i].key == nullptr || labels[i].values == nullptr) return false;
    LabelDef def;
    def.key = labels[i].key;
    for (const char* const* v = labels[i].values; *v != nullptr; ++v) def.values.push_back(*v);
    out->push_back(std::move(def));
  }
  return true;
}

}  // namespace

// 防火墙：任何 C++ 异常不得越境（跨 FFI 的 unwind 是未定义行为）。
#define IMOBS_GUARD_STATUS_BEGIN                                          \
  if (g_disabled.load(std::memory_order_acquire)) return IMOBS_E_DISABLED; \
  try {
#define IMOBS_GUARD_STATUS_END                 \
  }                                            \
  catch (...) {                                \
    RecordInternalError();                     \
    return IMOBS_E_INTERNAL;                   \
  }
#define IMOBS_GUARD_VOID_BEGIN                                  \
  if (g_disabled.load(std::memory_order_acquire)) return;        \
  try {
#define IMOBS_GUARD_VOID_END \
  }                          \
  catch (...) {              \
    RecordInternalError();   \
  }

extern "C" {

imobs_handle_t imobs_create(const char* wal_dir, imobs_channel_send_fn send, void* channel_ctx) {
  if (g_disabled.load(std::memory_order_acquire)) return 0;
  try {
    if (wal_dir == nullptr || wal_dir[0] == '\0' || send == nullptr) return 0;
    std::lock_guard<std::mutex> lk(g_lifecycle_mu);  // audit-allow: cold-path create 生命周期互斥
    for (uint32_t i = 0; i < kMaxInstances; ++i) {
      if (g_slots[i].inst.load(std::memory_order_relaxed) != nullptr) continue;
      auto inst = std::make_unique<Instance>();  // audit-allow: cold-path create 期实例分配
      inst->generation = g_slots[i].next_gen++;
      inst->dir = wal_dir;
      inst->channel = std::make_unique<CChannel>();  // audit-allow: cold-path create 期通道桥分配
      inst->channel->send_fn = send;
      inst->channel->ctx = channel_ctx;
      const imobs_handle_t h = MakeHandle(i, inst->generation);
      g_slots[i].inst.store(inst.release(), std::memory_order_release);
      return h;
    }
    return 0;  // IMOBS_E_TOO_MANY_INSTANCES 语义（句柄接口只能以 0 表达）
  } catch (...) {
    RecordInternalError();
    return 0;
  }
}

imobs_status_t imobs_set_control_key(imobs_handle_t h, const uint8_t* key, size_t len) {
  IMOBS_GUARD_STATUS_BEGIN
  Instance* inst = Resolve(h);
  if (inst == nullptr) return IMOBS_E_INVALID_HANDLE;
  if (key == nullptr && len != 0) return IMOBS_E_INVALID_ARG;
  if (inst->kernel) return IMOBS_E_BAD_STATE;
  inst->opt.control_key.assign(reinterpret_cast<const char*>(key), len);
  return IMOBS_OK;
  IMOBS_GUARD_STATUS_END
}

imobs_status_t imobs_enable_logs(imobs_handle_t h, const char* path, uint32_t segment_size,
                                 uint32_t segment_count, const uint8_t* key32) {
  IMOBS_GUARD_STATUS_BEGIN
  Instance* inst = Resolve(h);
  if (inst == nullptr) return IMOBS_E_INVALID_HANDLE;
  if (path == nullptr || segment_size == 0 || segment_count == 0) return IMOBS_E_INVALID_ARG;
  if (inst->kernel) return IMOBS_E_BAD_STATE;
  inst->opt.log_path = path;
  inst->opt.log_segment_size = segment_size;
  inst->opt.log_segment_count = segment_count;
  if (key32 != nullptr) {
    inst->log_key.assign(key32, key32 + 32);  // 拷贝：宿主缓冲可即刻失效
  } else {
    inst->log_key.clear();
  }
  return IMOBS_OK;
  IMOBS_GUARD_STATUS_END
}

imobs_status_t imobs_set_tuning(imobs_handle_t h, const imobs_tuning_t* t) {
  IMOBS_GUARD_STATUS_BEGIN
  Instance* inst = Resolve(h);
  if (inst == nullptr) return IMOBS_E_INVALID_HANDLE;
  if (t == nullptr) return IMOBS_E_INVALID_ARG;
  if (inst->kernel) return IMOBS_E_BAD_STATE;
  if (t->snapshot_interval_ms != 0) inst->opt.metrics_snapshot_interval_ms = t->snapshot_interval_ms;
  if (t->trace_chunk_max_age_ms != 0) inst->opt.trace_chunk_max_age_ms = t->trace_chunk_max_age_ms;
  if (t->poll_interval_ms != 0) inst->opt.poll_interval_ms = t->poll_interval_ms;
  if (t->p1_flush_bytes != 0) inst->opt.pipeline.p1_flush_bytes = t->p1_flush_bytes;
  if (t->p2_flush_interval_ms != 0) inst->opt.pipeline.p2_flush_interval_ms = t->p2_flush_interval_ms;
  return IMOBS_OK;
  IMOBS_GUARD_STATUS_END
}

imobs_status_t imobs_register_counter(imobs_handle_t h, const char* name,
                                      const imobs_label_def_t* labels, size_t n_labels,
                                      uint64_t* out_key) {
  IMOBS_GUARD_STATUS_BEGIN
  Instance* inst = Resolve(h);
  if (inst == nullptr) return IMOBS_E_INVALID_HANDLE;
  if (name == nullptr || out_key == nullptr || (labels == nullptr && n_labels != 0)) {
    return IMOBS_E_INVALID_ARG;
  }
  if (inst->kernel) return IMOBS_E_BAD_STATE;
  std::vector<LabelDef> defs;  // audit-allow: cold-path 注册期（start 前）
  if (!TranslateLabels(labels, n_labels, &defs)) return IMOBS_E_INVALID_ARG;
  const uint64_t key = inst->schema.RegisterCounter(name, std::move(defs));
  if (key == 0) return IMOBS_E_SCHEMA_REJECTED;
  *out_key = key;
  return IMOBS_OK;
  IMOBS_GUARD_STATUS_END
}

imobs_status_t imobs_register_histogram(imobs_handle_t h, const char* name,
                                        const imobs_label_def_t* labels, size_t n_labels,
                                        uint64_t* out_key) {
  IMOBS_GUARD_STATUS_BEGIN
  Instance* inst = Resolve(h);
  if (inst == nullptr) return IMOBS_E_INVALID_HANDLE;
  if (name == nullptr || out_key == nullptr || (labels == nullptr && n_labels != 0)) {
    return IMOBS_E_INVALID_ARG;
  }
  if (inst->kernel) return IMOBS_E_BAD_STATE;
  std::vector<LabelDef> defs;  // audit-allow: cold-path 注册期（start 前）
  if (!TranslateLabels(labels, n_labels, &defs)) return IMOBS_E_INVALID_ARG;
  const uint64_t key = inst->schema.RegisterHistogram(name, std::move(defs));
  if (key == 0) return IMOBS_E_SCHEMA_REJECTED;
  *out_key = key;
  return IMOBS_OK;
  IMOBS_GUARD_STATUS_END
}

imobs_status_t imobs_resolve_labels(imobs_handle_t h, uint64_t key, const char* const* keys,
                                    const char* const* values, size_t n, uint16_t* out_idx) {
  IMOBS_GUARD_STATUS_BEGIN
  Instance* inst = Resolve(h);
  if (inst == nullptr) return IMOBS_E_INVALID_HANDLE;
  if (out_idx == nullptr || ((keys == nullptr || values == nullptr) && n != 0)) {
    return IMOBS_E_INVALID_ARG;
  }
  std::vector<std::pair<std::string_view, std::string_view>> combo;  // audit-allow: cold-path 冷路径解析一次，热路径只带索引（event.h 契约）
  combo.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    if (keys[i] == nullptr || values[i] == nullptr) return IMOBS_E_INVALID_ARG;
    combo.emplace_back(keys[i], values[i]);
  }
  if (!inst->schema.ResolveLabelIndex(key, combo, out_idx)) return IMOBS_E_SCHEMA_REJECTED;
  return IMOBS_OK;
  IMOBS_GUARD_STATUS_END
}

imobs_status_t imobs_register_heal_scene(imobs_handle_t h, const imobs_scene_spec_t* spec,
                                         imobs_heal_execute_fn execute,
                                         imobs_heal_verify_fn verify, void* exec_ctx) {
  IMOBS_GUARD_STATUS_BEGIN
  Instance* inst = Resolve(h);
  if (inst == nullptr) return IMOBS_E_INVALID_HANDLE;
  if (spec == nullptr || execute == nullptr || verify == nullptr || spec->ladder == nullptr ||
      spec->n_actions == 0 || spec->signals_all_of == nullptr || spec->n_signals == 0) {
    return IMOBS_E_INVALID_ARG;
  }
  if (inst->kernel) return IMOBS_E_BAD_STATE;

  SceneSpec s;
  s.scene_id = spec->scene_id;
  s.detector.all_of.assign(spec->signals_all_of, spec->signals_all_of + spec->n_signals);
  s.detector.debounce_ms = spec->debounce_ms;
  s.detector.debounce_jitter_ms = spec->debounce_jitter_ms;
  s.detector.diagnose_ms = spec->diagnose_ms;
  for (size_t i = 0; i < spec->n_actions; ++i) {
    const imobs_action_spec_t& a = spec->ladder[i];
    if (a.name == nullptr || a.damage_level > 2) return IMOBS_E_INVALID_ARG;
    imobs::ActionSpec as;
    as.name = a.name;
    as.level = static_cast<imobs::DamageLevel>(a.damage_level);
    as.cooldown_ms = a.cooldown_ms;
    as.max_per_day = a.max_per_day;
    as.needs_precondition = a.needs_precondition != 0;
    as.timeout_ms = a.timeout_ms;
    s.ladder.push_back(std::move(as));
  }
  s.verify_timeout_ms = spec->verify_timeout_ms;
  s.breaker_threshold = spec->breaker_threshold;
  s.muted_ttl_ms = spec->muted_ttl_ms;

  auto exec = std::make_unique<CExecutor>();  // audit-allow: cold-path 自愈场景注册期（start 前）
  exec->exec_fn = execute;
  exec->verify_fn = verify;
  exec->ctx = exec_ctx;
  inst->opt.heal_scenes.push_back({std::move(s), exec.get()});
  inst->executors.push_back(std::move(exec));
  return IMOBS_OK;
  IMOBS_GUARD_STATUS_END
}

imobs_status_t imobs_start(imobs_handle_t h) {
  IMOBS_GUARD_STATUS_BEGIN
  std::lock_guard<std::mutex> lk(g_lifecycle_mu);  // audit-allow: cold-path start 生命周期互斥
  Instance* inst = Resolve(h);
  if (inst == nullptr) return IMOBS_E_INVALID_HANDLE;
  if (inst->kernel) return inst->running.load() ? IMOBS_OK : IMOBS_E_BAD_STATE;  // stop is terminal
  // 日志加密钥：指向实例级拷贝（与内核同生命周期；start 后配置期字段不再变更）。
  inst->opt.log_key32 = inst->log_key.empty() ? nullptr : inst->log_key.data();
  std::string err;  // audit-allow: cold-path start 期错误载体
  inst->kernel = ObsKernel::Start(inst->dir, inst->channel.get(), &inst->schema, inst->opt, &err);
  inst->running.store(inst->kernel != nullptr);
  return inst->kernel ? IMOBS_OK : IMOBS_E_OPEN_FAILED;
  IMOBS_GUARD_STATUS_END
}

imobs_status_t imobs_stop(imobs_handle_t h) {
  try { // Cleanup must remain available after the telemetry firewall trips.
  std::lock_guard<std::mutex> lk(g_lifecycle_mu);  // audit-allow: cold-path stop 生命周期互斥
  Instance* inst = Resolve(h);
  if (inst == nullptr) return IMOBS_E_INVALID_HANDLE;
  inst->running.store(false);
  if (inst->kernel) inst->kernel->Stop();
  return IMOBS_OK;
  IMOBS_GUARD_STATUS_END
}

imobs_status_t imobs_destroy(imobs_handle_t h) {
  try { // Cleanup must remain available after the telemetry firewall trips.
  std::lock_guard<std::mutex> lk(g_lifecycle_mu);  // audit-allow: cold-path destroy 生命周期互斥
  Instance* inst = Resolve(h);
  if (inst == nullptr) return IMOBS_E_INVALID_HANDLE;
  const uint32_t idx = static_cast<uint32_t>(h & 0xFFFFFFFFu) - 1;
  inst->running.store(false);
  if (inst->kernel) inst->kernel->Stop();  // 契约：destroy 期间宿主不得并发调用其他 API
  g_slots[idx].inst.store(nullptr, std::memory_order_release);
  delete inst;
  return IMOBS_OK;
  IMOBS_GUARD_STATUS_END
}

void imobs_counter_add(imobs_handle_t h, uint64_t key, uint16_t label_idx, uint64_t delta) {
  IMOBS_GUARD_VOID_BEGIN
  Instance* inst = Resolve(h);
  if (inst != nullptr && inst->kernel) inst->kernel->AddCounter(key, label_idx, delta);
  IMOBS_GUARD_VOID_END
}

void imobs_histogram_record(imobs_handle_t h, uint64_t key, uint16_t label_idx, uint64_t value) {
  IMOBS_GUARD_VOID_BEGIN
  Instance* inst = Resolve(h);
  if (inst != nullptr && inst->kernel) inst->kernel->RecordHistogram(key, label_idx, value);
  IMOBS_GUARD_VOID_END
}

void imobs_trace_stage(imobs_handle_t h, uint64_t msg_id, uint8_t stage, uint8_t status,
                       uint32_t attr, uint64_t value) {
  IMOBS_GUARD_VOID_BEGIN
  Instance* inst = Resolve(h);
  if (inst != nullptr && inst->kernel) {
    inst->kernel->TraceStage(msg_id, stage, status, attr, value);
  }
  IMOBS_GUARD_VOID_END
}

void imobs_emit_event(imobs_handle_t h, uint64_t key, uint8_t level, uint32_t a, uint32_t b) {
  IMOBS_GUARD_VOID_BEGIN
  Instance* inst = Resolve(h);
  if (inst != nullptr && inst->kernel) inst->kernel->EmitEvent(key, level, a, b);
  IMOBS_GUARD_VOID_END
}

void imobs_health_signal(imobs_handle_t h, uint64_t signal_id, int active) {
  IMOBS_GUARD_VOID_BEGIN
  Instance* inst = Resolve(h);
  if (inst != nullptr && inst->kernel) inst->kernel->SetHealthSignal(signal_id, active != 0);
  IMOBS_GUARD_VOID_END
}

void imobs_global_outage(imobs_handle_t h, int outage) {
  IMOBS_GUARD_VOID_BEGIN
  Instance* inst = Resolve(h);
  if (inst != nullptr && inst->kernel) inst->kernel->SetGlobalOutage(outage != 0);
  IMOBS_GUARD_VOID_END
}

void imobs_set_foreground(imobs_handle_t h, int foreground) {
  IMOBS_GUARD_VOID_BEGIN
  Instance* inst = Resolve(h);
  if (inst != nullptr && inst->kernel) inst->kernel->SetForeground(foreground != 0);
  IMOBS_GUARD_VOID_END
}

void imobs_submit_control(imobs_handle_t h, const uint8_t* data, size_t len) {
  IMOBS_GUARD_VOID_BEGIN
  Instance* inst = Resolve(h);
  if (inst != nullptr && inst->kernel && data != nullptr && len != 0) {
    inst->kernel->SubmitControlEnvelope(std::string_view(reinterpret_cast<const char*>(data), len));
  }
  IMOBS_GUARD_VOID_END
}

void imobs_log(imobs_handle_t h, uint8_t level, uint64_t module_hash, const char* text,
               size_t text_len) {
  IMOBS_GUARD_VOID_BEGIN
  Instance* inst = Resolve(h);
  if (inst != nullptr && inst->kernel && text != nullptr) {
    inst->kernel->Log(level, module_hash, std::string_view(text, text_len));
  }
  IMOBS_GUARD_VOID_END
}

void imobs_set_fetch_consent(imobs_handle_t h, int granted) {
  IMOBS_GUARD_VOID_BEGIN
  Instance* inst = Resolve(h);
  if (inst != nullptr && inst->kernel) inst->kernel->SetFetchConsent(granted != 0);
  IMOBS_GUARD_VOID_END
}

uint64_t imobs_name_hash(const char* name) {
  if (name == nullptr) return 0;
  return imobs::Fnv1a(name);
}

imobs_status_t imobs_get_self_stats(imobs_handle_t h, imobs_self_stats_t* out) {
  IMOBS_GUARD_STATUS_BEGIN
  Instance* inst = Resolve(h);
  if (inst == nullptr) return IMOBS_E_INVALID_HANDLE;
  if (out == nullptr) return IMOBS_E_INVALID_ARG;
  imobs::KernelCore::SelfStats s{};
  if (inst->kernel) s = inst->kernel->core().self_stats();
  out->drained = s.drained;
  out->ring_dropped = s.ring_dropped;
  out->unknown_key_dropped = s.unknown_key_dropped;
  out->trace_sampled_out = s.trace_sampled_out;
  out->internal_errors = g_internal_errors.load(std::memory_order_relaxed);
  return IMOBS_OK;
  IMOBS_GUARD_STATUS_END
}

void imobs_debug_force_internal_error(void) { RecordInternalError(); }

void imobs_debug_reset_firewall(void) {
  g_internal_errors.store(0, std::memory_order_relaxed);
  g_disabled.store(false, std::memory_order_release);
}

/* —— 崩溃哨兵出海（OB-1）—— */

imobs_status_t imobs_crash_install(const char* marker_path) {
  IMOBS_GUARD_STATUS_BEGIN
  if (marker_path == nullptr) return IMOBS_E_INVALID_ARG;
  imobs::CrashSentry::Options opt;
  opt.path = marker_path;
  std::string err;  // audit-allow: cold-path 崩溃哨兵安装，进程启动一次
  return imobs::CrashSentry::Install(opt, &err) ? IMOBS_OK : IMOBS_E_OPEN_FAILED;
  IMOBS_GUARD_STATUS_END
}

int32_t imobs_crash_consume_pending(imobs_handle_t h, const char* marker_path) {
  if (marker_path == nullptr) return -static_cast<int32_t>(IMOBS_E_INVALID_ARG);
  try {
    imobs::CrashMarker marker;
    imobs::CrashBacktrace bt;
    if (!imobs::CrashSentry::TakePendingMarker(marker_path, &marker, &bt)) return 0;
    // P0 事件入账：a=信号号，b=崩溃总序号（上报优先级最高，永不覆盖铁律随 WAL）。
    imobs_emit_event(h, imobs_name_hash("imobs.crash"), /*level=*/0,
                     static_cast<uint32_t>(marker.signal_no), marker.seq);
    // 回溯以结构化日志入账（回捞后 file_addr = pc - slide 离线符号化）。
    char line[1024];
    int n = std::snprintf(line, sizeof(line),
                          "crash sig=%d seq=%u wall_ms=%lld fault=0x%llx slide=0x%llx pcs=",
                          marker.signal_no, marker.seq,
                          static_cast<long long>(marker.wall_ms),
                          static_cast<unsigned long long>(marker.fault_addr),
                          static_cast<unsigned long long>(bt.image_slide));
    for (uint32_t i = 0; i < bt.n_frames && n > 0 &&
                         n < static_cast<int>(sizeof(line)) - 20; ++i) {
      n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), "%llx,",
                         static_cast<unsigned long long>(bt.pc[i]));
    }
    imobs_log(h, /*level=*/3, imobs_name_hash("imobs.crash"), line,
              static_cast<size_t>(n > 0 ? n : 0));
    return 1;
  } catch (...) {
    RecordInternalError();
    return -static_cast<int32_t>(IMOBS_E_INTERNAL);
  }
}

int32_t imobs_crash_in_loop(const char* marker_path, uint32_t threshold, int64_t window_ms,
                            int64_t now_wall_ms) {
  if (marker_path == nullptr) return -static_cast<int32_t>(IMOBS_E_INVALID_ARG);
  try {
    return imobs::CrashSentry::InCrashLoop(marker_path, threshold, window_ms, now_wall_ms)
               ? 1
               : 0;
  } catch (...) {
    RecordInternalError();
    return -static_cast<int32_t>(IMOBS_E_INTERNAL);
  }
}

}  // extern "C"

namespace {
void MadObserverEmit(void* ctx,const madobs_event_v1* event) {
  auto* binding=static_cast<madobs_observer_binding_v1*>(ctx);
  if(binding) (void)madobs_emit_generic_event(binding->runtime_handle,event);
}
}
madobs_status_t madobs_emit_generic_event(madobs_handle_t h,const madobs_event_v1* event) {
  IMOBS_GUARD_STATUS_BEGIN
  if(!imobs::ValidGenericEvent(event)) return MADOBS_E_INVALID_ARG;
  Instance* inst=Resolve(h);
  if(!inst) return MADOBS_E_INVALID_HANDLE;
  if(!inst->running.load(std::memory_order_acquire)) return MADOBS_E_BAD_STATE;
  // Copy only active attributes. Unused caller bytes never enter queues or wire.
  madobs_event_v1 copy{}; copy.struct_size=sizeof(copy); copy.abi_version=1;
  copy.sdk_id=event->sdk_id; copy.sdk_version=event->sdk_version;
  copy.event_id=event->event_id; copy.severity=event->severity;
  copy.monotonic_ns=event->monotonic_ns; copy.attribute_count=event->attribute_count;
  for(uint32_t i=0;i<copy.attribute_count;++i) copy.attributes[i]=event->attributes[i];
  return inst->kernel->core().EmitGeneric(copy)?MADOBS_OK:MADOBS_E_QUEUE_FULL;
  IMOBS_GUARD_STATUS_END
}
madobs_status_t madobs_observer_for_runtime(madobs_handle_t h,madobs_observer_binding_v1* binding,madobs_observer_v1* out) {
  IMOBS_GUARD_STATUS_BEGIN
  if(!binding||!out) return MADOBS_E_INVALID_ARG;
  Instance* inst=Resolve(h);
  if(!inst) return MADOBS_E_INVALID_HANDLE;
  if(!inst->running.load()) return MADOBS_E_BAD_STATE;
  binding->runtime_handle=h;
  *out={sizeof(*out),MADOBS_ABI_VERSION,binding,&MadObserverEmit};
  return MADOBS_OK;
  IMOBS_GUARD_STATUS_END
}
madobs_status_t madobs_get_generic_stats(madobs_handle_t h,madobs_generic_stats_v1* out) {
  IMOBS_GUARD_STATUS_BEGIN
  if(!out) return MADOBS_E_INVALID_ARG;
  Instance* inst=Resolve(h);
  if(!inst) return MADOBS_E_INVALID_HANDLE;
  if(!inst->kernel) return MADOBS_E_BAD_STATE;
  auto& core=inst->kernel->core();
  *out={core.GenericEnqueued(),core.GenericDrained(),core.GenericDropped(),core.GenericWalDropped()};
  return MADOBS_OK;
  IMOBS_GUARD_STATUS_END
}
