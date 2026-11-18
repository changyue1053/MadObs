#pragma once
// 强加密提供方（架构 §12 安全）：Ed25519 签名/验签 + AES-256-GCM AEAD。
//
// 提供方选型：BoringSSL——与 IMLink/IMDB 三内核共用一套加密底座（imlink §11），
// 端上不再引第二套 crypto。构建期未链接 BoringSSL 时编入空实现
// （Available()=false），调用方回落 HMAC/明文路径：imobs 独立构建永远成立，
// 强加密随宿主产物自动点亮——依赖是注入的，不是绑死的。
//
// 线程契约：无状态纯函数，任意线程可用（非热路径：控制面验签 + 日志落盘）。

#include <cstddef>
#include <cstdint>
#include <vector>

namespace imobs::crypto {

// 本构建是否携带强加密提供方。false 时以下函数全部返回 false。
bool Available();

// —— Ed25519（RFC 8032）——
// priv 为 BoringSSL 私钥格式（64B = seed||pub）；keypair 供服务端/测试造钥。
bool Ed25519Keypair(uint8_t pub[32], uint8_t priv[64]);
bool Ed25519Sign(const uint8_t priv[64], const uint8_t* msg, size_t len, uint8_t sig[64]);
bool Ed25519Verify(const uint8_t pub[32], const uint8_t* msg, size_t len, const uint8_t sig[64]);

// —— AES-256-GCM ——
// out = ciphertext || 16B tag。nonce 恰 12B，同一 key 下绝不复用（调用方保证）。
bool AesGcmSeal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad,
                size_t aad_len, const uint8_t* plain, size_t plain_len,
                std::vector<uint8_t>* out);
// 输入 ciphertext||tag；认证失败（篡改/错钥/错 nonce/错 AAD）返回 false。
bool AesGcmOpen(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad,
                size_t aad_len, const uint8_t* sealed, size_t sealed_len,
                std::vector<uint8_t>* out);

inline constexpr size_t kGcmTagLen = 16;
inline constexpr size_t kGcmNonceLen = 12;

}  // namespace imobs::crypto
