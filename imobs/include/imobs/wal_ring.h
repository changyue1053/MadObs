#pragma once
// mmap 分段环形 WAL（架构 §9，实施计划 P2.1/P2.2）。
//
// 设计要点：
//   - 分区 = 一个文件 = N 个定长段组成的环；记录逐条 crc32c 校验；
//   - 写满策略由 overwrite_oldest 决定：P1/P2 分区覆盖最旧段（观测数据可再生），
//     P0 分区（overwrite_oldest=false）永不覆盖——满则拒绝并记账（满溢本身是强信号）；
//   - 崩溃口径（诚实账）：进程被杀（kill -9）零丢失——脏页由内核负责回写；
//     掉电可能丢最后未 sync 的记录，P0 分区用 sync_every_append 逐条 msync 兜底；
//   - 恢复：Open 时扫描写入段，crc 失败即判撕裂尾部，丢弃并计数——绝不出现半条记录；
//   - 消费水位 (epoch, offset) 持久化在文件头页；头页撕裂时回退到零水位，
//     方向安全：最坏重复上传，由上游 batch_id 去重（补报幂等）。
//
// 线程契约：Append/MarkConsumed 仅消费线程调用；ScanPending 返回的 payload
// 视图借用 mmap 内存，下一次 Append/析构后失效。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace imobs {

struct WalOptions {
  uint32_t segment_size = 64 * 1024;
  uint32_t segment_count = 8;
  bool overwrite_oldest = true;    // false = P0 语义（永不覆盖）
  bool sync_every_append = false;  // true = P0 语义（掉电兜底）
};

class WalRing {
 public:
  // 全局顺序 = (epoch, offset)：epoch 随段被启用单调递增，offset 是段内记录末端偏移。
  struct Cursor {
    uint64_t epoch = 0;
    uint32_t offset = 0;
  };
  struct Record {
    Cursor end;                // 该记录的消费水位（MarkConsumed 直接回传）
    std::string_view payload;  // 借用 mmap，见文件头线程契约
  };
  struct Stats {
    uint64_t appended = 0;
    uint64_t overflow_dropped = 0;  // P0 分区满 / 记录超长
    uint64_t torn_dropped = 0;      // 恢复时发现的撕裂尾部
  };

  static std::unique_ptr<WalRing> Open(const std::string& path, const WalOptions& opt,
                                       std::string* err);
  ~WalRing();
  WalRing(const WalRing&) = delete;
  WalRing& operator=(const WalRing&) = delete;

  bool Append(const void* data, uint32_t len);
  // 按 (epoch, offset) 升序回调所有未消费记录；回调返回 false 提前终止。
  void ScanPending(const std::function<bool(const Record&)>& cb) const;
  // 上报通道 ACK 后推进水位并持久化头页。
  bool MarkConsumed(const Cursor& upto);

  Stats stats() const { return stats_; }

 private:
  WalRing() = default;

  bool MapFile(const std::string& path, std::string* err);
  void InitFresh();
  void RecoverWriteSegment();
  bool AdvanceSegment();
  bool SegmentHasPending(uint32_t seg) const;
  void WipeSegment(uint32_t seg, uint64_t epoch);
  void PersistHeader();

  uint8_t* SegBase(uint32_t seg) const;
  uint64_t SegEpoch(uint32_t seg) const;
  // 段内记录遍历：返回 [start, ...) 每条有效记录；遇到无效头停止。
  void WalkSegment(uint32_t seg, uint32_t start,
                   const std::function<bool(uint32_t end_off, std::string_view payload)>& cb) const;

  WalOptions opt_;
  int fd_ = -1;
  uint8_t* map_ = nullptr;
  size_t map_len_ = 0;

  uint32_t cur_seg_ = 0;
  uint32_t write_off_ = 0;   // 段内偏移
  uint64_t max_epoch_ = 0;   // 当前写入段的 epoch
  Cursor watermark_{};

  Stats stats_{};
};

}  // namespace imobs
