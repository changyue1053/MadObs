#include "imobs/metrics.h"

namespace imobs {

void MetricsTable::Apply(const Event& ev) {
  if (!schema_->Known(ev.key)) {
    // 门面层已拒绝过未注册字段，这里是纵深防御第二道；命中说明有绕过门面的打点。
    ++unknown_dropped_;
    return;
  }
  const auto kind = schema_->KindOf(ev.key);
  Agg& a = table_[{ev.key, ev.label_idx}];
  a.kind = kind;
  ++a.count;
  a.sum += ev.value;
  if (kind == SchemaRegistry::MetricKind::kHistogram) {
    a.digest.Add(static_cast<double>(ev.value));
  }
}

std::vector<MetricsTable::Row> MetricsTable::SnapshotAndReset() {  // audit-allow: cold-path 30s 窗口快照导出，非 per-event（Apply 稳态零分配）
  std::vector<Row> rows;  // audit-allow: cold-path 30s 窗口快照导出
  rows.reserve(table_.size());
  for (auto& [k, a] : table_) {
    Row r;
    r.key = k.first;
    r.label_idx = k.second;
    r.kind = a.kind;
    r.count = a.count;
    r.sum = a.sum;
    r.digest = std::move(a.digest);
    rows.push_back(std::move(r));
  }
  table_.clear();
  return rows;
}

}  // namespace imobs
