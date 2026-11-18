#pragma once
// ObsKernel：端侧内核装配（实施计划 P1.1 消费线程骨架）。
//
// 分层与两个兄弟内核同构：
//   KernelCore —— 被动单线程核：drain 分片环 → 按信号类型分发（聚合/轨迹块/事件）
//                 → 窗口职责（30s 快照、轨迹块超龄冲刷）→ 驱动上报管线。
//                 全部时序由注入的 IClock + 显式 Step() 决定，虚拟时钟下 100% 确定性。
//   ObsKernel  —— 薄线程壳：真实部署形态。热路径打点 API（任意线程，wait-free）
//                 + 唯一消费线程循环驱动 KernelCore::Step。
//
// 线程契约：
//   - Emit / AddCounter / RecordHistogram / TraceStage / EmitEvent：任意线程；
//   - Step / Shutdown：仅消费线程；
//   - SetForeground：任意线程（原子标志，Step 时应用到管线）。

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "imobs/clock.h"
#include "imobs/event_ring.h"
#include "imobs/healer.h"
#include "imobs/generic_events.h"
#include "imobs/log_store.h"
#include "imobs/metrics.h"
#include "imobs/pipeline.h"
#include "imobs/policy.h"
#include "imobs/schema.h"
#include "imobs/wire.h"

namespace imobs {

// kHealth 事件的子操作（stage 字节承载）：域信号与控制面指令经分片环跨线程
// 进入健康域——健康域是消费线程私有状态，任何线程都不得直接触碰。
enum class HealthOp : uint8_t {
  kSignal = 0,        // key = signal_id, value = 0/1
  kGlobalOutage = 1,  // value = 0/1（"大家都坏"健康位）
  kPrecondition = 2,  // key = scene_id, value = 0/1（L2 前置就绪）
  kMute = 3,          // key = scene_id, value = ttl_ms（云端 kill switch）
  kUnmute = 4,        // key = scene_id
};

// 命令回执以事件记录回流（key 固定）：服务端按 command_id 对账闭环。
inline constexpr uint64_t kPolicyReceiptEventKey = Fnv1a("imobs.policy.receipt");

struct KernelOptions {
  size_t ring_shards = 4;                       // pow2
  size_t ring_capacity_per_shard = 1 << 12;     // 4 × 4K × 128B = 2MB
  uint64_t metrics_snapshot_interval_ms = 30'000;
  size_t trace_chunk_max_events = 256;          // 轨迹块：按量冲刷
  uint64_t trace_chunk_max_age_ms = 5'000;      //         或按龄冲刷
  uint64_t poll_interval_ms = 10;               // 线程壳空闲睡眠（消费延迟上界）
  uint32_t heal_rng_seed = 1;                   // 自愈防抖抖动种子
  PipelineOptions pipeline;
  // 自愈场景在启动前声明式注册（executor 生命周期由域保证长于内核）。
  std::vector<std::pair<SceneSpec, IActionExecutor*>> heal_scenes;
  // 控制面：HMAC 密钥（生产必须替换为分发密钥）与策略默认值。
  std::string control_key;  // Empty disables remote control; host must explicitly provision a key.
  PolicyBundle policy_defaults;
  // 回捞命令的宿主执行体覆盖（未设置且日志域启用时用内置执行体；
  // 两者皆无 → 回执 "no-fetch-handler"）。
  PolicyClient::CommandHandler fetch_logs_handler;

  // —— 日志域（P5：结构化环形日志 + 回捞闭环）——
  std::string log_path;                 // 空 = 不启用日志域
  uint32_t log_segment_size = 256 * 1024;
  uint32_t log_segment_count = 8;
  const uint8_t* log_key32 = nullptr;   // 非空 = AES-GCM 逐条加密（fail-closed）
  size_t log_inbox_cap = 4096;          // 日志收件箱上限：超出丢弃并记账（防打爆）
  bool fetch_requires_consent = true;   // 回捞必须用户授权（隐私铁律，架构 §12）
  size_t fetch_chunk_max_bytes = 8 * 1024;  // 回捞分片：单条 'L' 记录的文本预算
};

class KernelCore {
 public:
  static std::unique_ptr<KernelCore> Open(const std::string& dir, IClock* clock,
                                          IChannel* channel, const SchemaRegistry* schema,
                                          const KernelOptions& opt, std::string* err);

  // —— 热路径（任意线程）：wait-free 入队，满则丢弃（环内有账）——
  bool Emit(const Event& ev) { return ring_.TryPush(ev); }
  bool EmitGeneric(const madobs_event_v1& ev) { return generic_ring_.Push(ev); }
  uint64_t GenericEnqueued() const { return generic_ring_.enqueued.load(); }
  uint64_t GenericDropped() const { return generic_ring_.dropped.load(); }
  uint64_t GenericDrained() const { return generic_drained_.load(); }
  uint64_t GenericWalDropped() const { return generic_wal_dropped_.load(); }

  // —— 消费线程 ——
  // 一次步进：drain（单轮上限防风暴饿死定时职责）→ 分发 → 窗口职责 → 管线 Poll。
  // 返回本轮消化的事件数（线程壳据此决定是否空闲睡眠）。
  size_t Step();
  // 终局冲刷：排空环 → 冲刷轨迹块 → 强制快照 → 管线 FlushAll + Poll。
  void Shutdown();

  void SetForeground(bool fg) { foreground_.store(fg, std::memory_order_relaxed); }

  // 控制面入口（任意线程）：下发字节（策略/命令信封）投入收件箱，Step 内验签应用。
  // 冷路径允许互斥锁（热路径审计不涉及本层）；信封体积上限由 PolicyClient 防御。
  void SubmitControlEnvelope(const void* data, size_t len);

