#pragma once
// 消费线程侧指标聚合表（架构 §8：端内预聚合——一万次打点 = 一条上报记录）。
// 直方图 = t-digest（实施计划 P1.3）：分位摘要跨端可归并，端聚合、服务端合并
// 用同一结构；P99/P999 相对误差 < 1%（test_tdigest 对拍）。

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "imobs/event.h"
#include "imobs/schema.h"
#include "imobs/tdigest.h"

namespace imobs {

class MetricsTable {
 public:
  explicit MetricsTable(const SchemaRegistry* schema) : schema_(schema) {}

  // 仅消费线程调用。未注册 key 直接丢弃并记账（门面已拒绝过一次，这里是纵深防御）。
  void Apply(const Event& ev);

  struct Row {
    uint64_t key = 0;
    uint16_t label_idx = 0;
    SchemaRegistry::MetricKind kind = SchemaRegistry::MetricKind::kCounter;
    uint64_t count = 0;
    uint64_t sum = 0;
    TDigest digest;  // histogram：分位摘要；counter 为空
  };

  // 30s 周期调用：导出窗口增量并清零（快照即窗口，服务端按窗口累加/归并）。
  std::vector<Row> SnapshotAndReset();

  uint64_t unknown_key_dropped() const { return unknown_dropped_; }

 private:
  struct Agg {
    SchemaRegistry::MetricKind kind = SchemaRegistry::MetricKind::kCounter;
    uint64_t count = 0;
    uint64_t sum = 0;
    TDigest digest;
  };

  const SchemaRegistry* schema_;
  std::map<std::pair<uint64_t, uint16_t>, Agg> table_;
  uint64_t unknown_dropped_ = 0;
};

}  // namespace imobs
