#pragma once
// 轨迹拼接器（P7 拼接作业的核心逻辑，收集器/服务端/回放测试共用）：
// 批 → 'T' 轨迹块 → 按 msg_id 汇聚为旅程。幂等（batch_id 去重）由调用方负责——
// 拼接器只做"事件序列后期汇聚"这一件事（与传统 trace 的传播模型相对，架构 §5/§6）。
// 线程契约：单线程使用；并发访问由持有者（Collector）加锁。

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace imobs {

class TraceStitcher {
 public:
  // 输入一个完整批的字节：抽取其中全部轨迹块并入账。返回本批新增的轨迹事件数。
  size_t IngestBatch(const void* data, size_t len);

  size_t JourneyCount() const { return journeys_.size(); }
  uint64_t trace_events() const { return trace_events_; }

  // 该消息已见节点（升序去重）；不存在返回空。
  std::vector<uint8_t> StagesOf(uint64_t msg_id) const;
  // 完整性判定：required 全部到齐（"断链于哪一段"的反面）。
  bool Complete(uint64_t msg_id, const std::vector<uint8_t>& required) const;
  // 旅程枚举（查询面用）：最多 limit 个 msg_id（map 序 = 升序，翻页游标友好）。
  std::vector<uint64_t> JourneyIds(size_t limit) const;

 private:
  struct Journey {
    std::set<uint8_t> stages;
    uint64_t events = 0;
  };
  std::map<uint64_t, Journey> journeys_;
  uint64_t trace_events_ = 0;
};

}  // namespace imobs
