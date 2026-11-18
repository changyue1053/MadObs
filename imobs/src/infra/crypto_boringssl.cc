// BoringSSL 提供方：EVP_AEAD 单趟接口 + ED25519_*（均为 misuse-resistant 设计）。
#include <openssl/aead.h>
#include <openssl/curve25519.h>

#include "imobs/crypto.h"

namespace imobs::crypto {

bool Available() { return true; }

bool Ed25519Keypair(uint8_t pub[32], uint8_t priv[64]) {
  ED25519_keypair(pub, priv);
  return true;
}

bool Ed25519Sign(const uint8_t priv[64], const uint8_t* msg, size_t len, uint8_t sig[64]) {
  return ED25519_sign(sig, msg, len, priv) == 1;
}

bool Ed25519Verify(const uint8_t pub[32], const uint8_t* msg, size_t len,
                   const uint8_t sig[64]) {
  return ED25519_verify(msg, len, sig, pub) == 1;
}

namespace {

const EVP_AEAD* Aead() { return EVP_aead_aes_256_gcm(); }

}  // namespace

bool AesGcmSeal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad,
                size_t aad_len, const uint8_t* plain, size_t plain_len,
                std::vector<uint8_t>* out) {
  EVP_AEAD_CTX ctx;
  if (EVP_AEAD_CTX_init(&ctx, Aead(), key, 32, kGcmTagLen, nullptr) != 1) return false;
  out->resize(plain_len + kGcmTagLen);
  size_t out_len = 0;
  const int rc = EVP_AEAD_CTX_seal(&ctx, out->data(), &out_len, out->size(), nonce,
                                   kGcmNonceLen, plain, plain_len, aad, aad_len);
  EVP_AEAD_CTX_cleanup(&ctx);
  if (rc != 1) return false;
  out->resize(out_len);
  return true;
}

bool AesGcmOpen(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad,
                size_t aad_len, const uint8_t* sealed, size_t sealed_len,
                std::vector<uint8_t>* out) {
  if (sealed_len < kGcmTagLen) return false;
  EVP_AEAD_CTX ctx;
  if (EVP_AEAD_CTX_init(&ctx, Aead(), key, 32, kGcmTagLen, nullptr) != 1) return false;
  out->resize(sealed_len);
  size_t out_len = 0;
  const int rc = EVP_AEAD_CTX_open(&ctx, out->data(), &out_len, out->size(), nonce,
                                   kGcmNonceLen, sealed, sealed_len, aad, aad_len);
  EVP_AEAD_CTX_cleanup(&ctx);
  if (rc != 1) return false;
  out->resize(out_len);
  return true;
}

}  // namespace imobs::crypto
