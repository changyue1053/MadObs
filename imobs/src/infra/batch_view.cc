#include "imobs/batch_view.h"

#include "imobs/codec.h"
#include "imobs/wire.h"

namespace imobs {
namespace {

constexpr uint64_t kMaxRecords = 100'000;
constexpr uint64_t kMaxRawLen = 64ull * 1024 * 1024;  // 解压炸弹护栏

// 记录帧区解析：两种批的共同尾段。
bool ParseFrames(const uint8_t* data, size_t len, uint64_t n,
                 std::vector<std::string_view>* out) {
  WireReader r(data, len);
  out->clear();
  out->reserve(n);
  for (uint64_t i = 0; i < n; ++i) {
    uint64_t rec_len = 0;
    const uint8_t* p = nullptr;
    if (!r.Varint(&rec_len) || !r.Bytes(&p, rec_len)) return false;
    out->emplace_back(reinterpret_cast<const char*>(p), rec_len);
  }
  return r.remaining() == 0;
}

}  // namespace

bool ParseBatchViewDetailed(const void* data, size_t len, BatchView* out, bool* resource_failure) {
  if(resource_failure) *resource_failure=false;
  WireReader r(data, len);
  const uint8_t* magic = nullptr;
  uint64_t n = 0;
  if (!r.Fixed64(&out->batch_id) || !r.Bytes(&magic, 4) || magic[0] != 'I' || magic[1] != 'M' ||
      magic[2] != 'B' || !r.U8(&out->pri) || !r.Fixed64(&out->anchor_wall_ms) ||
      !r.Fixed64(&out->anchor_mono_ns) || !r.Varint(&n) || n > kMaxRecords) {
    return false;
  }
  if (magic[3] == '1') {
    out->compressed = false;
    return ParseFrames(r.cursor(), r.remaining(), n, &out->records);
  }
  if (magic[3] != 'Z') return false;
  out->compressed = true;
  uint64_t raw_len = 0;
  if (!r.Varint(&raw_len) || raw_len == 0 || raw_len > kMaxRawLen) return false;
  const auto result=InflateBytesDetailed(r.cursor(),r.remaining(),raw_len,&out->owned);
  if(result!=CodecResult::kOk) {
    if(resource_failure) *resource_failure=result==CodecResult::kResourceFailure;
    return false;
  }
  return ParseFrames(out->owned.data(), out->owned.size(), n, &out->records);
}

bool ParseBatchView(const void* data,size_t len,BatchView* out) {
  return ParseBatchViewDetailed(data,len,out,nullptr);
}
}  // namespace imobs
