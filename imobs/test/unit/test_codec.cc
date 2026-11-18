// 打包压缩编解码：往返、压不动回退、解压护栏（长度不符 / 损坏必拒）。
#include "imobs/codec.h"

#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "imobs_test.h"

using imobs::DeflateBytes;
using imobs::InflateBytes;

namespace {

TEST(Codec, RoundTripCompressible) {
  // 高重复 TLV 形态（真实批的主体特征）。
  std::string raw;
  for (int i = 0; i < 200; ++i) raw += "msg.send.total|tenant=42|chan=tcp|v=1;";
  std::vector<uint8_t> z;
  ASSERT_TRUE(DeflateBytes(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), &z));
  EXPECT_LT(z.size(), raw.size() / 4);  // 重复文本至少 4 倍压缩

  std::vector<uint8_t> back;
  ASSERT_TRUE(InflateBytes(z.data(), z.size(), raw.size(), &back));
  EXPECT_TRUE(std::memcmp(back.data(), raw.data(), raw.size()) == 0);
}

TEST(Codec, IncompressibleFallsBack) {
  // 随机字节压不小 → false（调用方按原文落批，格式回退 IMB1）。
  std::mt19937_64 rng(7);
  std::vector<uint8_t> raw(4096);
  for (auto& b : raw) b = static_cast<uint8_t>(rng());
  std::vector<uint8_t> z;
  EXPECT_FALSE(DeflateBytes(raw.data(), raw.size(), &z));
  std::vector<uint8_t> empty;
  EXPECT_FALSE(DeflateBytes(empty.data(), 0, &z));
}

TEST(Codec, InflateRejectsCorruptionAndWrongLength) {
  std::string raw(2000, 'a');
  std::vector<uint8_t> z;
  ASSERT_TRUE(DeflateBytes(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), &z));

  std::vector<uint8_t> back;
  EXPECT_FALSE(InflateBytes(z.data(), z.size(), raw.size() + 1, &back));  // 长度声明不符
  EXPECT_FALSE(InflateBytes(z.data(), z.size() / 2, raw.size(), &back));  // 截断
  std::vector<uint8_t> bad = z;
  bad[bad.size() / 2] ^= 0xFF;  // 中段翻转（adler32 校验兜底）
  EXPECT_FALSE(InflateBytes(bad.data(), bad.size(), raw.size(), &back));
}

}  // namespace

IMOBS_TEST_MAIN
