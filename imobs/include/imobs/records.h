#pragma once
// 端云共识的记录编码（上报 schema 冻结件 P1.6 的工程底座）：
// pipeline 记录首字节为类型标——'M' 指标快照块 / 'T' 轨迹块 / 'E' 结构化事件。
// 编码在端侧（本内核），解码在服务端拼接/聚合作业——但解码器同库交付并 fuzz：
// 它是数据面的攻击面，安全标准与 wire.h 相同（有界读、拒绝而非截断）。

#include <cstddef>
#include <cstdint>
#include <vector>

#include <string>

#include "imobs/metrics.h"
#include "imobs/tdigest.h"
#include "imobs/wire.h"

namespace imobs {

inline constexpr uint8_t kRecMetrics = 'M';
inline constexpr uint8_t kRecTrace = 'T';
inline constexpr uint8_t kRecEvent = 'E';
inline constexpr uint8_t kRecHeal = 'H';
inline constexpr uint8_t kRecLogs = 'L';

// —— 指标快照块：一次 SnapshotAndReset 的全部行（30s 窗口增量）——
// ['M'][varint n] n × ([u8 kind][fixed64 key][varint label_idx][varint count]
//                      [varint sum][hist: varint dlen + digest])
void EncodeMetricsChunk(const std::vector<MetricsTable::Row>& rows, std::vector<uint8_t>* out);

struct MetricsRowView {
  uint8_t kind = 0;  // 0=counter 1=histogram（与 SchemaRegistry::MetricKind 对应）
  uint64_t key = 0;
  uint16_t label_idx = 0;
  uint64_t count = 0;
  uint64_t sum = 0;
  TDigest digest;
};
bool DecodeMetricsChunk(const uint8_t* data, size_t len, std::vector<MetricsRowView>* out);

// —— 轨迹块：公共 base（块内最小单调钟）+ delta 事件 ——
// ['T'][fixed64 base_mono_ns][varint n] n × TraceEventWire
void EncodeTraceChunk(uint64_t base_mono_ns, const std::vector<TraceEventWire>& events,
                      std::vector<uint8_t>* out);
bool DecodeTraceChunk(const uint8_t* data, size_t len, uint64_t* base_mono_ns,
                      std::vector<TraceEventWire>* out);

// —— 结构化事件（单条记录）——
// ['E'][u8 level][fixed64 key][fixed64 ts_mono_ns][varint a][varint b]
struct EventRecordWire {
  uint8_t level = 0;  // 0=P0 1=P1 2=P2（决定上报通道优先级）
  uint64_t key = 0;
  uint64_t ts_mono_ns = 0;
  uint64_t a = 0;
  uint64_t b = 0;
};
void EncodeEventRecord(const EventRecordWire& rec, std::vector<uint8_t>* out);
bool DecodeEventRecord(const uint8_t* data, size_t len, EventRecordWire* out);

// —— 自愈事件记录（全量上报铁律的线上载体，架构 §11）——
// ['H'][fixed64 scene_id][u8 from][u8 to][u8 ladder][u8 reason][fixed64 ts_ms]
// [varint action_len][action]
struct HealRecordWire {
  uint64_t scene_id = 0;
  uint8_t from = 0;
  uint8_t to = 0;
  uint8_t ladder_index = 0;
  uint8_t reason = 0;
  uint64_t ts_ms = 0;
  std::string action;
};
void EncodeHealRecord(const HealRecordWire& rec, std::vector<uint8_t>* out);
bool DecodeHealRecord(const uint8_t* data, size_t len, HealRecordWire* out);

// —— 日志回捞块（kFetchLogs 命令的上行载体）——
// ['L'][fixed64 command_id][fixed64 base_wall_ms][varint n]
// n × ([varint dwall_ms][u8 level][fixed64 module][varint len][text])
// command_id 随块携带：服务端把回捞产物对账到发起命令（闭环凭据）。
struct LogEntryWire {
  int64_t wall_ms = 0;
  uint8_t level = 0;
  uint64_t module_hash = 0;
  std::string text;
};
void EncodeLogChunk(uint64_t command_id, int64_t base_wall_ms,
                    const std::vector<LogEntryWire>& entries, std::vector<uint8_t>* out);
bool DecodeLogChunk(const uint8_t* data, size_t len, uint64_t* command_id,
                    std::vector<LogEntryWire>* out);

}  // namespace imobs
