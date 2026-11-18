#pragma once
// 控制面客户端（实施计划 P3，架构 §3 控制流）：策略下发 + 命令通道。
//
// 控制流的工程标准比数据流更高，因为它有能力伤害端：
//   1. 信封必须验签（HMAC-SHA256，Ed25519 随 P6 升级）——篡改整包拒绝；
//   2. 验签防篡改、防不了运营手滑（IMLink §19 勘误的教训）：参数做
//      范围 + 关系组合校验，坏组合整包拒绝并记账，原值保持；
//   3. 版本单调递增才接受（回滚 = 用更高版本下发旧内容）——防重放降级；
//   4. TTL 到期回落编译期默认值——没有配置服务也能正常工作，
//      临时策略（如全采）绝不允许变成永久遗留；
//   5. 命令按 command_id 幂等，每条命令必有回执（结果 + 原因）。
//
// 线程契约：全部方法仅消费线程调用（下发字节由通道投递到消费线程后 Apply）。

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "imobs/clock.h"

namespace imobs {

inline constexpr uint8_t kEnvelopePolicy = 1;
inline constexpr uint8_t kEnvelopeCommand = 2;
// kind 高位 = Ed25519 信封标志：尾部 64B Ed25519 签名替代 32B HMAC。
// 客户端一旦配置了 Ed25519 公钥，HMAC 信封一律拒收——升级是单向闸门，防降级攻击。
inline constexpr uint8_t kEnvelopeEd25519Flag = 0x80;

// v1 策略面：采样、上报调度、自愈总开关（字段只增不删，版本化演进）。
struct PolicyBundle {
  uint64_t version = 0;   // 单调递增；0 = 编译期默认
  uint64_t ttl_ms = 0;    // 0 = 长期有效；否则到期回落默认值
  uint32_t trace_sample_permille = 1000;      // 轨迹采样千分比（常态全量）
  uint32_t rpc_trace_sample_permille = 10;    // RPC trace 常规采样
  uint64_t day_quota_bytes = 1'000'000;
  uint64_t p1_flush_interval_ms = 30'000;
  bool healer_enabled = true;                 // 自愈总开关（kill switch 的粗粒度档）
};

enum class CommandType : uint8_t {
  kFetchLogs = 1,        // arg_a=起始 wall_ms, arg_b=结束 wall_ms, arg_s=模块过滤
  kMuteHealScene = 2,    // arg_a=scene_id, arg_b=ttl_ms
  kUnmuteHealScene = 3,  // arg_a=scene_id
  kForceTraceSample = 4, // arg_a=permille, arg_b=ttl_ms（临时全采/降采，TTL 强制）
};

struct Command {
  uint64_t command_id = 0;
  CommandType type = CommandType::kFetchLogs;
  uint64_t arg_a = 0;
  uint64_t arg_b = 0;
  std::string arg_s;
};

// —— 编解码与签名（服务端下发侧与测试复用同一实现）——
void EncodePolicyBundle(const PolicyBundle& b, std::vector<uint8_t>* out);
bool DecodePolicyBundle(const uint8_t* data, size_t len, PolicyBundle* out);
void EncodeCommand(const Command& c, std::vector<uint8_t>* out);
bool DecodeCommand(const uint8_t* data, size_t len, Command* out);
// 信封：[u8 kind][varint payload_len][payload][32B HMAC-SHA256(key, kind||payload)]
void SignEnvelope(uint8_t kind, const std::vector<uint8_t>& payload, std::string_view key,
                  std::vector<uint8_t>* out);
// Ed25519 信封：[u8 kind|0x80][varint payload_len][payload][64B Ed25519(kind||payload)]。
// false = 本构建无强加密提供方（crypto::Available()==false）。
bool SignEnvelopeEd25519(uint8_t kind, const std::vector<uint8_t>& payload,
                         const uint8_t priv[64], std::vector<uint8_t>* out);

class PolicyClient {
 public:
  struct Receipt {
    uint64_t command_id = 0;
    bool ok = false;
    std::string reason;
  };
  // 返回 false + reason = 执行失败（回执带原因上送）。
  using CommandHandler = std::function<bool(const Command& cmd, std::string* reason)>;
  using ReceiptSink = std::function<void(const Receipt&)>;

  struct Stats {
    uint64_t applied = 0;
    uint64_t rejected_signature = 0;
    uint64_t rejected_version = 0;
    uint64_t rejected_validation = 0;
    uint64_t rejected_malformed = 0;
    uint64_t ttl_reverts = 0;
    uint64_t commands_executed = 0;
    uint64_t commands_deduped = 0;
    uint64_t commands_failed = 0;
  };

  PolicyClient(IClock* clock, std::string hmac_key, PolicyBundle defaults = {});

  // 配置 Ed25519 公钥（单向闸门）：此后仅接受 Ed25519 信封，HMAC 一律拒收。
  void SetEd25519PublicKey(const uint8_t pub[32]);

  void RegisterHandler(CommandType type, CommandHandler handler);
  void SetReceiptSink(ReceiptSink sink) { receipt_sink_ = std::move(sink); }

  // 下发入口（任意来源的字节：长连推送 / HTTP 拉取）。false = 整包被拒（细分见 stats）。
  bool ApplyEnvelope(const uint8_t* data, size_t len);

  // 周期驱动：策略 TTL 回落、采样覆盖到期。
  void Tick();

  const PolicyBundle& effective() const { return effective_; }
  uint32_t EffectiveTracePermille() const;  // 命令覆盖 > 策略包 > 默认

  // 坏配置防御（暴露为静态口径，服务端下发前可用同一实现预检）。
  static bool ValidateBundle(const PolicyBundle& b, uint64_t current_version, std::string* why);

  Stats stats() const { return stats_; }

 private:
  uint64_t MonoMs() const { return clock_->MonoNanos() / 1'000'000ull; }
  bool ApplyPolicy(const uint8_t* payload, size_t len);
  bool ApplyCommand(const uint8_t* payload, size_t len);
  void Emit(const Receipt& r) {
    if (receipt_sink_) receipt_sink_(r);
  }
  bool SeenCommand(uint64_t id);

  IClock* clock_;
  std::string key_;
  std::array<uint8_t, 32> ed_pub_{};
  bool ed_pub_set_ = false;
  PolicyBundle defaults_;
  PolicyBundle effective_;
  uint64_t bundle_expiry_ms_ = 0;  // 0 = 不过期
  uint32_t force_permille_ = 0;
  uint64_t force_until_ms_ = 0;

  std::array<uint64_t, 128> recent_cmds_{};  // 幂等窗口（v1 内存态；持久化随 v1.1）
  size_t recent_pos_ = 0;

  std::function<bool(const Command&, std::string*)> handlers_[8];
  ReceiptSink receipt_sink_;
  Stats stats_;
};

}  // namespace imobs
