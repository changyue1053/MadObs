#include "imobs/healer.h"

namespace imobs {
namespace {

constexpr size_t kStateCount = 7;

// 合法迁移矩阵（表驱动的"表"）：不在表里的组合 = 逻辑漏洞，防御性拒绝并记账。
constexpr bool kAllowed[kStateCount][kStateCount] = {
    //             to: Healthy Suspect Diagnos Healing Verify  Exhaust Muted
    /* Healthy   */ {false, true, false, false, false, false, true},
    /* Suspect   */ {true, false, true, false, false, false, true},
    /* Diagnosing*/ {true, false, false, true, false, true, true},
    /* Healing   */ {false, false, false, true, true, true, true},
    /* Verifying */ {true, false, false, true, false, true, true},
    /* Exhausted */ {true, false, false, false, false, false, true},
    /* Muted     */ {true, false, false, false, false, false, false},
};

}  // namespace

Healer::Healer(IClock* clock, EventSink sink, HealerOptions opt)
    : clock_(clock), sink_(std::move(sink)) {
  rng_.seed(opt.rng_seed);
}

bool Healer::RegisterScene(const SceneSpec& spec, IActionExecutor* executor) {
  if (spec.scene_id == 0 || executor == nullptr || spec.ladder.empty() ||
      spec.detector.all_of.empty() || scenes_.count(spec.scene_id) != 0) {
    return false;
  }
  Scene s;
  s.spec = spec;
  s.exec = executor;
  s.rt.resize(spec.ladder.size());
  scenes_.emplace(spec.scene_id, std::move(s));
  return true;
}

void Healer::SetSignal(uint64_t signal_id, bool active) { signals_[signal_id] = active; }

void Healer::SetGlobalOutage(bool outage) { global_outage_ = outage; }

void Healer::SetPrecondition(uint64_t scene_id, bool ready) {
  const auto it = scenes_.find(scene_id);
  if (it != scenes_.end()) it->second.precondition_ready = ready;
}

bool Healer::AllSignalsActive(const Scene& s) const {
  for (const uint64_t sig : s.spec.detector.all_of) {
    const auto it = signals_.find(sig);
    if (it == signals_.end() || !it->second) return false;
  }
  return true;
}

void Healer::Transit(Scene& s, HealthState to, HealReason reason, const std::string& action) {
  const auto from_i = static_cast<size_t>(s.state);
  const auto to_i = static_cast<size_t>(to);
  if (!kAllowed[from_i][to_i]) {
    ++stats_.unexpected_transitions;  // 防御：不迁移、不崩溃、留账（架构 §11 FSM 约定）
    return;
  }
  HealEvent ev;
  ev.scene_id = s.spec.scene_id;
  ev.from = s.state;
  ev.to = to;
  ev.ladder_index = static_cast<uint8_t>(s.ladder_idx);
  ev.reason = reason;
  ev.action = action;
  ev.ts_ms = NowMs();
  s.state = to;
  if (sink_) sink_(ev);  // 全量上报铁律：每次迁移必留痕
}

void Healer::ReleaseHeavy(Scene& s) {
  if (s.holds_heavy) {
    s.holds_heavy = false;
    heavy_busy_ = false;
  }
}

void Healer::EnterHealing(Scene& s, HealReason why) {
  const uint64_t now = NowMs();
  while (s.ladder_idx < s.spec.ladder.size()) {
    const ActionSpec& a = s.spec.ladder[s.ladder_idx];
    ActionRt& rt = s.rt[s.ladder_idx];
    const int64_t day = clock_->WallMillis() / 86'400'000;
    if (rt.day != day) {
      rt.day = day;
      rt.day_count = 0;
    }
    const bool cooldown_ok = rt.last_exec_ms == 0 || now - rt.last_exec_ms >= a.cooldown_ms;
    const bool quota_ok = rt.day_count < a.max_per_day;
    const bool precond_ok = !a.needs_precondition || s.precondition_ready;
    const bool heavy_ok = a.level != DamageLevel::kL2 || !heavy_busy_ || s.holds_heavy;
    if (cooldown_ok && quota_ok && precond_ok && heavy_ok) {
      if (a.level == DamageLevel::kL2 && !s.holds_heavy) {
        heavy_busy_ = true;  // 重手段互斥：竞争者跳过本级——宁可晚愈，不可并发重手术
        s.holds_heavy = true;
      }
      rt.last_exec_ms = now;
      ++rt.day_count;
      s.inflight = true;
      s.deadline_ms = now + a.timeout_ms;
      ++stats_.heal_attempts;
      Transit(s, HealthState::kHealing, why, a.name);
      const uint64_t id = s.spec.scene_id;
      const uint32_t epoch = s.epoch;
      s.exec->Execute(id, a.name, CompletionCallback(id, epoch, false));
      return;
    }
    ++s.ladder_idx;  // 本级不可执行（冷却/频控/前置/互斥）：跳过
  }
  Exhaust(s);
}

void Healer::Escalate(Scene& s, HealReason why) {
  ++stats_.escalations;
  ReleaseHeavy(s);
  ++s.epoch;  // 作废本级一切在途回执
  s.inflight = false;
  ++s.ladder_idx;
  EnterHealing(s, why);
}

void Healer::Exhaust(Scene& s) {
  ReleaseHeavy(s);
  ++s.epoch;
  s.inflight = false;
  ++stats_.exhausted;
  ++s.consecutive_exhausted;
  Transit(s, HealthState::kExhausted, HealReason::kExhaustedLadder);  // P0 级上报点
  if (s.consecutive_exhausted >= s.spec.breaker_threshold) {
    ++stats_.breaker_trips;
    s.muted_until_ms = NowMs() + s.spec.muted_ttl_ms;
    Transit(s, HealthState::kMuted, HealReason::kBreakerTrip);
  } else {
    // 允许再检测：冷却与日频控防热循环（治不好反复治只是掩盖根因）
    Transit(s, HealthState::kHealthy, HealReason::kExhaustedLadder);
  }
}

void Healer::OnExecDone(uint64_t scene_id, uint32_t epoch, bool ok) {
  const auto it = scenes_.find(scene_id);
  if (it == scenes_.end()) return;
  Scene& s = it->second;
  if (s.epoch != epoch || !s.inflight || s.state != HealthState::kHealing) return;  // 迟到/失效
  if (!ok) {
    Escalate(s, HealReason::kActionFail);
    return;
  }
  // 动作完成 ≠ 治好：必须愈后校验
  s.deadline_ms = NowMs() + s.spec.verify_timeout_ms;
  Transit(s, HealthState::kVerifying, HealReason::kActionOk, s.spec.ladder[s.ladder_idx].name);
  const uint32_t e = s.epoch;
  s.exec->Verify(scene_id, CompletionCallback(scene_id, e, true));
}

void Healer::OnVerifyDone(uint64_t scene_id, uint32_t epoch, bool ok) {
  const auto it = scenes_.find(scene_id);
  if (it == scenes_.end()) return;
  Scene& s = it->second;
  if (s.epoch != epoch || !s.inflight || s.state != HealthState::kVerifying) return;
  if (!ok) {
    Escalate(s, HealReason::kVerifyFail);
    return;
  }
  const std::string action = s.spec.ladder[s.ladder_idx].name;
  ReleaseHeavy(s);
  ++s.epoch;
  s.inflight = false;
  s.ladder_idx = 0;
  s.consecutive_exhausted = 0;
  ++stats_.heal_success;
  Transit(s, HealthState::kHealthy, HealReason::kVerifyOk, action);
}

void Healer::TickScene(Scene& s) {
  const uint64_t now = NowMs();
  const bool suppressed = global_outage_ && s.spec.detector.suppressed_by_global;
  switch (s.state) {
    case HealthState::kMuted:
      if (now >= s.muted_until_ms) {
        s.consecutive_exhausted = 0;
        Transit(s, HealthState::kHealthy, HealReason::kMutedTtlExpired);
      }
      return;

    case HealthState::kHealthy:
      if (suppressed) return;  // 全局故障期间判定冻结（退避等待，不自愈）
      if (AllSignalsActive(s)) {
        const uint64_t jmax = s.spec.detector.debounce_jitter_ms;
        const uint64_t jitter = jmax == 0 ? 0 : rng_() % (jmax + 1);
        s.debounce_target_ms = now + s.spec.detector.debounce_ms + jitter;
        Transit(s, HealthState::kSuspect, HealReason::kSignal);
      }
      return;

    case HealthState::kSuspect:
      if (suppressed) {
        ++stats_.suppressed_by_global;
        Transit(s, HealthState::kHealthy, HealReason::kGlobalSuppressed);
        return;
      }
      if (!AllSignalsActive(s)) {
        Transit(s, HealthState::kHealthy, HealReason::kFlapCleared);  // 防抖过滤，非误报
        return;
      }
      if (now >= s.debounce_target_ms) {
        s.diagnose_since_ms = now;
        Transit(s, HealthState::kDiagnosing, HealReason::kDebouncePassed);
      }
      return;

    case HealthState::kDiagnosing:
      if (suppressed) {
        ++stats_.suppressed_by_global;
        Transit(s, HealthState::kHealthy, HealReason::kGlobalSuppressed);
        return;
      }
      if (!AllSignalsActive(s)) {
        ++stats_.false_positives;  // 交叉验证不成立：这才是误报
        Transit(s, HealthState::kHealthy, HealReason::kFalsePositive);
        return;
      }
      if (now - s.diagnose_since_ms >= s.spec.detector.diagnose_ms) {
        s.ladder_idx = 0;
        EnterHealing(s, HealReason::kConfirmed);
      }
      return;

    case HealthState::kHealing:
    case HealthState::kVerifying:
      if (s.inflight && now >= s.deadline_ms) {
        const bool verifying = s.state == HealthState::kVerifying;
        Escalate(s, verifying ? HealReason::kVerifyTimeout : HealReason::kActionTimeout);
      }
      return;

    case HealthState::kExhausted:
      return;  // 瞬态：Exhaust() 内即刻迁走，不应驻留
  }
}

IActionExecutor::DoneFn Healer::CompletionCallback(uint64_t id, uint32_t epoch, bool verify) {
  std::weak_ptr<CompletionInbox> weak = inbox_;
  return [weak, id, epoch, verify](bool ok) {
    if (auto inbox = weak.lock()) {
      try {
      std::lock_guard<std::mutex> lk(inbox->mu);
      // Bounded: overflow leaves the action to its existing timeout path.
      if (inbox->items.size() < 1024) inbox->items.push_back({id, epoch, verify, ok});
      else inbox->dropped.fetch_add(1, std::memory_order_relaxed);
      } catch (...) {
        // A host completion must never unwind across its C callback boundary.
        inbox->dropped.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };
}

void Healer::DrainCompletions() {
  for (size_t budget = 0; budget < 1024; ++budget) {
    Completion c;
    {
      std::lock_guard<std::mutex> lk(inbox_->mu);
      if (inbox_->items.empty()) break;
      c = inbox_->items.front(); inbox_->items.pop_front();
    }
    if (c.verify) OnVerifyDone(c.id, c.epoch, c.ok);
    else OnExecDone(c.id, c.epoch, c.ok);
  }
}

void Healer::Tick() {
  DrainCompletions();
  for (auto& [id, scene] : scenes_) {
    (void)id;
    TickScene(scene);
    DrainCompletions();
  }
}

void Healer::MuteScene(uint64_t scene_id, uint64_t ttl_ms) {
  const auto it = scenes_.find(scene_id);
  if (it == scenes_.end()) return;
  Scene& s = it->second;
  ReleaseHeavy(s);
  ++s.epoch;  // 作废一切在途回执：静默即终结当前疗程
  s.inflight = false;
  s.muted_until_ms = NowMs() + ttl_ms;
  Transit(s, HealthState::kMuted, HealReason::kKillSwitch);
}

void Healer::UnmuteScene(uint64_t scene_id) {
  const auto it = scenes_.find(scene_id);
  if (it == scenes_.end()) return;
  Scene& s = it->second;
  if (s.state != HealthState::kMuted) return;
  s.consecutive_exhausted = 0;
  Transit(s, HealthState::kHealthy, HealReason::kUnmute);
}

HealthState Healer::state(uint64_t scene_id) const {
  const auto it = scenes_.find(scene_id);
  return it == scenes_.end() ? HealthState::kHealthy : it->second.state;
}

}  // namespace imobs
