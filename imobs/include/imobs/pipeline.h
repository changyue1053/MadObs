#pragma once
// 上报管线（实施计划 P2.3~P2.5 的被动式核心）：
//   Submit(记录) → 内存缓冲 → 成批（batch_id = 内容哈希，崩溃重发天然同 id）
//   → WAL 持久化 → 优先级调度（P0 即时 / P1 批量 / P2 周期）→ IChannel 发送
//   → ACK 推进 WAL 水位。
//
// 形态决策：被动对象 + 显式 Poll(now) 驱动，内部零线程零定时器——
// 全部行为由注入的 IClock 决定，虚拟时钟下可写毫秒级确定性测试
// （与 IMLink "库内任何模块不允许自带 sleep/线程定时器"同一铁律）；
// 消费线程外壳（真实定时驱动）是后续薄封装。
//
// 线程契约：全部方法仅消费线程调用；IChannel 的完成回调**可在任意线程调用**——
// 结果经线程安全完成队列转交消费线程（Poll 内消化），真实异步通道无需自行投递。

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "imobs/clock.h"
#include "imobs/wal_ring.h"

namespace imobs {

enum class Priority : uint8_t { kP0 = 0, kP1 = 1, kP2 = 2 };
inline constexpr size_t kPriorityCount = 3;

class IChannel {
 public:
  struct Result {
    bool ok = false;
  };
  using SendCallback = std::function<void(Result)>;
  virtual ~IChannel() = default;
  // payload 视图只在本次调用内有效：异步通道必须自行拷贝。
  // cb 可在任意线程调用、且允许晚于 pipeline 析构（回调落入的完成队列以
  // shared_ptr 续命，孤儿结果被安全丢弃）——但必须恰好调用一次。
  virtual void Send(Priority pri, uint64_t batch_id, std::string_view payload,
                    SendCallback cb) = 0;
};

struct PipelineOptions {
  uint64_t p1_flush_interval_ms = 30'000;
  uint64_t p1_flush_bytes = 64 * 1024;
  uint64_t p2_flush_interval_ms = 300'000;
  uint64_t day_quota_bytes = 1'000'000;  // 常态日上报预算（P0 不受限）
  uint64_t retry_base_ms = 1'000;        // full-jitter 指数退避
  uint64_t retry_cap_ms = 60'000;
  uint32_t rng_seed = 1;                 // 退避抖动种子（测试确定性）
  bool compress = true;                  // P1/P2 记录区 zlib 压缩（P0 永远明文）
  size_t compress_min_bytes = 512;       // 小批不压：deflate 头开销可能反向膨胀
  WalOptions wal[kPriorityCount];        // 三分区参数；默认按 P0/P1/P2 语义预置

  PipelineOptions() {
    wal[0].segment_size = 64 * 1024;   // P0：永不覆盖 + 逐条 sync（掉电兜底）
    wal[0].segment_count = 8;
    wal[0].overwrite_oldest = false;
    wal[0].sync_every_append = true;
    wal[1].segment_size = 256 * 1024;  // P1：覆盖最旧
    wal[1].segment_count = 8;
    wal[2].segment_size = 128 * 1024;  // P2：覆盖最旧（指标快照可再生）
    wal[2].segment_count = 8;
  }
};

class UploadPipeline {
 public:
  struct Stats {
    uint64_t batches_built = 0;
    uint64_t sends = 0;
    uint64_t acks = 0;
    uint64_t fails = 0;
    uint64_t quota_deferred = 0;
    uint64_t generic_records_dropped = 0;  // Generic records rejected at WAL append, not batch count
    uint64_t submit_dropped = 0;  // WAL 满（P0 分区）等原因
  };

  static std::unique_ptr<UploadPipeline> Open(const std::string& dir, IClock* clock,
                                              IChannel* channel, const PipelineOptions& opt,
                                              std::string* err);

  // 提交一条已序列化信号记录。P0 逐条即时成批；P1/P2 进内存缓冲等 flush。
  bool Submit(Priority pri, std::string_view record, bool generic = false);

  // 周期驱动：flush 判定 → 发送调度 → 退避重试。幂等，可任意频率调用。
  void Poll();

  void FlushAll();                 // 关停/切后台前：内存缓冲全部落批（进 WAL）
  void SetForeground(bool fg) { foreground_ = fg; }

  // 控制面运行时生效点（消费线程）：策略包里的可运营参数落到调度行为。
  void ApplyPolicy(uint64_t day_quota_bytes, uint64_t p1_flush_interval_ms) {
    opt_.day_quota_bytes = day_quota_bytes;
    opt_.p1_flush_interval_ms = p1_flush_interval_ms;
  }

  Stats stats() const { return stats_; }
  uint64_t PendingBatches(Priority pri) const;

 private:
  struct Part {
    std::unique_ptr<WalRing> wal;
    std::vector<uint8_t> buf;        // 未成批记录帧：varint(len) + bytes
    uint32_t buf_records = 0;
    uint32_t generic_records = 0;
    uint64_t last_flush_ms = 0;      // 单调毫秒
    bool in_flight = false;
    uint64_t next_try_ms = 0;
    uint32_t attempts = 0;
    std::string sending;             // 在发批的拷贝（通道异步时的生命周期锚）
  };

  struct Completion {
    Priority pri;
    WalRing::Cursor cursor;
    bool ok;
  };
  struct CompletionInbox {  // 通道任意线程 → 消费线程的唯一交接点
    std::mutex mu;
    std::vector<Completion> items;
  };

  UploadPipeline() = default;
  uint64_t MonoMs() const { return clock_->MonoNanos() / 1'000'000ull; }
  bool QuotaExhausted();
  void MaybeFlush(Priority pri, bool force);
  void TrySend(Priority pri);
  void DrainCompletions();
  void OnSendResult(Priority pri, WalRing::Cursor cursor, bool ok);

  IClock* clock_ = nullptr;
  IChannel* channel_ = nullptr;
  PipelineOptions opt_;
  std::shared_ptr<CompletionInbox> inbox_ = std::make_shared<CompletionInbox>();
  Part parts_[kPriorityCount];
  bool foreground_ = true;
  int64_t quota_day_ = -1;
  uint64_t quota_used_ = 0;
  std::minstd_rand rng_;
  Stats stats_;
};

}  // namespace imobs
