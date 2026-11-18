// 强加密提供方（BoringSSL 构建下运行；空提供方构建下打印跳过并通过——
// 跳过本身也是断言：Available()=false 时所有操作必须显式失败，不允许假成功）。
#include "imobs/crypto.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imobs_test.h"

namespace crypto = imobs::crypto;

namespace {

#define SKIP_WITHOUT_PROVIDER()                                        \
  do {                                                                 \
    if (!crypto::Available()) {                                        \
      std::printf("        [skip] 本构建无强加密提供方\n");            \
      return;                                                          \
    }                                                                  \
  } while (0)

TEST(Crypto, NullProviderFailsClosed) {
  if (crypto::Available()) return;  // 仅空提供方构建有意义
  uint8_t pub[32];
  uint8_t priv[64];
  uint8_t sig[64];
  std::vector<uint8_t> out;
  EXPECT_FALSE(crypto::Ed25519Keypair(pub, priv));
  EXPECT_FALSE(crypto::Ed25519Sign(priv, pub, 32, sig));
  EXPECT_FALSE(crypto::Ed25519Verify(pub, pub, 32, sig));
  uint8_t key[32] = {};
  uint8_t nonce[12] = {};
  EXPECT_FALSE(crypto::AesGcmSeal(key, nonce, nullptr, 0, pub, 32, &out));
  EXPECT_FALSE(crypto::AesGcmOpen(key, nonce, nullptr, 0, pub, 32, &out));
}

TEST(Crypto, Ed25519SignVerifyTamper) {
  SKIP_WITHOUT_PROVIDER();
  uint8_t pub[32];
  uint8_t priv[64];
  ASSERT_TRUE(crypto::Ed25519Keypair(pub, priv));

  const std::string msg = "policy-bundle-v42";
  uint8_t sig[64];
  ASSERT_TRUE(crypto::Ed25519Sign(priv, reinterpret_cast<const uint8_t*>(msg.data()),
                                  msg.size(), sig));
  EXPECT_TRUE(crypto::Ed25519Verify(pub, reinterpret_cast<const uint8_t*>(msg.data()),
                                    msg.size(), sig));

  uint8_t bad_sig[64];
  std::memcpy(bad_sig, sig, 64);
  bad_sig[10] ^= 1;
  EXPECT_FALSE(crypto::Ed25519Verify(pub, reinterpret_cast<const uint8_t*>(msg.data()),
                                     msg.size(), bad_sig));

  const std::string msg2 = "policy-bundle-v43";
  EXPECT_FALSE(crypto::Ed25519Verify(pub, reinterpret_cast<const uint8_t*>(msg2.data()),
                                     msg2.size(), sig));
}

TEST(Crypto, AesGcmRoundTripAndAuth) {
  SKIP_WITHOUT_PROVIDER();
  uint8_t key[32];
  for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i * 7 + 1);
  uint8_t nonce[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  const std::string plain = "2026-08-30 conn reset uid=42 途中含中文与\x01二进制";
  const uint8_t aad[1] = {0x01};

  std::vector<uint8_t> sealed;
  ASSERT_TRUE(crypto::AesGcmSeal(key, nonce, aad, 1,
                                 reinterpret_cast<const uint8_t*>(plain.data()), plain.size(),
                                 &sealed));
  EXPECT_EQ(sealed.size(), plain.size() + crypto::kGcmTagLen);
  // 密文不含明文子串（起码的语义检查）。
  EXPECT_TRUE(std::string(reinterpret_cast<char*>(sealed.data()), sealed.size())
                  .find("conn reset") == std::string::npos);

  std::vector<uint8_t> back;
  ASSERT_TRUE(crypto::AesGcmOpen(key, nonce, aad, 1, sealed.data(), sealed.size(), &back));
  ASSERT_EQ(back.size(), plain.size());
  EXPECT_TRUE(std::memcmp(back.data(), plain.data(), plain.size()) == 0);

  // 篡改密文 / 错钥 / 错 AAD / 错 nonce → 全部认证失败。
  std::vector<uint8_t> bad = sealed;
  bad[3] ^= 0xFF;
  EXPECT_FALSE(crypto::AesGcmOpen(key, nonce, aad, 1, bad.data(), bad.size(), &back));
  uint8_t key2[32];
  std::memcpy(key2, key, 32);
  key2[0] ^= 1;
  EXPECT_FALSE(crypto::AesGcmOpen(key2, nonce, aad, 1, sealed.data(), sealed.size(), &back));
  const uint8_t aad2[1] = {0x02};
  EXPECT_FALSE(crypto::AesGcmOpen(key, nonce, aad2, 1, sealed.data(), sealed.size(), &back));
  uint8_t nonce2[12];
  std::memcpy(nonce2, nonce, 12);
  nonce2[0] ^= 1;
  EXPECT_FALSE(crypto::AesGcmOpen(key, nonce2, aad, 1, sealed.data(), sealed.size(), &back));
}

}  // namespace

IMOBS_TEST_MAIN
