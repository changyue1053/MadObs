#pragma once
// 薄服务端（imobs-lite，实施计划 P7 的单机可运行形态）：
//   TCP ingest（Collector：帧解析 + batch_id 幂等 + 轨迹拼接）
//   + 记录聚合（指标合并 t-digest / 事件与自愈环形账 / 回捞日志按命令归档）
//   + HTTP/1.0 只读查询面（JSON；排障台与演示台直连）。
//
// "薄"的边界（架构 §8 的降配承诺）：单进程、内存态、无鉴权、只读查询——
// 它是协议正确性的参考实现与小规模部署形态；Kafka/Flink/ClickHouse 管道
// 是同一批 Decode* 函数的水平扩展，不是另一套协议。
//
// 查询面（GET）：
//   /healthz                     存活 + ingest 统计
//   /metrics                     聚合行：key/label/kind/count/sum/p50/p99
//   /journeys?limit=N            旅程列表（msg_id + 已见节点）
//   /journey?msg=<id>            单条消息物流单
//   /heals?limit=N               自愈事件账
//   /events?limit=N              结构化事件账
//   /logs?cmd=<command_id>       回捞产物（按发起命令对账）
//
// 线程契约：Start/Stop 生命周期线程；ingest 回调在 Collector 连接线程；
// HTTP 单 accept 线程内联应答（查询是内存读，薄形态足够）。

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imobs/collector.h"
#include "imobs/records.h"
#include "imobs/tdigest.h"

namespace imobs {

class ThinServer {
 public:
  struct Options {
    uint16_t ingest_port = 0;  // 0 = 内核分配
    uint16_t http_port = 0;
    size_t event_cap = 1024;   // 事件/自愈环形账上限（薄形态：内存有界）
  };

  ThinServer() = default;
  ~ThinServer() { Stop(); }

  bool Start(const Options& opt, std::string* err);
  void Stop();

  uint16_t ingest_port() const { return collector_.port(); }
  uint16_t http_port() const { return http_port_; }

  // 程序化查询面（HTTP 之外；测试与嵌入式部署用）。
  struct MetricRow {
    uint8_t kind = 0;
    uint64_t key = 0;
    uint16_t label_idx = 0;
    uint64_t count = 0;
    uint64_t sum = 0;
    TDigest digest;
  };
  std::vector<MetricRow> MetricsSnapshot() const;
  std::vector<LogEntryWire> LogsOf(uint64_t command_id) const;
  Collector& collector() { return collector_; }

 private:
  void OnBatch(const void* data, size_t len);
  void HttpLoop(int listen_fd);  // fd 按值传入：Stop 写成员不与循环读竞争
  std::string Handle(const std::string& path) const;  // 路由 → JSON（无锁序依赖）

  Collector collector_;
  Options opt_;

  mutable std::mutex agg_mu_;
  std::map<std::pair<uint64_t, uint16_t>, MetricRow> metrics_;
  std::deque<EventRecordWire> events_;
  std::deque<HealRecordWire> heals_;
  std::map<uint64_t, std::vector<LogEntryWire>> logs_;

  int http_fd_ = -1;
  uint16_t http_port_ = 0;
  std::thread http_thread_;
};

}  // namespace imobs
