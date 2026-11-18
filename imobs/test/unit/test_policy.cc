// 控制面确定性剧本（实施计划 P3 DoD）：
// 篡改整包拒绝且原值保持、坏配置十连拒、TTL 回落默认、命令幂等与回执、临时全采自动回落。
#include "imobs/policy.h"

#include <cstdio>
#include <string>
#include <vector>

#include "imobs/clock.h"
#include "imobs/crypto.h"
#include "imobs_test.h"

using imobs::Command;
using imobs::CommandType;
using imobs::EncodeCommand;
using imobs::EncodePolicyBundle;
using imobs::kEnvelopeCommand;
using imobs::kEnvelopePolicy;
using imobs::PolicyBundle;
using imobs::PolicyClient;
using imobs::SignEnvelope;
using imobs::VirtualClock;

namespace {

constexpr const char* kKey = "imobs-test-key";

std::vector<uint8_t> SignedPolicy(const PolicyBundle& b, const char* key = kKey) {
  std::vector<uint8_t> payload;
  EncodePolicyBundle(b, &payload);
  std::vector<uint8_t> env;
  SignEnvelope(kEnvelopePolicy, payload, key, &env);
  return env;
}

std::vector<uint8_t> SignedCommand(const Command& c, const char* key = kKey) {
  std::vector<uint8_t> payload;
  EncodeCommand(c, &payload);
  std::vector<uint8_t> env;
  SignEnvelope(kEnvelopeCommand, payload, key, &env);
  return env;
}

PolicyBundle GoodBundle(uint64_t version) {
  PolicyBundle b;
  b.version = version;
  b.ttl_ms = 0;
  b.trace_sample_permille = 1000;
  b.rpc_trace_sample_permille = 10;
  b.day_quota_bytes = 2'000'000;
  b.p1_flush_interval_ms = 20'000;
  b.healer_enabled = true;
  return b;
}

}  // namespace

TEST(Policy, DefaultsWorkWithoutServer) {
  VirtualClock clk;
  PolicyClient pc(&clk, kKey);
  EXPECT_EQ(pc.effective().version, 0u);
  EXPECT_EQ(pc.EffectiveTracePermille(), 1000u);
  EXPECT_EQ(pc.effective().day_quota_bytes, 1'000'000u);
}

TEST(Policy, ValidBundleAppliesAndTamperRejectedWholesale) {
  VirtualClock clk;
  PolicyClient pc(&clk, kKey);

  auto env = SignedPolicy(GoodBundle(7));
  ASSERT_TRUE(pc.ApplyEnvelope(env.data(), env.size()));
  EXPECT_EQ(pc.effective().version, 7u);
  EXPECT_EQ(pc.effective().day_quota_bytes, 2'000'000u);
  EXPECT_EQ(pc.stats().applied, 1u);

  // 篡改 payload 一个字节：验签失败，整包拒绝，原值保持
  auto bad = SignedPolicy(GoodBundle(8));
  bad[10] ^= 0xFF;
  EXPECT_FALSE(pc.ApplyEnvelope(bad.data(), bad.size()));
  EXPECT_EQ(pc.effective().version, 7u);
  EXPECT_EQ(pc.stats().rejected_signature, 1u);

  // 错误密钥签的包同样拒绝
  auto wrong_key = SignedPolicy(GoodBundle(9), "attacker-key");
  EXPECT_FALSE(pc.ApplyEnvelope(wrong_key.data(), wrong_key.size()));
  EXPECT_EQ(pc.effective().version, 7u);
  EXPECT_EQ(pc.stats().rejected_signature, 2u);

  // 类别混淆：把策略 payload 装进命令信封（kind 参与 MAC，重签也解不出命令）
  std::vector<uint8_t> payload;
  EncodePolicyBundle(GoodBundle(9), &payload);
  std::vector<uint8_t> confused;
  SignEnvelope(kEnvelopeCommand, payload, kKey, &confused);
  EXPECT_FALSE(pc.ApplyEnvelope(confused.data(), confused.size()));
  EXPECT_EQ(pc.effective().version, 7u);
}

