#pragma once
// 热路径审计区（scripts/audit_hotpath.py）：本文件禁止锁 / 堆 / 系统调用。

#include <cstdint>
#include <type_traits>

namespace imobs {

// 五类信号（架构 §5）。热路径事件是 64B 定长 POD：无堆、无指针、整槽拷贝。
enum class EventKind : uint8_t {
  kMetric = 1,
  kTrace = 2,
  kEvent = 3,
  kLog = 4,
  kHealth = 5,
};

struct Event {
  uint64_t ts_mono_ns;  // 单调钟；墙钟换算见 ClockAnchor（clock.h）
  uint64_t key;         // schema 注册返回的指标/实体键（编译期 fnv1a）
  uint64_t entity_id;   // 轨迹事件 = msg_id 低 64 位；其余为 0
  uint64_t value;       // counter 增量 / 时延 ns / 复用字段
  uint32_t attr_a;      // 场景自定义（conn_id 低位等）
  uint32_t attr_b;
  uint16_t label_idx;   // 预注册 label 组合索引：冷路径解析一次，热路径只带索引
  uint8_t kind;         // EventKind
  uint8_t stage;        // 轨迹节点 T0~T10；其余复用
  uint8_t status;       // 0=OK 1=RETRY 2=FAIL
  uint8_t pad_[19];
};

static_assert(sizeof(Event) == 64, "Event 必须是 64B 定长槽（缓存行友好、整槽拷贝）");
static_assert(std::is_trivially_copyable_v<Event>, "热路径整槽拷贝依赖 POD");

}  // namespace imobs
