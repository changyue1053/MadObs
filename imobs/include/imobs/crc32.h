#pragma once
// CRC-32C（Castagnoli）软件实现：WAL 记录级校验用。
// 表在编译期生成；吞吐足够（WAL 追加在消费线程，非热路径）。

#include <cstddef>
#include <cstdint>

namespace imobs {

namespace detail {
struct Crc32cTable {
  uint32_t t[256] = {};
};

constexpr Crc32cTable MakeCrc32cTable() {
  Crc32cTable tb{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
    }
    tb.t[i] = c;
  }
  return tb;
}

inline constexpr Crc32cTable kCrc32c = MakeCrc32cTable();
}  // namespace detail

inline uint32_t Crc32c(const void* data, size_t n, uint32_t seed = 0) {
  const auto* p = static_cast<const uint8_t*>(data);
  uint32_t c = ~seed;
  for (size_t i = 0; i < n; ++i) {
    c = detail::kCrc32c.t[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
  }
  return ~c;
}

}  // namespace imobs
