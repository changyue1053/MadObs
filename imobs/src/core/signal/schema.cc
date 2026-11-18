#include "imobs/schema.h"

namespace imobs {
namespace {

// 基数红线（架构 §15）：ID 类高基数字段永不进 metrics label。
constexpr std::string_view kForbiddenKeys[] = {"uid", "device_id", "msg_id", "conn_id",
                                               "trace_id"};

bool IsForbidden(std::string_view k) {
  for (auto f : kForbiddenKeys) {
    if (f == k) return true;
  }
  return false;
}

}  // namespace

uint64_t SchemaRegistry::RegisterCounter(std::string_view name, std::vector<LabelDef> labels) {  // audit-allow: cold-path 注册期（start 前，schema.h 注明冷路径）
  return Register(name, MetricKind::kCounter, std::move(labels));
}

uint64_t SchemaRegistry::RegisterHistogram(std::string_view name, std::vector<LabelDef> labels) {  // audit-allow: cold-path 注册期（start 前）
  return Register(name, MetricKind::kHistogram, std::move(labels));
}

uint64_t SchemaRegistry::Register(std::string_view name, MetricKind kind,
                                  std::vector<LabelDef> labels) {  // audit-allow: cold-path 注册期（start 前）
  const uint64_t key = Fnv1a(name);
  if (specs_.count(key) != 0) {
    rejected_.push_back({std::string(name), SchemaError::kDuplicateName});  // audit-allow: cold-path 注册期拒绝账本
    return 0;
  }
  uint64_t combos = 1;
  for (const auto& l : labels) {
    if (IsForbidden(l.key)) {
      rejected_.push_back({std::string(name), SchemaError::kForbiddenLabelKey});  // audit-allow: cold-path 注册期拒绝账本
      return 0;
    }
    if (l.values.empty()) {
      rejected_.push_back({std::string(name), SchemaError::kUnboundedLabel});  // audit-allow: cold-path 注册期拒绝账本
      return 0;
    }
    combos *= l.values.size();
    if (combos > 65536) {
      rejected_.push_back({std::string(name), SchemaError::kTooManyCombos});  // audit-allow: cold-path 注册期拒绝账本
      return 0;
    }
  }
  specs_.emplace(key,
                 Spec{std::string(name), kind, std::move(labels), static_cast<uint32_t>(combos)});  // audit-allow: cold-path 注册期 spec 落账
  return key;
}

bool SchemaRegistry::ResolveLabelIndex(
    uint64_t key, const std::vector<std::pair<std::string_view, std::string_view>>& combo,  // audit-allow: cold-path 冷路径解析一次，热路径只带索引
    uint16_t* out_idx) const {
  const auto it = specs_.find(key);
  if (it == specs_.end()) return false;
  const Spec& sp = it->second;
  if (combo.size() != sp.labels.size()) return false;

  // 混合进制编码：idx = ((pos0 * n1) + pos1) * n2 + pos2 ...
  uint32_t idx = 0;
  size_t i = 0;
  for (const auto& [k, v] : combo) {
    const LabelDef& def = sp.labels[i++];
    if (def.key != k) return false;  // 顺序契约：与注册顺序一致
    size_t pos = def.values.size();
    for (size_t j = 0; j < def.values.size(); ++j) {
      if (def.values[j] == v) {
        pos = j;
        break;
      }
    }
    if (pos == def.values.size()) return false;  // 未注册取值
    idx = idx * static_cast<uint32_t>(def.values.size()) + static_cast<uint32_t>(pos);
  }
  *out_idx = static_cast<uint16_t>(idx);
  return true;
}

std::string SchemaRegistry::RenderLabels(uint64_t key, uint16_t idx) const {  // audit-allow: cold-path 快照序列化/查询面
  const auto it = specs_.find(key);
  if (it == specs_.end()) return {};
  const Spec& sp = it->second;
  if (sp.labels.empty()) return {};

  std::vector<size_t> pos(sp.labels.size());  // audit-allow: cold-path 快照序列化/查询面
  uint32_t rest = idx;
  for (size_t i = sp.labels.size(); i-- > 0;) {
    const size_t n = sp.labels[i].values.size();
    pos[i] = rest % n;
    rest /= n;
  }
  std::string out;  // audit-allow: cold-path 快照序列化/查询面
  for (size_t i = 0; i < sp.labels.size(); ++i) {
    if (i != 0) out += ',';
    out += sp.labels[i].key;
    out += '=';
    out += sp.labels[i].values[pos[i]];
  }
  return out;
}

}  // namespace imobs
