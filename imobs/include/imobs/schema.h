#pragma once
// 白名单 schema 注册表（架构 §15/§16，实施计划 P1.5）——冷路径。
// 两条红线在注册期强制执行，不是运行时约定：
//   1. uid/device_id/msg_id/conn_id/trace_id 永不进 metrics label（基数红线）；
//   2. label 取值域必须有限枚举——自由文本没有入口（隐私红线）。

#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace imobs {

// 编译期 FNV-1a：指标名在编译期折叠成 8 字节 key，热路径不携带字符串。
constexpr uint64_t Fnv1a(std::string_view s) {
  uint64_t h = 1469598103934665603ull;
  for (char c : s) {
    h ^= static_cast<uint8_t>(c);
    h *= 1099511628211ull;
  }
  return h;
}

enum class SchemaError : uint8_t {
  kOk = 0,
  kForbiddenLabelKey,  // 高基数 ID 类 label，注册期直接拒绝
  kUnboundedLabel,     // 取值域为空 = 自由基数，拒绝
  kDuplicateName,
  kTooManyCombos,      // label 组合空间超出 uint16 索引
};

struct LabelDef {
  std::string key;
  std::vector<std::string> values;  // 必须有限枚举
};

class SchemaRegistry {
 public:
  enum class MetricKind : uint8_t { kCounter, kHistogram };
  struct Rejection {
    std::string name;
    SchemaError reason;
  };

  // 成功返回热路径使用的 key（fnv1a(name)）；失败返回 0 并计入 rejected()。
  uint64_t RegisterCounter(std::string_view name, std::vector<LabelDef> labels);
  uint64_t RegisterHistogram(std::string_view name, std::vector<LabelDef> labels);

  // label 组合 → 索引。冷路径解析一次，调用点缓存索引（热路径只带 uint16）。
  // combo 的顺序必须与注册时的 label 顺序一致：约定优于灵活，换来热路径零查找。
  bool ResolveLabelIndex(
      uint64_t key,
      const std::vector<std::pair<std::string_view, std::string_view>>& combo,
      uint16_t* out_idx) const;
  bool ResolveLabelIndex(
      uint64_t key,
      std::initializer_list<std::pair<std::string_view, std::string_view>> combo,
      uint16_t* out_idx) const {
    return ResolveLabelIndex(
        key, std::vector<std::pair<std::string_view, std::string_view>>(combo), out_idx);
  }

  // 索引 → "k=v,k=v"（快照序列化用）；无 label 返回空串。
  std::string RenderLabels(uint64_t key, uint16_t idx) const;

  bool Known(uint64_t key) const { return specs_.count(key) != 0; }
  MetricKind KindOf(uint64_t key) const { return specs_.at(key).kind; }
  const std::vector<Rejection>& rejected() const { return rejected_; }

 private:
  struct Spec {
    std::string name;
    MetricKind kind;
    std::vector<LabelDef> labels;
    uint32_t combos = 1;
  };
  uint64_t Register(std::string_view name, MetricKind kind, std::vector<LabelDef> labels);

  std::map<uint64_t, Spec> specs_;
  std::vector<Rejection> rejected_;
};

}  // namespace imobs
