#include "imobs/codec.h"

#include <zlib.h>

namespace imobs {

CodecResult DeflateBytesDetailed(const uint8_t* data, size_t len, std::vector<uint8_t>* out) {
  if (len == 0) return CodecResult::kMalformed;
  uLongf cap = compressBound(static_cast<uLong>(len));
  out->resize(cap);
  // Z_BEST_SPEED：端上 CPU 优先于压缩率——批主体是高重复 TLV，1 档已够肥。
  const int rc = compress2(out->data(), &cap, data, static_cast<uLong>(len), Z_BEST_SPEED);
  if (rc == Z_MEM_ERROR) return CodecResult::kResourceFailure;
  if (rc != Z_OK) return CodecResult::kMalformed;
  if (cap >= len) return CodecResult::kIncompressible;
  out->resize(cap);
  return CodecResult::kOk;
}

CodecResult InflateBytesDetailed(const uint8_t* data, size_t len, size_t raw_len, std::vector<uint8_t>* out) {
  if (raw_len == 0 || len == 0) return CodecResult::kMalformed;
  out->resize(raw_len);
  uLongf got = static_cast<uLongf>(raw_len);
  const int rc = uncompress(out->data(), &got, data, static_cast<uLong>(len));
  if (rc == Z_MEM_ERROR) return CodecResult::kResourceFailure;
  return rc == Z_OK && got == raw_len ? CodecResult::kOk : CodecResult::kMalformed;  // 长度必须严格吻合：截断/膨胀都算损坏
}

bool DeflateBytes(const uint8_t* data,size_t len,std::vector<uint8_t>* out) {
  return DeflateBytesDetailed(data,len,out)==CodecResult::kOk;
}
bool InflateBytes(const uint8_t* data,size_t len,size_t raw_len,std::vector<uint8_t>* out) {
  return InflateBytesDetailed(data,len,raw_len,out)==CodecResult::kOk;
}
}  // namespace imobs
