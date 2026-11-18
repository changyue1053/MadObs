#include "imobs/sha256.h"

#include <cstring>
#include <vector>

namespace imobs {
namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Ctx {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
};

void Compress(Ctx& c, const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) | (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(p[i * 4 + 2]) << 8) | static_cast<uint32_t>(p[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
  uint32_t e = c.h[4], f = c.h[5], g = c.h[6], h = c.h[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = h + s1 + ch + kK[i] + w[i];
    const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    const uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = cc;
    cc = b;
    b = a;
    a = t1 + t2;
  }
  c.h[0] += a;
  c.h[1] += b;
  c.h[2] += cc;
  c.h[3] += d;
  c.h[4] += e;
  c.h[5] += f;
  c.h[6] += g;
  c.h[7] += h;
}

}  // namespace

Sha256Digest Sha256(const void* data, size_t len) {
  const auto* p = static_cast<const uint8_t*>(data);
  Ctx c;
  size_t off = 0;
  while (len - off >= 64) {
    Compress(c, p + off);
    off += 64;
  }
  // 末块填充：0x80 + 零 + 64bit 大端比特长度（可能占两个块）
  uint8_t tail[128] = {};
  const size_t rem = len - off;
  std::memcpy(tail, p + off, rem);
  tail[rem] = 0x80;
  const size_t total = rem >= 56 ? 128 : 64;
  const uint64_t bits = static_cast<uint64_t>(len) * 8;
  for (int i = 0; i < 8; ++i) {
    tail[total - 1 - i] = static_cast<uint8_t>(bits >> (i * 8));
  }
  Compress(c, tail);
  if (total == 128) Compress(c, tail + 64);

  Sha256Digest out;
  for (int i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<uint8_t>(c.h[i] >> 24);
    out[i * 4 + 1] = static_cast<uint8_t>(c.h[i] >> 16);
    out[i * 4 + 2] = static_cast<uint8_t>(c.h[i] >> 8);
    out[i * 4 + 3] = static_cast<uint8_t>(c.h[i]);
  }
  return out;
}

Sha256Digest HmacSha256(const void* key, size_t key_len, const void* data, size_t len) {
  uint8_t k0[64] = {};
  if (key_len > 64) {
    const Sha256Digest kh = Sha256(key, key_len);
    std::memcpy(k0, kh.data(), kh.size());
  } else {
    std::memcpy(k0, key, key_len);
  }
  uint8_t ipad[64];
  uint8_t opad[64];
  for (int i = 0; i < 64; ++i) {
    ipad[i] = k0[i] ^ 0x36;
    opad[i] = k0[i] ^ 0x5c;
  }
  // inner = H(ipad || data)：借尾块接口一次成型（数据量小，控制面不追求流式）
  std::vector<uint8_t> inner_msg(64 + len);
  std::memcpy(inner_msg.data(), ipad, 64);
  std::memcpy(inner_msg.data() + 64, data, len);
  const Sha256Digest inner = Sha256(inner_msg.data(), inner_msg.size());

  uint8_t outer_msg[64 + 32];
  std::memcpy(outer_msg, opad, 64);
  std::memcpy(outer_msg + 64, inner.data(), inner.size());
  return Sha256(outer_msg, sizeof(outer_msg));
}

bool ConstantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

}  // namespace imobs
