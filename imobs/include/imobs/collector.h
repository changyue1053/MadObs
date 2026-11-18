#pragma once
// 收集器桩（P7 网关+拼接的单机原型，e2e 资产）：
//   accept → 帧（[u32 len][batch]）→ **batch_id 幂等**（重复批照样 ACK 但不入账——
//   端侧崩溃重投递的服务端另一半）→ 轨迹拼接 → ACK（回显 batch_id）。
// 线程形态：监听线程 + 每连接一线程（桩的诚实规模；生产网关是另一个故事）。

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "imobs/stitcher.h"

namespace imobs {

class Collector {
 public:
  Collector() = default;
  ~Collector() { Stop(); }

  bool Start(uint16_t port, std::string* err);  // port 0 = 内核分配（port() 查询）
  void Stop();
  uint16_t port() const { return port_; }

  // 批观察者（ThinServer 等上层聚合的接入点）：**去重后**的新批原始字节，
  // 在连接线程、collector 锁外回调（观察者自行加锁；与查询路径无锁序耦合）。
  // 必须在 Start 之前设置。
  using BatchObserver = std::function<void(const void* data, size_t len)>;
  void SetBatchObserver(BatchObserver obs) { observer_ = std::move(obs); }

  struct Stats {
    uint64_t batches = 0;
    uint64_t duplicates = 0;  // 幂等命中：补报/重放被去重
    uint64_t trace_events = 0;
  };
  Stats stats() const;

  size_t JourneyCount() const;
  std::vector<uint8_t> StagesOf(uint64_t msg_id) const;
  bool JourneyComplete(uint64_t msg_id, const std::vector<uint8_t>& required) const;
  std::vector<uint64_t> JourneyIds(size_t limit) const;
  std::vector<uint8_t> SampleBatch() const;  // 首个批的原始字节（重放注入试验用）

 private:
  void AcceptLoop(int listen_fd);  // fd 按值传入：Stop 写成员不与循环读竞争
  void ServeConnection(int fd);

  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread accept_thread_;
  std::vector<std::thread> conn_threads_;
  std::set<int> conn_fds_;  // 活跃连接：Stop 时 shutdown 解除 recv 阻塞（否则 join 永挂）
  std::mutex conn_mu_;      // conn_threads_/conn_fds_ 保护

  mutable std::mutex mu_;  // 数据面：stitcher/去重集/统计
  TraceStitcher stitcher_;
  std::set<uint64_t> seen_batch_ids_;
  Stats stats_;
  std::vector<uint8_t> sample_batch_;
  BatchObserver observer_;  // Start 前设置，运行期只读
};

}  // namespace imobs
