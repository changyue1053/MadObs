#pragma once
// 端上自愈框架（实施计划 P4，架构 §11）：HealthFSM + 判定器 + 升级链 + 熔断频控。
//
// 五条框架级铁律（比"聪明"更优先的是"可控"）：
//   1. 编排器只发指令不执行动作——修复动作由拥有状态的域在自己线程执行，
//      完成后回执（回执必须投递回消费线程；测试用同步回执）；
//   2. 每次状态迁移必经合法迁移矩阵校验并全量上报（EventSink）——
//      自愈的目标是"故障不伤害用户"，绝不是"故障不被看见"；
//   3. 判定保守：多信号交叉验证（∧）+ 防抖窗口 + 全局故障位抑制
//      （"大家都坏"时端侧只等待，不自愈——防自愈风暴第一道防线）；
//   4. 破坏等级前置：L2 重手段要求前置条件就绪 + 全局互斥（同刻一台重手术）；
//   5. 熔断先于聪明：本地连续力竭自动熔断（MUTED + TTL），云端 kill switch
//      可随时静默任意场景；迟到回执用 epoch 作废，绝不复活已终结的疗程。
//
// 线程契约：全部方法仅消费线程调用；done 可任意线程，投递到有界完成队列，Tick 消费。

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <deque>
#include <atomic>
#include <random>
#include <string>
#include <vector>

#include "imobs/clock.h"

namespace imobs {

enum class HealthState : uint8_t {
  kHealthy = 0,
  kSuspect,     // 异常信号，防抖窗口内
  kDiagnosing,  // 交叉验证窗口：信号须持续活跃
  kHealing,     // 升级链当前级动作执行中
  kVerifying,   // 愈后校验中
  kExhausted,   // 瞬态：链耗尽（P0 级上报点），随即转 Healthy 或 Muted
  kMuted,       // 熔断态：本地力竭或云端 kill switch
};

enum class DamageLevel : uint8_t { kL0 = 0, kL1, kL2 };

enum class HealReason : uint8_t {
  kSignal = 1,        // Healthy → Suspect
  kFlapCleared,       // Suspect → Healthy（防抖过滤，正常工作不算误报）
  kGlobalSuppressed,  // → Healthy（全局故障位置位：判定冻结）
  kDebouncePassed,    // Suspect → Diagnosing
  kFalsePositive,     // Diagnosing → Healthy（交叉验证不成立）
  kConfirmed,         // Diagnosing → Healing
  kActionOk,          // Healing → Verifying
  kActionFail,        // 升级链上行
  kActionTimeout,
  kVerifyOk,          // Verifying → Healthy（自愈成功）
  kVerifyFail,
  kVerifyTimeout,
  kExhaustedLadder,   // → Exhausted
  kBreakerTrip,       // Exhausted → Muted
  kMutedTtlExpired,   // Muted → Healthy
  kKillSwitch,        // 任意态 → Muted（云端）
  kUnmute,
};

// 全量上报的最小载体：内核层将其路由为 P0/P1 事件（kExhausted/kBreakerTrip 为 P0）。
struct HealEvent {
  uint64_t scene_id = 0;
  HealthState from = HealthState::kHealthy;
  HealthState to = HealthState::kHealthy;
  uint8_t ladder_index = 0;
  HealReason reason = HealReason::kSignal;
  std::string action;  // Healing/Verifying 相关迁移携带动作名
  uint64_t ts_ms = 0;
};

// 域执行接口：Execute 发起修复动作，Verify 做愈后校验（"没好换更强的药"的依据）。
class IActionExecutor {
 public:
  using DoneFn = std::function<void(bool ok)>;
  virtual ~IActionExecutor() = default;
  virtual void Execute(uint64_t scene_id, const std::string& action, DoneFn done) = 0;
  virtual void Verify(uint64_t scene_id, DoneFn done) = 0;
};

struct ActionSpec {
  std::string name;
  DamageLevel level = DamageLevel::kL0;
  uint64_t cooldown_ms = 60'000;   // 同动作冷却：反复自愈说明根因未除
  uint32_t max_per_day = 10;       // 日频控（按破坏等级建议递减）
  bool needs_precondition = false; // L2 前置：如"备份已完成"（SetPrecondition 置位）
  uint64_t timeout_ms = 10'000;    // 执行超时 → 按失败升级
};

struct DetectorSpec {
  std::vector<uint64_t> all_of;        // 多信号交叉验证：全部活跃才算疑似（∧）
  uint64_t debounce_ms = 2'000;        // 防抖窗口
  uint64_t debounce_jitter_ms = 1'000; // 防抖抖动：防全体同刻确诊（风暴第二道防线）
  uint64_t diagnose_ms = 1'000;        // 交叉验证窗口：期间信号消退 = 误报
  bool suppressed_by_global = true;    // 全局故障位置位时冻结判定
};

struct SceneSpec {
  uint64_t scene_id = 0;               // Fnv1a("conn.half_open") 等
  DetectorSpec detector;
  std::vector<ActionSpec> ladder;      // 轻 → 重
  uint64_t verify_timeout_ms = 5'000;
  uint32_t breaker_threshold = 3;      // 连续力竭 N 次 → 本地熔断
  uint64_t muted_ttl_ms = 3'600'000;
};

struct HealerOptions {
  uint32_t rng_seed = 1;  // 防抖抖动种子（测试确定性）
};

class Healer {
 public:
  using EventSink = std::function<void(const HealEvent&)>;