  // 日志入口（任意线程，冷路径互斥锁）：进收件箱，Step 内由消费线程落盘。
  // 文本超长在 LogStore 截断；收件箱满则丢弃并记账——日志绝不反压业务线程。
  void Log(uint8_t level, uint64_t module_hash, std::string_view text);

  // 回捞授权（任意线程）：默认拒绝，宿主在用户明示同意后打开（隐私铁律）。
  void SetFetchConsent(bool granted) { fetch_consent_.store(granted, std::memory_order_relaxed); }

  struct SelfStats {
    uint64_t drained = 0;
    uint64_t ring_dropped = 0;
    uint64_t unknown_key_dropped = 0;
    uint64_t trace_sampled_out = 0;  // 采样策略丢弃的轨迹事件（有账）
    uint64_t log_inbox_dropped = 0;  // 日志收件箱满丢弃（有账）
  };
  SelfStats self_stats() const;
  UploadPipeline& pipeline() { return *pipeline_; }  // 诊断与测试用（仅消费线程）
  Healer& healer() { return *healer_; }              // 同上
  PolicyClient& policy() { return *policy_; }        // 同上
  LogStore* log_store() { return log_store_.get(); }  // 同上（未启用 = nullptr）

 private:
  KernelCore(IClock* clock, const SchemaRegistry* schema, const KernelOptions& opt);
  uint64_t MonoMs() const { return clock_->MonoNanos() / 1'000'000ull; }
  void Dispatch(const Event& ev);
  size_t DrainGeneric();
  void DispatchHealthOp(const Event& ev);
  void OnHealEvent(const HealEvent& e);  // 自愈事件回流观测（全量上报铁律）
  void FlushTraceChunk();
  void MaybeSnapshot(bool force);
  void DrainLogInbox();
  bool ExecuteFetchLogs(const Command& cmd, std::string* reason);  // 内置回捞执行体

  IClock* clock_;
  KernelOptions opt_;
  ShardedEventRing ring_;
  GenericEventQueue generic_ring_;
  std::atomic<uint64_t> generic_drained_{0}, generic_wal_dropped_{0};
  MetricsTable metrics_;  // schema 指针由聚合表持有（内核不直接消费 schema）
  std::unique_ptr<UploadPipeline> pipeline_;
  std::unique_ptr<Healer> healer_;
  std::unique_ptr<PolicyClient> policy_;

  std::mutex control_mu_;                            // 控制面收件箱（冷路径）
  std::vector<std::vector<uint8_t>> control_inbox_;

  std::unique_ptr<LogStore> log_store_;              // 日志域（消费线程私有）
  std::mutex log_mu_;                                // 日志收件箱（冷路径）
  std::vector<LogStore::Entry> log_inbox_;
  std::atomic<uint64_t> log_inbox_dropped_{0};       // 任意线程写、消费线程读
  std::atomic<bool> fetch_consent_{false};

  std::vector<Event> trace_raw_;   // 轨迹块缓冲：冲刷时取块内最小 ts 为 base（跨分片乱序安全）
  uint64_t trace_first_ms_ = 0;
  uint64_t last_snapshot_ms_ = 0;
  std::atomic<bool> foreground_{true};
  bool fg_applied_ = true;
  uint64_t drained_ = 0;
  uint64_t trace_sampled_out_ = 0;
};

class ObsKernel {
 public:
  // schema 与 channel 由宿主持有且必须比内核长寿（注册期资产 / 注入的传输）。
  static std::unique_ptr<ObsKernel> Start(const std::string& dir, IChannel* channel,
                                          const SchemaRegistry* schema,
                                          const KernelOptions& opt, std::string* err);
  ~ObsKernel();
  void Stop();  // 幂等：停消费线程并在其内完成终局冲刷

  // —— 热路径打点 API（任意线程）：key/label_idx 由冷路径注册解析后缓存 ——
  void AddCounter(uint64_t key, uint16_t label_idx, uint64_t delta = 1);
  void RecordHistogram(uint64_t key, uint16_t label_idx, uint64_t value);
  void TraceStage(uint64_t msg_id, uint8_t stage, uint8_t status, uint32_t attr = 0,
                  uint64_t value = 0);
  void EmitEvent(uint64_t key, uint8_t level, uint32_t a = 0, uint32_t b = 0);

  // —— 自愈域输入（任意线程，经环转交消费线程的健康域）——
  void SetHealthSignal(uint64_t signal_id, bool active);
  void SetGlobalOutage(bool outage);
  void MuteHealScene(uint64_t scene_id, uint64_t ttl_ms);   // 云端 kill switch 入口
  void SetHealPrecondition(uint64_t scene_id, bool ready);

  // 控制面下发入口（任意线程；通道收到推送/拉取结果后调用）
  void SubmitControlEnvelope(std::string_view bytes) {
    core_->SubmitControlEnvelope(bytes.data(), bytes.size());
  }

  // —— 日志域（任意线程，冷路径）：本地全都记，回捞才上传 ——
  void Log(uint8_t level, uint64_t module_hash, std::string_view text) {
    core_->Log(level, module_hash, text);
  }
  // 回捞授权（隐私铁律）：默认拒绝，用户明示同意后宿主打开。
  void SetFetchConsent(bool granted) { core_->SetFetchConsent(granted); }

  void SetForeground(bool fg) { core_->SetForeground(fg); }
  KernelCore& core() { return *core_; }

 private:
  ObsKernel() = default;
  void EmitHealthOp(HealthOp op, uint64_t key, uint64_t value);

  IClock* clock_ = nullptr;
  KernelOptions opt_;
  std::unique_ptr<KernelCore> core_;
  std::thread worker_;
  std::atomic<bool> stop_{false};
};

}  // namespace imobs
