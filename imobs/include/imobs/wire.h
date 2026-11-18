#pragma once
// imobs 线格式 v1（实施计划 P1.4/P1.6）：varint(LEB128) + 小端 fixed64。
// 自包含实现（不依赖 protobuf 库），字段布局与 protobuf wire 兼容思想一致，
// 后续接入 proto 时按 schema 平移。
// 安全契约：WireReader 一切读取有界校验，任何越界/超长输入返回 false——
// 解码器是攻击面（回捞/下发通道的对端数据），fuzz 目标（test_wire）。

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "imobs/event.h"

namespace imobs {

inline uint64_t Fnv1a64(const void* data, size_t n) {
  const auto* p = static_cast<const uint8_t*>(data);
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

inline uint64_t DoubleBits(double v) {
  uint64_t b;
  std::memcpy(&b, &v, 8);
  return b;
}
inline double BitsDouble(uint64_t b) {
  double v;
  std::memcpy(&v, &b, 8);
  return v;
}

class WireWriter {
 public:
  explicit WireWriter(std::vector<uint8_t>* out) : out_(out) {}
  void U8(uint8_t v) { out_->push_back(v); }
  void Fixed64(uint64_t v) {
    for (int i = 0; i < 8; ++i) out_->push_back(static_cast<uint8_t>(v >> (i * 8)));
  }
  void Varint(uint64_t v) {
    while (v >= 0x80) {
      out_->push_back(static_cast<uint8_t>(v) | 0x80);
      v >>= 7;
    }
    out_->push_back(static_cast<uint8_t>(v));
  }
  void Bytes(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    out_->insert(out_->end(), b, b + n);
  }

 private:
  std::vector<uint8_t>* out_;
};

class WireReader {
 public:
  WireReader(const void* p, size_t n)
      : p_(static_cast<const uint8_t*>(p)), end_(static_cast<const uint8_t*>(p) + n) {}

  bool U8(uint8_t* v) {
    if (p_ >= end_) return false;
    *v = *p_++;
    return true;
  }
  bool Fixed64(uint64_t* v) {
    if (end_ - p_ < 8) return false;
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i) r |= static_cast<uint64_t>(p_[i]) << (i * 8);
    p_ += 8;
    *v = r;
    return true;
  }
  bool Varint(uint64_t* v) {
    uint64_t r = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (p_ >= end_) return false;
      const uint8_t b = *p_++;
      r |= static_cast<uint64_t>(b & 0x7F) << shift;
      if ((b & 0x80) == 0) {
        *v = r;
        return true;
      }
    }
    return false;  // 超过 10 字节：非法
  }
  bool Bytes(const uint8_t** p, size_t n) {
    if (static_cast<size_t>(end_ - p_) < n) return false;
    *p = p_;
    p_ += n;
    return true;
  }
  size_t remaining() const { return static_cast<size_t>(end_ - p_); }
  const uint8_t* cursor() const { return p_; }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
};

// —— MsgTrace 轨迹事件线格式：单事件典型 20~40B，契约上限 50B（test_wire 断言）——
// [fixed64 msg_id][u8 stage][u8 status][varint ts_delta_ns][varint n_attrs]
// ([varint tag][varint value]) × n
// ts 为批内 delta（批头携带 ClockAnchor，架构 §6 时钟锚定）。
struct TraceAttr {
  uint32_t tag = 0;
  uint64_t value = 0;
};

// attr tag 约定（契约文件的一部分，只增不改）
inline constexpr uint32_t kAttrConnLow = 1;   // attr_a：conn_id 低位
inline constexpr uint32_t kAttrExtra = 2;     // attr_b：场景自定义
inline constexpr uint32_t kAttrLabelIdx = 3;  // label 组合索引
inline constexpr uint32_t kAttrValue = 4;     // value 复用（seq 等）

struct TraceEventWire {
  uint64_t msg_id = 0;
  uint8_t stage = 0;
  uint8_t status = 0;
  uint64_t ts_delta_ns = 0;
  TraceAttr attrs[4];
  uint8_t n_attrs = 0;  // 定长上限 4：64B 事件槽本就只带这几个字段
};

void EncodeTraceEvent(const TraceEventWire& ev, std::vector<uint8_t>* out);
bool DecodeTraceEvent(WireReader& r, TraceEventWire* out);
TraceEventWire TraceFromEvent(const Event& ev, uint64_t base_ts_ns);

}  // namespace imobs
