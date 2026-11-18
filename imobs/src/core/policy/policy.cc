#include "imobs/policy.h"

#include <algorithm>
#include <cstring>

#include "imobs/crypto.h"
#include "imobs/sha256.h"
#include "imobs/wire.h"

namespace imobs {
namespace {

constexpr size_t kMacLen = 32;
constexpr size_t kEdSigLen = 64;
constexpr uint64_t kQuotaMin = 4096;
constexpr uint64_t kQuotaMax = 100ull * 1024 * 1024;
constexpr uint64_t kFlushMin = 1'000;
constexpr uint64_t kFlushMax = 600'000;
constexpr uint64_t kTtlMin = 60'000;  // 短命策略是抖动之源：TTL 要么 0 要么 ≥1min

}  // namespace

void EncodePolicyBundle(const PolicyBundle& b, std::vector<uint8_t>* out) {
  WireWriter w(out);
  w.Fixed64(b.version);
  w.Fixed64(b.ttl_ms);
  w.Varint(b.trace_sample_permille);
  w.Varint(b.rpc_trace_sample_permille);
  w.Varint(b.day_quota_bytes);
  w.Varint(b.p1_flush_interval_ms);
  w.U8(b.healer_enabled ? 1 : 0);
}

bool DecodePolicyBundle(const uint8_t* data, size_t len, PolicyBundle* out) {
  WireReader r(data, len);
  uint64_t tsp = 0;
  uint64_t rtsp = 0;
  uint8_t healer = 0;
  if (!r.Fixed64(&out->version) || !r.Fixed64(&out->ttl_ms) || !r.Varint(&tsp) ||
      !r.Varint(&rtsp) || !r.Varint(&out->day_quota_bytes) ||
      !r.Varint(&out->p1_flush_interval_ms) || !r.U8(&healer) || healer > 1 ||
      tsp > 0xFFFFFFFF || rtsp > 0xFFFFFFFF) {
    return false;
  }
  out->trace_sample_permille = static_cast<uint32_t>(tsp);
  out->rpc_trace_sample_permille = static_cast<uint32_t>(rtsp);
  out->healer_enabled = healer == 1;
  return r.remaining() == 0;
}

void EncodeCommand(const Command& c, std::vector<uint8_t>* out) {
  WireWriter w(out);
  w.Fixed64(c.command_id);
  w.U8(static_cast<uint8_t>(c.type));
  w.Fixed64(c.arg_a);
  w.Fixed64(c.arg_b);
  w.Varint(c.arg_s.size());
  w.Bytes(c.arg_s.data(), c.arg_s.size());
}

bool DecodeCommand(const uint8_t* data, size_t len, Command* out) {
  WireReader r(data, len);
  uint8_t type = 0;
  uint64_t slen = 0;
  const uint8_t* sp = nullptr;
  if (!r.Fixed64(&out->command_id) || !r.U8(&type) || !r.Fixed64(&out->arg_a) ||
      !r.Fixed64(&out->arg_b) || !r.Varint(&slen) || slen > 4096 || !r.Bytes(&sp, slen)) {
    return false;
  }
  out->type = static_cast<CommandType>(type);
  out->arg_s.assign(reinterpret_cast<const char*>(sp), slen);
  return r.remaining() == 0;
}

void SignEnvelope(uint8_t kind, const std::vector<uint8_t>& payload, std::string_view key,
                  std::vector<uint8_t>* out) {
  WireWriter w(out);
  w.U8(kind);
  w.Varint(payload.size());
  w.Bytes(payload.data(), payload.size());
  // MAC 覆盖 kind || payload：kind 参与签名，防"命令包改标成策略包"类别混淆
  std::vector<uint8_t> macced;
  macced.resize(1 + payload.size());
  macced[0] = kind;
  std::copy(payload.begin(), payload.end(), macced.begin() + 1);
  const Sha256Digest mac = HmacSha256(key.data(), key.size(), macced.data(), macced.size());
  w.Bytes(mac.data(), mac.size());
}

bool SignEnvelopeEd25519(uint8_t kind, const std::vector<uint8_t>& payload,
                         const uint8_t priv[64], std::vector<uint8_t>* out) {
  if (!crypto::Available()) return false;
  const uint8_t wire_kind = static_cast<uint8_t>(kind | kEnvelopeEd25519Flag);
  // 签名覆盖 wire_kind || payload：标志位参与签名——"降级重标"也算篡改。
  std::vector<uint8_t> signed_bytes;
  signed_bytes.resize(1 + payload.size());
  signed_bytes[0] = wire_kind;
  std::copy(payload.begin(), payload.end(), signed_bytes.begin() + 1);
  uint8_t sig[kEdSigLen];
  if (!crypto::Ed25519Sign(priv, signed_bytes.data(), signed_bytes.size(), sig)) return false;
  WireWriter w(out);
  w.U8(wire_kind);
  w.Varint(payload.size());
  w.Bytes(payload.data(), payload.size());
  w.Bytes(sig, kEdSigLen);
  return true;
}

PolicyClient::PolicyClient(IClock* clock, std::string hmac_key, PolicyBundle defaults)
    : clock_(clock), key_(std::move(hmac_key)), defaults_(defaults), effective_(defaults) {}

void PolicyClient::SetEd25519PublicKey(const uint8_t pub[32]) {
  std::copy(pub, pub + 32, ed_pub_.begin());
  ed_pub_set_ = true;
}

void PolicyClient::RegisterHandler(CommandType type, CommandHandler handler) {
  const auto idx = static_cast<size_t>(type);
  if (idx < 8) handlers_[idx] = std::move(handler);
}

bool PolicyClient::ValidateBundle(const PolicyBundle& b, uint64_t current_version,
                                  std::string* why) {
  const auto fail = [why](const char* reason) {
    if (why) *why = reason;
    return false;
  };
  if (b.version <= current_version) return fail("version-stale");
  if (b.trace_sample_permille > 1000) return fail("trace-sample-range");
  if (b.rpc_trace_sample_permille > 1000) return fail("rpc-sample-range");
  // 关系检查：RPC 放大镜不允许比轨迹主键更全（采样体系的层级约束）
  if (b.rpc_trace_sample_permille > b.trace_sample_permille) return fail("rpc-exceeds-trace");
  if (b.day_quota_bytes < kQuotaMin || b.day_quota_bytes > kQuotaMax) return fail("quota-range");
  if (b.p1_flush_interval_ms < kFlushMin || b.p1_flush_interval_ms > kFlushMax) {
    return fail("flush-range");
  }
  if (b.ttl_ms != 0 && b.ttl_ms < kTtlMin) return fail("ttl-too-short");
  return true;
}

bool PolicyClient::ApplyEnvelope(const uint8_t* data, size_t len) {
  WireReader r(data, len);
  uint8_t wire_kind = 0;
  uint64_t plen = 0;
  const uint8_t* payload = nullptr;
  if (!r.U8(&wire_kind)) {
    ++stats_.rejected_malformed;
    return false;
  }
  const bool ed = (wire_kind & kEnvelopeEd25519Flag) != 0;
  const uint8_t kind = static_cast<uint8_t>(wire_kind & ~kEnvelopeEd25519Flag);
  const size_t sig_len = ed ? kEdSigLen : kMacLen;
  const uint8_t* sig = nullptr;
  if ((kind != kEnvelopePolicy && kind != kEnvelopeCommand) || !r.Varint(&plen) ||
      plen > 64 * 1024 || !r.Bytes(&payload, plen) || !r.Bytes(&sig, sig_len) ||
      r.remaining() != 0) {
    ++stats_.rejected_malformed;
    return false;
  }

  // 单向闸门：配置了 Ed25519 公钥后 HMAC 信封拒收（防降级）；反向（收到 Ed25519
  // 信封但本端没有公钥/没有提供方）同样拒收——不存在"验不了就放行"。
  if (ed != ed_pub_set_) {
    ++stats_.rejected_signature;
    return false;
  }

  // 签名/MAC 均覆盖 wire_kind || payload：标志位与类别都参与，防类别混淆与降级重标。
  std::vector<uint8_t> signed_bytes;
  signed_bytes.resize(1 + plen);
  signed_bytes[0] = wire_kind;
  std::copy(payload, payload + plen, signed_bytes.begin() + 1);
  if (ed) {
    if (!crypto::Ed25519Verify(ed_pub_.data(), signed_bytes.data(), signed_bytes.size(), sig)) {
      ++stats_.rejected_signature;
      return false;
    }
  } else {
    const Sha256Digest expect =
        HmacSha256(key_.data(), key_.size(), signed_bytes.data(), signed_bytes.size());
    if (!ConstantTimeEqual(expect.data(), sig, kMacLen)) {
      ++stats_.rejected_signature;  // 篡改/密钥不符：整包拒绝，原值保持
      return false;
    }
  }
  return kind == kEnvelopePolicy ? ApplyPolicy(payload, plen) : ApplyCommand(payload, plen);
}

bool PolicyClient::ApplyPolicy(const uint8_t* payload, size_t len) {
  PolicyBundle b;
  if (!DecodePolicyBundle(payload, len, &b)) {
    ++stats_.rejected_malformed;
    return false;
  }
  std::string why;
  if (!ValidateBundle(b, effective_.version, &why)) {
    if (why == "version-stale") {
      ++stats_.rejected_version;  // 重放/降级：版本必须单调前进
    } else {
      ++stats_.rejected_validation;  // 验签防篡改，组合校验防手滑
    }
    return false;
  }
  effective_ = b;
  bundle_expiry_ms_ = b.ttl_ms == 0 ? 0 : MonoMs() + b.ttl_ms;
  ++stats_.applied;
  return true;
}

bool PolicyClient::SeenCommand(uint64_t id) {
  for (const uint64_t v : recent_cmds_) {
    if (v == id) return true;
  }
  recent_cmds_[recent_pos_] = id;
  recent_pos_ = (recent_pos_ + 1) % recent_cmds_.size();
  return false;
}

bool PolicyClient::ApplyCommand(const uint8_t* payload, size_t len) {
  Command cmd;
  if (!DecodeCommand(payload, len, &cmd) || cmd.command_id == 0) {
    ++stats_.rejected_malformed;
    return false;
  }
  if (SeenCommand(cmd.command_id)) {
    ++stats_.commands_deduped;  // 幂等：重复指令只执行一次，但回执照发（服务端对账）
    Emit({cmd.command_id, true, "duplicate-ignored"});
    return true;
  }

  // 内置命令：临时采样覆盖（TTL 强制——临时策略绝不允许变成永久遗留）
  if (cmd.type == CommandType::kForceTraceSample) {
    if (cmd.arg_a > 1000 || cmd.arg_b < 1'000) {
      ++stats_.commands_failed;
      Emit({cmd.command_id, false, "force-sample-args"});
      return false;
    }
    force_permille_ = static_cast<uint32_t>(cmd.arg_a);
    force_until_ms_ = MonoMs() + cmd.arg_b;
    ++stats_.commands_executed;
    Emit({cmd.command_id, true, "applied"});
    return true;
  }

  const auto idx = static_cast<size_t>(cmd.type);
  if (idx >= 8 || !handlers_[idx]) {
    ++stats_.commands_failed;
    Emit({cmd.command_id, false, "unsupported-command"});
    return false;
  }
  std::string reason = "ok";
  const bool ok = handlers_[idx](cmd, &reason);
  if (ok) {
    ++stats_.commands_executed;
  } else {
    ++stats_.commands_failed;
  }
  Emit({cmd.command_id, ok, reason});
  return ok;
}

void PolicyClient::Tick() {
  const uint64_t now = MonoMs();
  if (bundle_expiry_ms_ != 0 && now >= bundle_expiry_ms_) {
    effective_ = defaults_;
    effective_.version = 0;  // 回落默认后从零起版：下一份任何正版本都可接受
    bundle_expiry_ms_ = 0;
    ++stats_.ttl_reverts;
  }
  if (force_until_ms_ != 0 && now >= force_until_ms_) {
    force_until_ms_ = 0;  // 覆盖到期：自动回落，无需服务端再发"取消"
  }
}

uint32_t PolicyClient::EffectiveTracePermille() const {
  if (force_until_ms_ != 0 && clock_->MonoNanos() / 1'000'000ull < force_until_ms_) {
    return force_permille_;
  }
  return effective_.trace_sample_permille;
}

}  // namespace imobs
