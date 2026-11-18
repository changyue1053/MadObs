// 空提供方：未链接 BoringSSL 的独立构建。所有强加密操作显式不可用——
// 调用方（策略验签/日志加密）据此回落 HMAC/明文或 fail-closed，绝不静默降级。
#include "imobs/crypto.h"

namespace imobs::crypto {

bool Available() { return false; }
bool Ed25519Keypair(uint8_t*, uint8_t*) { return false; }
bool Ed25519Sign(const uint8_t*, const uint8_t*, size_t, uint8_t*) { return false; }
bool Ed25519Verify(const uint8_t*, const uint8_t*, size_t, const uint8_t*) { return false; }
bool AesGcmSeal(const uint8_t*, const uint8_t*, const uint8_t*, size_t, const uint8_t*, size_t,
                std::vector<uint8_t>*) {
  return false;
}
bool AesGcmOpen(const uint8_t*, const uint8_t*, const uint8_t*, size_t, const uint8_t*, size_t,
                std::vector<uint8_t>*) {
  return false;
}

}  // namespace imobs::crypto