TEST(Policy, TenBadBundlesAllRejectedAndValuePreserved) {
  VirtualClock clk;
  PolicyClient pc(&clk, kKey);
  auto good = SignedPolicy(GoodBundle(9));
  ASSERT_TRUE(pc.ApplyEnvelope(good.data(), good.size()));

  std::vector<PolicyBundle> bad;
  {  // 1 轨迹采样超千分
    auto b = GoodBundle(10);
    b.trace_sample_permille = 1001;
    bad.push_back(b);
  }
  {  // 2 RPC 采样超千分
    auto b = GoodBundle(10);
    b.rpc_trace_sample_permille = 1001;
    bad.push_back(b);
  }
  {  // 3 关系违例：RPC 放大镜比轨迹主键更全
    auto b = GoodBundle(10);
    b.trace_sample_permille = 100;
    b.rpc_trace_sample_permille = 500;
    bad.push_back(b);
  }
  {  // 4 配额过小
    auto b = GoodBundle(10);
    b.day_quota_bytes = 1024;
    bad.push_back(b);
  }
  {  // 5 配额过大
    auto b = GoodBundle(10);
    b.day_quota_bytes = 200ull * 1024 * 1024;
    bad.push_back(b);
  }
  {  // 6 flush 间隔过短
    auto b = GoodBundle(10);
    b.p1_flush_interval_ms = 500;
    bad.push_back(b);
  }
  {  // 7 flush 间隔过长
    auto b = GoodBundle(10);
    b.p1_flush_interval_ms = 3'600'000;
    bad.push_back(b);
  }
  {  // 8 TTL 过短（抖动之源）
    auto b = GoodBundle(10);
    b.ttl_ms = 1'000;
    bad.push_back(b);
  }
  {  // 9 版本重放（等于当前）
    bad.push_back(GoodBundle(9));
  }
  {  // 10 版本降级（低于当前）
    bad.push_back(GoodBundle(8));
  }

  for (const auto& b : bad) {
    auto env = SignedPolicy(b);
    EXPECT_FALSE(pc.ApplyEnvelope(env.data(), env.size()));
    EXPECT_EQ(pc.effective().version, 9u);                 // 原值保持
    EXPECT_EQ(pc.effective().day_quota_bytes, 2'000'000u);
  }
  EXPECT_EQ(pc.stats().rejected_validation, 8u);
  EXPECT_EQ(pc.stats().rejected_version, 2u);
  EXPECT_EQ(pc.stats().applied, 1u);
}

