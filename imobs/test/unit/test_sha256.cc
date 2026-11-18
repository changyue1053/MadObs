// SHA-256 / HMAC-SHA256 标准向量对拍：自实现密码学基元的唯一可信验证方式。
#include "imobs/sha256.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imobs_test.h"

namespace {

std::string Hex(const imobs::Sha256Digest& d) {
  char buf[65];
  for (size_t i = 0; i < d.size(); ++i) std::snprintf(buf + i * 2, 3, "%02x", d[i]);
  return std::string(buf, 64);
}

}  // namespace

TEST(Sha256, NistVectors) {
  // FIPS 180-4 标准向量
  EXPECT_TRUE(Hex(imobs::Sha256("", 0)) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_TRUE(Hex(imobs::Sha256("abc", 3)) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const char* two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  EXPECT_TRUE(Hex(imobs::Sha256(two_block, std::strlen(two_block))) ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  // 填充边界（55/56/64 字节是 padding 逻辑的三个临界点）
  const std::string a55(55, 'a');
  const std::string a56(56, 'a');
  const std::string a64(64, 'a');
  EXPECT_TRUE(Hex(imobs::Sha256(a55.data(), 55)) ==
              "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  EXPECT_TRUE(Hex(imobs::Sha256(a56.data(), 56)) ==
              "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
  EXPECT_TRUE(Hex(imobs::Sha256(a64.data(), 64)) ==
              "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
  // 百万个 'a'（NIST 长向量）
  const std::string mil(1'000'000, 'a');
  EXPECT_TRUE(Hex(imobs::Sha256(mil.data(), mil.size())) ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, HmacRfc4231Vectors) {
  // Case 1: key = 0x0b × 20, data = "Hi There"
  const std::string k1(20, '\x0b');
  EXPECT_TRUE(Hex(imobs::HmacSha256(k1.data(), k1.size(), "Hi There", 8)) ==
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  // Case 2: key = "Jefe"
  const char* d2 = "what do ya want for nothing?";
  EXPECT_TRUE(Hex(imobs::HmacSha256("Jefe", 4, d2, std::strlen(d2))) ==
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  // Case 3: key = 0xaa × 20, data = 0xdd × 50
  const std::string k3(20, '\xaa');
  const std::string d3(50, '\xdd');
  EXPECT_TRUE(Hex(imobs::HmacSha256(k3.data(), k3.size(), d3.data(), d3.size())) ==
              "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
  // Case 6: 超长 key（>64B，先哈希）
  const std::string k6(131, '\xaa');
  const char* d6 = "Test Using Larger Than Block-Size Key - Hash Key First";
  EXPECT_TRUE(Hex(imobs::HmacSha256(k6.data(), k6.size(), d6, std::strlen(d6))) ==
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST(Sha256, ConstantTimeEqual) {
  const imobs::Sha256Digest a = imobs::Sha256("x", 1);
  imobs::Sha256Digest b = a;
  EXPECT_TRUE(imobs::ConstantTimeEqual(a.data(), b.data(), a.size()));
  b[31] ^= 1;
  EXPECT_FALSE(imobs::ConstantTimeEqual(a.data(), b.data(), a.size()));
  b = a;
  b[0] ^= 0x80;
  EXPECT_FALSE(imobs::ConstantTimeEqual(a.data(), b.data(), a.size()));
}

IMOBS_TEST_MAIN