  Healer(IClock* clock, EventSink sink, HealerOptions opt = {});
  Healer(const Healer&) = delete;
  Healer& operator=(const Healer&) = delete;
  Healer(Healer&&) = delete;
  Healer& operator=(Healer&&) = delete;

  // 注册强制项：executor 非空、判定信号非空、升级链非空；重复 scene_id 拒绝。
  bool RegisterScene(const SceneSpec& spec, IActionExecutor* executor);

  // —— 信号与环境输入 ——
  void SetSignal(uint64_t signal_id, bool active);
  void SetGlobalOutage(bool outage);                     // "大家都坏"健康位（拨测/下发）
  void SetPrecondition(uint64_t scene_id, bool ready);   // L2 前置条件

  // —— 云端控制面 ——
  void MuteScene(uint64_t scene_id, uint64_t ttl_ms);    // kill switch
  void UnmuteScene(uint64_t scene_id);

  // —— 时间驱动（消费线程周期调用；虚拟时钟下全确定性）——
  void Tick();

  HealthState state(uint64_t scene_id) const;

  struct Stats {
    uint64_t completion_dropped = 0;
    uint64_t heal_attempts = 0;
    uint64_t heal_success = 0;
    uint64_t escalations = 0;
    uint64_t false_positives = 0;
    uint64_t exhausted = 0;
    uint64_t breaker_trips = 0;
    uint64_t suppressed_by_global = 0;
    uint64_t unexpected_transitions = 0;  // 非法迁移防御计数（正常恒为 0）
  };
  Stats stats() const { auto s = stats_; s.completion_dropped = inbox_->dropped.load(); return s; }

 private:
  struct ActionRt {
    uint64_t last_exec_ms = 0;
    int64_t day = -1;
    uint32_t day_count = 0;
  };
  struct Scene {
    SceneSpec spec;
    IActionExecutor* exec = nullptr;
    HealthState state = HealthState::kHealthy;
    uint32_t epoch = 0;  // 迟到回执作废锚：任何终结/升级都递增
    uint64_t debounce_target_ms = 0;
    uint64_t diagnose_since_ms = 0;
    size_t ladder_idx = 0;
    bool inflight = false;
    uint64_t deadline_ms = 0;
    bool holds_heavy = false;
    bool precondition_ready = false;
    uint32_t consecutive_exhausted = 0;
    uint64_t muted_until_ms = 0;
    std::vector<ActionRt> rt;
  };

  uint64_t NowMs() const { return clock_->MonoNanos() / 1'000'000ull; }
  bool AllSignalsActive(const Scene& s) const;
  void Transit(Scene& s, HealthState to, HealReason reason, const std::string& action = {});
  void TickScene(Scene& s);
  void EnterHealing(Scene& s, HealReason why);
  void Escalate(Scene& s, HealReason why);
  void Exhaust(Scene& s);
  void ReleaseHeavy(Scene& s);
  void OnExecDone(uint64_t scene_id, uint32_t epoch, bool ok);
  void OnVerifyDone(uint64_t scene_id, uint32_t epoch, bool ok);

  struct Completion { uint64_t id; uint32_t epoch; bool verify; bool ok; };
  struct CompletionInbox { std::mutex mu; std::deque<Completion> items; std::atomic<uint64_t> dropped{0}; };
  std::shared_ptr<CompletionInbox> inbox_ = std::make_shared<CompletionInbox>();
  IActionExecutor::DoneFn CompletionCallback(uint64_t id, uint32_t epoch, bool verify);
  void DrainCompletions();
  IClock* clock_;
  EventSink sink_;
  std::minstd_rand rng_;
  std::map<uint64_t, Scene> scenes_;
  std::map<uint64_t, bool> signals_;
  bool global_outage_ = false;
  bool heavy_busy_ = false;  // L2 全局互斥：同刻一台重手术
  Stats stats_;
};

}  // namespace imobs