TEST(Policy, TtlExpiryRevertsToDefaultsAndAcceptsNext) {
  VirtualClock clk;
  PolicyClient pc(&clk, kKey);
  auto b = GoodBundle(5);
  b.ttl_ms = 60'000;
  b.day_quota_bytes = 8'192;
  auto env = SignedPolicy(b);
  ASSERT_TRUE(pc.ApplyEnvelope(env.data(), env.size()));
  EXPECT_EQ(pc.effective().day_quota_bytes, 8'192u);

  clk.AdvanceMillis(59'999);
  pc.Tick();
  EXPECT_EQ(pc.effective().day_quota_bytes, 8'192u);
  clk.AdvanceMillis(1);
  pc.Tick();  // TTL 到期：回落编译期默认值
  EXPECT_EQ(pc.effective().day_quota_bytes, 1'000'000u);
  EXPECT_EQ(pc.stats().ttl_reverts, 1u);

  // 回落后从零起版：服务端重发 version=1 也能接受（不被旧高版本卡死）
  auto next = SignedPolicy(GoodBundle(1));
  EXPECT_TRUE(pc.ApplyEnvelope(next.data(), next.size()));
}

TEST(Policy, CommandDedupAndReceipts) {
  VirtualClock clk;
  PolicyClient pc(&clk, kKey);
  std::vector<PolicyClient::Receipt> receipts;
  pc.SetReceiptSink([&](const PolicyClient::Receipt& r) { receipts.push_back(r); });

  int mute_calls = 0;
  pc.RegisterHandler(CommandType::kMuteHealScene,
                     [&](const Command& c, std::string*) {
                       ++mute_calls;
                       return c.arg_a != 0;
                     });

  Command mute;
  mute.command_id = 42;
  mute.type = CommandType::kMuteHealScene;
  mute.arg_a = 0xC0;
  mute.arg_b = 60'000;
  auto env = SignedCommand(mute);
  ASSERT_TRUE(pc.ApplyEnvelope(env.data(), env.size()));
  ASSERT_TRUE(pc.ApplyEnvelope(env.data(), env.size()));  // 重复下发：幂等
  EXPECT_EQ(mute_calls, 1);                               // 只执行一次
  EXPECT_EQ(pc.stats().commands_executed, 1u);
  EXPECT_EQ(pc.stats().commands_deduped, 1u);
  ASSERT_EQ(receipts.size(), size_t{2});                  // 回执照发两条（服务端对账）
  EXPECT_TRUE(receipts[0].ok);
  EXPECT_TRUE(receipts[1].reason == "duplicate-ignored");

  // 执行失败：回执带原因
  int fetch_calls = 0;
  pc.RegisterHandler(CommandType::kFetchLogs, [&](const Command&, std::string* reason) {
    ++fetch_calls;
    *reason = "user-consent-missing";
    return false;
  });
  Command fetch;
  fetch.command_id = 43;
  fetch.type = CommandType::kFetchLogs;
  auto fenv = SignedCommand(fetch);
  EXPECT_FALSE(pc.ApplyEnvelope(fenv.data(), fenv.size()));
  EXPECT_EQ(fetch_calls, 1);
  EXPECT_TRUE(receipts.back().reason == "user-consent-missing");
  EXPECT_FALSE(receipts.back().ok);

  // 未注册的命令类型：明确回执，不静默吞
  Command unk;
  unk.command_id = 44;
  unk.type = CommandType::kUnmuteHealScene;
  auto uenv = SignedCommand(unk);
  EXPECT_FALSE(pc.ApplyEnvelope(uenv.data(), uenv.size()));
  EXPECT_TRUE(receipts.back().reason == "unsupported-command");
}

TEST(Policy, ForceSampleOverrideWithMandatoryTtl) {
  VirtualClock clk;
  PolicyClient pc(&clk, kKey);
  auto b = GoodBundle(3);
  b.trace_sample_permille = 10;  // 常态 1% 采样
  auto env = SignedPolicy(b);
  ASSERT_TRUE(pc.ApplyEnvelope(env.data(), env.size()));
  EXPECT_EQ(pc.EffectiveTracePermille(), 10u);

  // 故障排查：临时全采 30s（TTL 强制，不带 TTL 的全采指令被拒）
  Command bad_force;
  bad_force.command_id = 100;
  bad_force.type = CommandType::kForceTraceSample;
  bad_force.arg_a = 1000;
  bad_force.arg_b = 0;  // 无 TTL：拒绝
  auto benv = SignedCommand(bad_force);
  EXPECT_FALSE(pc.ApplyEnvelope(benv.data(), benv.size()));
  EXPECT_EQ(pc.EffectiveTracePermille(), 10u);

  Command force;
  force.command_id = 101;
  force.type = CommandType::kForceTraceSample;
  force.arg_a = 1000;
  force.arg_b = 30'000;
  auto fenv = SignedCommand(force);
  ASSERT_TRUE(pc.ApplyEnvelope(fenv.data(), fenv.size()));
  EXPECT_EQ(pc.EffectiveTracePermille(), 1000u);  // 覆盖生效

  clk.AdvanceMillis(30'000);
  pc.Tick();  // 覆盖到期：自动回落，无需服务端再发取消
  EXPECT_EQ(pc.EffectiveTracePermille(), 10u);
}

TEST(Policy, GarbageAndTruncationRejected) {
  VirtualClock clk;
  PolicyClient pc(&clk, kKey);
  const uint8_t junk[] = {0x03, 0xFF, 0x00, 0x12, 0x34};
  EXPECT_FALSE(pc.ApplyEnvelope(junk, sizeof(junk)));
  auto env = SignedPolicy(GoodBundle(2));
  EXPECT_FALSE(pc.ApplyEnvelope(env.data(), env.size() - 5));  // 截断
  EXPECT_TRUE(pc.stats().rejected_malformed >= 2);
  EXPECT_EQ(pc.effective().version, 0u);
}

TEST(Policy, Ed25519EnvelopeAndDowngradeGate) {
  if (!imobs::crypto::Available()) {
    std::printf("        [skip] 本构建无强加密提供方\n");
    return;
  }
  uint8_t pub[32];
  uint8_t priv[64];
  ASSERT_TRUE(imobs::crypto::Ed25519Keypair(pub, priv));

  VirtualClock clk;
  PolicyClient pc(&clk, kKey);
  pc.SetEd25519PublicKey(pub);

  // Ed25519 信封：正常接受。
  std::vector<uint8_t> payload;
  EncodePolicyBundle(GoodBundle(5), &payload);
  std::vector<uint8_t> env;
  ASSERT_TRUE(imobs::SignEnvelopeEd25519(kEnvelopePolicy, payload, priv, &env));
  EXPECT_TRUE(pc.ApplyEnvelope(env.data(), env.size()));
  EXPECT_EQ(pc.effective().version, 5u);

  // 篡改一字节：整包拒绝，原值保持。
  std::vector<uint8_t> payload6;
  EncodePolicyBundle(GoodBundle(6), &payload6);
  std::vector<uint8_t> env6;
  ASSERT_TRUE(imobs::SignEnvelopeEd25519(kEnvelopePolicy, payload6, priv, &env6));
  env6[env6.size() / 2] ^= 1;
  EXPECT_FALSE(pc.ApplyEnvelope(env6.data(), env6.size()));
  EXPECT_EQ(pc.effective().version, 5u);

  // 降级闸门：配置公钥后，HMAC 信封（哪怕密钥正确）一律拒收。
  auto hmac_env = SignedPolicy(GoodBundle(7));
  EXPECT_FALSE(pc.ApplyEnvelope(hmac_env.data(), hmac_env.size()));
  EXPECT_EQ(pc.effective().version, 5u);

  // 错钥拒收：另一对钥签的包。
  uint8_t pub2[32];
  uint8_t priv2[64];
  ASSERT_TRUE(imobs::crypto::Ed25519Keypair(pub2, priv2));
  std::vector<uint8_t> env8;
  std::vector<uint8_t> payload8;
  EncodePolicyBundle(GoodBundle(8), &payload8);
  ASSERT_TRUE(imobs::SignEnvelopeEd25519(kEnvelopePolicy, payload8, priv2, &env8));
  EXPECT_FALSE(pc.ApplyEnvelope(env8.data(), env8.size()));
  EXPECT_EQ(pc.effective().version, 5u);

  // 反向闸门：没配公钥的客户端收到 Ed25519 信封 → 拒收（验不了就拒，不放行）。
  PolicyClient pc_plain(&clk, kKey);
  EXPECT_FALSE(pc_plain.ApplyEnvelope(env.data(), env.size()));
  EXPECT_TRUE(pc_plain.stats().rejected_signature >= 1);
}

IMOBS_TEST_MAIN
