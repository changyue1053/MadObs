#pragma once
// 自包含 SHA-256 / HMAC-SHA256（FIPS 180-4 / RFC 2104）：控制面信封的完整性校验用。
// 正确性由 NIST / RFC 4231 标准向量守护（test_sha256）。
// 诚实边界：HMAC 是共享密钥 MAC——防传输篡改与运营手滑；对"攻击者已拿到端上
// 密钥"的威胁需非对称签名（Ed25519 随 P6 BoringSSL 接入，ISigner 接口已为此预留形状）。

#include <array>
#include <cstddef>
#include <cstdint>

namespace imobs {

using Sha256Digest = std::array<uint8_t, 32>;

Sha256Digest Sha256(const void* data, size_t len);
Sha256Digest HmacSha256(const void* key, size_t key_len, const void* data, size_t len);

// 常量时间比较：MAC 校验不允许早退出（旁路计时面）。
bool ConstantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n);

}  // namespace imobs
