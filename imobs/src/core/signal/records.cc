#include "imobs/records.h"

namespace imobs {
namespace {
constexpr uint64_t kMaxRows = 100'000;    // 解码防御上限
constexpr uint64_t kMaxEvents = 100'000;
constexpr uint64_t kMaxDigestLen = 1 << 20;
}  // namespace

void EncodeMetricsChunk(const std::vector<MetricsTable::Row>& rows, std::vector<uint8_t>* out) {  // audit-allow: cold-path 快照序列化（30s 批级，消费线程）
  WireWriter w(out);
  w.U8(kRecMetrics);
  w.Varint(rows.size());
  std::vector<uint8_t> dbuf;  // audit-allow: cold-path 快照序列化（30s 批级）
  for (const auto& r : rows) {
    const bool hist = r.kind == SchemaRegistry::MetricKind::kHistogram;
    w.U8(hist ? 1 : 0);
    w.Fixed64(r.key);
    w.Varint(r.label_idx);
    w.Varint(r.count);
    w.Varint(r.sum);
    if (hist) {
      dbuf.clear();
      r.digest.Serialize(&dbuf);
      w.Varint(dbuf.size());
      w.Bytes(dbuf.data(), dbuf.size());
    }
  }
}

bool DecodeMetricsChunk(const uint8_t* data, size_t len, std::vector<MetricsRowView>* out) {  // audit-allow: cold-path 解码运行于服务端/收集器作业，非端上热路径
  WireReader r(data, len);
  uint8_t tag = 0;
  uint64_t n = 0;
  if (!r.U8(&tag) || tag != kRecMetrics || !r.Varint(&n) || n > kMaxRows) return false;
  out->clear();
  out->reserve(n);
  for (uint64_t i = 0; i < n; ++i) {
    MetricsRowView row;
    uint64_t label = 0;
    if (!r.U8(&row.kind) || row.kind > 1 || !r.Fixed64(&row.key) || !r.Varint(&label) ||
        label > 0xFFFF || !r.Varint(&row.count) || !r.Varint(&row.sum)) {
      return false;
    }
    row.label_idx = static_cast<uint16_t>(label);
    if (row.kind == 1) {
      uint64_t dlen = 0;
      const uint8_t* dptr = nullptr;
      if (!r.Varint(&dlen) || dlen > kMaxDigestLen || !r.Bytes(&dptr, dlen)) return false;
      if (!TDigest::Deserialize(dptr, dlen, &row.digest)) return false;
    }
    out->push_back(std::move(row));
  }
  return r.remaining() == 0;
}

void EncodeTraceChunk(uint64_t base_mono_ns, const std::vector<TraceEventWire>& events,  // audit-allow: cold-path 轨迹块打包（批级，消费线程）
                      std::vector<uint8_t>* out) {  // audit-allow: cold-path 轨迹块打包（批级，消费线程）
  WireWriter w(out);
  w.U8(kRecTrace);
  w.Fixed64(base_mono_ns);
  w.Varint(events.size());
  for (const auto& ev : events) EncodeTraceEvent(ev, out);
}

bool DecodeTraceChunk(const uint8_t* data, size_t len, uint64_t* base_mono_ns,
                      std::vector<TraceEventWire>* out) {  // audit-allow: cold-path 解码运行于服务端/收集器作业
  WireReader r(data, len);
  uint8_t tag = 0;
  uint64_t n = 0;
  if (!r.U8(&tag) || tag != kRecTrace || !r.Fixed64(base_mono_ns) || !r.Varint(&n) ||
      n > kMaxEvents) {
    return false;
  }
  out->clear();
  out->reserve(n);
  for (uint64_t i = 0; i < n; ++i) {
    TraceEventWire ev;
    if (!DecodeTraceEvent(r, &ev)) return false;
    out->push_back(ev);
  }
  return r.remaining() == 0;
}

void EncodeEventRecord(const EventRecordWire& rec, std::vector<uint8_t>* out) {  // audit-allow: cold-path 结构化事件编码（消费线程；事件为低频信号）
  WireWriter w(out);
  w.U8(kRecEvent);
  w.U8(rec.level);
  w.Fixed64(rec.key);
  w.Fixed64(rec.ts_mono_ns);
  w.Varint(rec.a);
  w.Varint(rec.b);
}

bool DecodeEventRecord(const uint8_t* data, size_t len, EventRecordWire* out) {
  WireReader r(data, len);
  uint8_t tag = 0;
  if (!r.U8(&tag) || tag != kRecEvent || !r.U8(&out->level) || !r.Fixed64(&out->key) ||
      !r.Fixed64(&out->ts_mono_ns) || !r.Varint(&out->a) || !r.Varint(&out->b)) {
    return false;
  }
  return r.remaining() == 0;
}

void EncodeHealRecord(const HealRecordWire& rec, std::vector<uint8_t>* out) {  // audit-allow: cold-path 自愈事件编码（分钟级）
  WireWriter w(out);
  w.U8(kRecHeal);
  w.Fixed64(rec.scene_id);
  w.U8(rec.from);
  w.U8(rec.to);
  w.U8(rec.ladder_index);
  w.U8(rec.reason);
  w.Fixed64(rec.ts_ms);
  w.Varint(rec.action.size());
  w.Bytes(rec.action.data(), rec.action.size());
}

bool DecodeHealRecord(const uint8_t* data, size_t len, HealRecordWire* out) {
  WireReader r(data, len);
  uint8_t tag = 0;
  uint64_t alen = 0;
  const uint8_t* ap = nullptr;
  if (!r.U8(&tag) || tag != kRecHeal || !r.Fixed64(&out->scene_id) || !r.U8(&out->from) ||
      !r.U8(&out->to) || !r.U8(&out->ladder_index) || !r.U8(&out->reason) ||
      !r.Fixed64(&out->ts_ms) || !r.Varint(&alen) || alen > 256 || !r.Bytes(&ap, alen)) {
    return false;
  }
  out->action.assign(reinterpret_cast<const char*>(ap), alen);
  return r.remaining() == 0;
}

void EncodeLogChunk(uint64_t command_id, int64_t base_wall_ms,
                    const std::vector<LogEntryWire>& entries, std::vector<uint8_t>* out) {  // audit-allow: cold-path 回捞上行打包（指令级）
  WireWriter w(out);
  w.U8(kRecLogs);
  w.Fixed64(command_id);
  w.Fixed64(static_cast<uint64_t>(base_wall_ms));
  w.Varint(entries.size());
  for (const LogEntryWire& e : entries) {
    // 提取窗口 [start,end] 保证 wall ≥ base（base = 首条 wall）：delta 无符号安全。
    w.Varint(static_cast<uint64_t>(e.wall_ms - base_wall_ms));
    w.U8(e.level);
    w.Fixed64(e.module_hash);
    w.Varint(e.text.size());
    w.Bytes(e.text.data(), e.text.size());
  }
}

bool DecodeLogChunk(const uint8_t* data, size_t len, uint64_t* command_id,
                    std::vector<LogEntryWire>* out) {  // audit-allow: cold-path 解码运行于服务端作业
  WireReader r(data, len);
  uint8_t tag = 0;
  uint64_t base = 0;
  uint64_t n = 0;
  if (!r.U8(&tag) || tag != kRecLogs || !r.Fixed64(command_id) || !r.Fixed64(&base) ||
      !r.Varint(&n) || n > 100'000) {
    return false;
  }
  out->clear();
  out->reserve(n);
  for (uint64_t i = 0; i < n; ++i) {
    LogEntryWire e;
    uint64_t dwall = 0;
    uint64_t tlen = 0;
    const uint8_t* tp = nullptr;
    if (!r.Varint(&dwall) || !r.U8(&e.level) || !r.Fixed64(&e.module_hash) ||
        !r.Varint(&tlen) || tlen > 4096 || !r.Bytes(&tp, tlen)) {
      return false;
    }
    e.wall_ms = static_cast<int64_t>(base + dwall);
    e.text.assign(reinterpret_cast<const char*>(tp), tlen);
    out->push_back(std::move(e));
  }
  return r.remaining() == 0;
}

}  // namespace imobs
