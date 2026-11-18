#include "imobs/obs_kernel.h"

#include <algorithm>
#include <chrono>

#include "imobs/records.h"

namespace imobs {
namespace {
constexpr size_t kDrainBatch = 512;
constexpr size_t kMaxDrainPerStep = 4096;  // 单轮上限：事件风暴不得饿死定时职责与管线
}  // namespace

// ============================ KernelCore ============================

KernelCore::KernelCore(IClock* clock, const SchemaRegistry* schema, const KernelOptions& opt)
    : clock_(clock),
      opt_(opt),
      ring_(opt.ring_shards, opt.ring_capacity_per_shard),
      metrics_(schema) {
  trace_raw_.reserve(opt.trace_chunk_max_events);
}

std::unique_ptr<KernelCore> KernelCore::Open(const std::string& dir, IClock* clock,
                                             IChannel* channel, const SchemaRegistry* schema,
                                             const KernelOptions& opt, std::string* err) {
  std::unique_ptr<KernelCore> core(new KernelCore(clock, schema, opt));
  core->pipeline_ = UploadPipeline::Open(dir, clock, channel, opt.pipeline, err);
  if (!core->pipeline_) return nullptr;
  core->last_snapshot_ms_ = core->MonoMs();

  HealerOptions hopt;
  hopt.rng_seed = opt.heal_rng_seed;
  KernelCore* raw = core.get();
  core->healer_ = std::make_unique<Healer>(
      clock, [raw](const HealEvent& e) { raw->OnHealEvent(e); }, hopt);
  for (const auto& [spec, executor] : opt.heal_scenes) {
    if (!core->healer_->RegisterScene(spec, executor)) {
      if (err) *err = "自愈场景注册失败：" + std::to_string(spec.scene_id);
      return nullptr;  // 注册强制项不满足即拒绝启动——带病的自愈比没有更危险
    }
  }

  // 控制面装配：回执回流 + 内置命令 handler（kill switch 的最后一公里）
  core->policy_ = std::make_unique<PolicyClient>(clock, opt.control_key, opt.policy_defaults);
  core->policy_->SetReceiptSink([raw](const PolicyClient::Receipt& r) {
    EventRecordWire rec{/*level=*/1, kPolicyReceiptEventKey, raw->clock_->MonoNanos(),
                        r.command_id, r.ok ? 1u : 0u};
    std::vector<uint8_t> buf;
    EncodeEventRecord(rec, &buf);
    raw->pipeline_->Submit(
        Priority::kP1, std::string_view(reinterpret_cast<const char*>(buf.data()), buf.size()));
  });
  core->policy_->RegisterHandler(CommandType::kMuteHealScene,
                                 [raw](const Command& c, std::string*) {
                                   raw->healer_->MuteScene(c.arg_a, c.arg_b);
                                   return true;
                                 });
  core->policy_->RegisterHandler(CommandType::kUnmuteHealScene,
                                 [raw](const Command& c, std::string*) {
                                   raw->healer_->UnmuteScene(c.arg_a);
                                   return true;
                                 });
  // 日志域装配：环形日志 + 内置回捞执行体（宿主 handler 可整体覆盖）。
  if (!opt.log_path.empty()) {
    core->log_store_ = LogStore::Open(opt.log_path, opt.log_segment_size, opt.log_segment_count,
                                      err, opt.log_key32);
    if (!core->log_store_) return nullptr;  // fail-closed：要加密没提供方就拒绝启动
    core->log_inbox_.reserve(std::min<size_t>(opt.log_inbox_cap, 1024));
  }
  if (opt.fetch_logs_handler) {
    core->policy_->RegisterHandler(CommandType::kFetchLogs, opt.fetch_logs_handler);
  } else if (core->log_store_ != nullptr) {
    core->policy_->RegisterHandler(CommandType::kFetchLogs,
                                   [raw](const Command& c, std::string* reason) {
                                     return raw->ExecuteFetchLogs(c, reason);
                                   });
  }
  return core;
}

void KernelCore::Log(uint8_t level, uint64_t module_hash, std::string_view text) {
  if (log_store_ == nullptr) return;  // 未启用日志域：无声丢弃（打点 API 不报错）
  std::lock_guard<std::mutex> lk(log_mu_);
  if (log_inbox_.size() >= opt_.log_inbox_cap) {
    log_inbox_dropped_.fetch_add(1, std::memory_order_relaxed);  // 满则丢：不反压业务
    return;
  }
  LogStore::Entry e;
  e.wall_ms = clock_->WallMillis();
  e.level = level;
  e.module_hash = module_hash;
  e.text.assign(text.data(), text.size());
  log_inbox_.push_back(std::move(e));
}

void KernelCore::DrainLogInbox() {
  if (log_store_ == nullptr) return;
  std::vector<LogStore::Entry> items;
  {
    std::lock_guard<std::mutex> lk(log_mu_);
    items.swap(log_inbox_);
  }
  for (const LogStore::Entry& e : items) {
    log_store_->Append(e.wall_ms, e.level, e.module_hash, e.text);
  }
}

// 内置回捞执行体（kFetchLogs）：consent 门 → 冲刷收件箱 → 时间窗提取 →
// 分片编码 'L' 记录 → P1 上行。回执原因携带条数与分片数（服务端对账凭据）。
bool KernelCore::ExecuteFetchLogs(const Command& cmd, std::string* reason) {
  if (opt_.fetch_requires_consent && !fetch_consent_.load(std::memory_order_relaxed)) {
    *reason = "user-consent-missing";  // 隐私铁律：未授权的回捞必须失败且留痕
    return false;
  }
  DrainLogInbox();  // 截至此刻的日志先落盘，窗口提取才完整
  const auto start_ms = static_cast<int64_t>(cmd.arg_a);
  const auto end_ms = static_cast<int64_t>(cmd.arg_b);
  const uint64_t module = cmd.arg_s.empty() ? 0 : Fnv1a(cmd.arg_s);
  const std::vector<LogStore::Entry> entries = log_store_->Extract(start_ms, end_ms, module);

  size_t chunks = 0;
  std::vector<LogEntryWire> pending;
  size_t pending_bytes = 0;
  const auto flush = [&] {
    if (pending.empty()) return;
    std::vector<uint8_t> buf;
    EncodeLogChunk(cmd.command_id, pending.front().wall_ms, pending, &buf);
    pipeline_->Submit(Priority::kP1,
                      std::string_view(reinterpret_cast<const char*>(buf.data()), buf.size()));
    ++chunks;
    pending.clear();
    pending_bytes = 0;
  };
  for (const LogStore::Entry& e : entries) {
    LogEntryWire w;
    w.wall_ms = e.wall_ms;
    w.level = e.level;
    w.module_hash = e.module_hash;
    w.text = e.text;
    pending_bytes += e.text.size() + 24;
    pending.push_back(std::move(w));
    if (pending_bytes >= opt_.fetch_chunk_max_bytes) flush();
  }
  flush();
  *reason = "logs=" + std::to_string(entries.size()) + ",chunks=" + std::to_string(chunks);
  return true;
}

void KernelCore::SubmitControlEnvelope(const void* data, size_t len) {
  if (opt_.control_key.empty() || opt_.control_key == "imobs-dev-key" ||
      data == nullptr || len == 0 || len > 64 * 1024) return;
  const auto* p = static_cast<const uint8_t*>(data);
  std::vector<uint8_t> buf(p, p + len);
  std::lock_guard<std::mutex> lk(control_mu_);
  if (control_inbox_.size() < 64) control_inbox_.push_back(std::move(buf));
}

// 自愈事件回流观测（铁律）：力竭/熔断/静默是 P0（体系级强信号），其余 P1。
void KernelCore::OnHealEvent(const HealEvent& e) {
  HealRecordWire rec;
  rec.scene_id = e.scene_id;
  rec.from = static_cast<uint8_t>(e.from);
  rec.to = static_cast<uint8_t>(e.to);
  rec.ladder_index = e.ladder_index;
  rec.reason = static_cast<uint8_t>(e.reason);
  rec.ts_ms = e.ts_ms;
  rec.action = e.action;
  std::vector<uint8_t> buf;
  EncodeHealRecord(rec, &buf);
  const bool p0 = e.reason == HealReason::kExhaustedLadder ||
                  e.reason == HealReason::kBreakerTrip || e.reason == HealReason::kKillSwitch;
  pipeline_->Submit(p0 ? Priority::kP0 : Priority::kP1,
                    std::string_view(reinterpret_cast<const char*>(buf.data()), buf.size()));
}

void KernelCore::DispatchHealthOp(const Event& ev) {
  switch (static_cast<HealthOp>(ev.stage)) {
    case HealthOp::kSignal:
      healer_->SetSignal(ev.key, ev.value != 0);
      break;
    case HealthOp::kGlobalOutage:
      healer_->SetGlobalOutage(ev.value != 0);
      break;
    case HealthOp::kPrecondition:
      healer_->SetPrecondition(ev.key, ev.value != 0);
      break;
    case HealthOp::kMute:
      healer_->MuteScene(ev.key, ev.value);
      break;
    case HealthOp::kUnmute:
      healer_->UnmuteScene(ev.key);
      break;
  }
}

void KernelCore::Dispatch(const Event& ev) {
  switch (static_cast<EventKind>(ev.kind)) {
    case EventKind::kMetric:
      metrics_.Apply(ev);
      break;

    case EventKind::kTrace: {
      // 采样决策以 msg_id 为键：同一条消息的全部节点同进同出，轨迹不半残。
      const uint32_t permille = policy_->EffectiveTracePermille();
      if (permille < 1000 &&
          Fnv1a64(&ev.entity_id, sizeof(ev.entity_id)) % 1000 >= permille) {
        ++trace_sampled_out_;
        break;
      }
      if (trace_raw_.empty()) trace_first_ms_ = MonoMs();
      trace_raw_.push_back(ev);
      if (trace_raw_.size() >= opt_.trace_chunk_max_events) FlushTraceChunk();
      break;
    }

    case EventKind::kEvent: {
      // 约定：kEvent 的 stage 字节承载级别（0=P0 / 1=P1 / 2=P2）。
      EventRecordWire rec{ev.stage, ev.key, ev.ts_mono_ns, ev.attr_a, ev.attr_b};
      std::vector<uint8_t> buf;
      EncodeEventRecord(rec, &buf);
      const Priority pri = ev.stage == 0   ? Priority::kP0
                           : ev.stage == 1 ? Priority::kP1
                                           : Priority::kP2;
      pipeline_->Submit(pri,
                        std::string_view(reinterpret_cast<const char*>(buf.data()), buf.size()));
      break;
    }

    case EventKind::kHealth:
      DispatchHealthOp(ev);  // 域信号/控制面指令 → 健康域（消费线程私有）
      break;

    default:
      break;  // 未知类型：静默丢弃（门面层构造事件，正常不可达）
  }
}

void KernelCore::FlushTraceChunk() {
  if (trace_raw_.empty()) return;
  // base 取块内最小 ts：分片环跨分片合并后事件可能轻微乱序，min 保证 delta 无截断。
  uint64_t base = trace_raw_[0].ts_mono_ns;
  for (const Event& ev : trace_raw_) base = std::min(base, ev.ts_mono_ns);
  std::vector<TraceEventWire> events;
  events.reserve(trace_raw_.size());
  for (const Event& ev : trace_raw_) events.push_back(TraceFromEvent(ev, base));
  std::vector<uint8_t> buf;
  EncodeTraceChunk(base, events, &buf);
  pipeline_->Submit(Priority::kP1,
                    std::string_view(reinterpret_cast<const char*>(buf.data()), buf.size()));
  trace_raw_.clear();
}

void KernelCore::MaybeSnapshot(bool force) {
  const uint64_t now = MonoMs();
  if (!force && now - last_snapshot_ms_ < opt_.metrics_snapshot_interval_ms) return;
  last_snapshot_ms_ = now;
  auto rows = metrics_.SnapshotAndReset();
  if (rows.empty()) return;  // 空窗口不产生批（省流量，也让快照周期语义 = "有货才发车"）
  std::vector<uint8_t> buf;
  EncodeMetricsChunk(rows, &buf);
  pipeline_->Submit(Priority::kP2,
                    std::string_view(reinterpret_cast<const char*>(buf.data()), buf.size()));
}

size_t KernelCore::DrainGeneric() {
  madobs_event_v1 ev{}; size_t count=0;
  while(count<GenericEventQueue::kCapacity && generic_ring_.Pop(ev)) {
    std::vector<uint8_t> record; EncodeGenericEvent(ev,&record);
    const auto priority=ev.severity>=4 ? Priority::kP0 : Priority::kP1;
    pipeline_->Submit(priority,std::string_view(reinterpret_cast<const char*>(record.data()),record.size()),true);
    ++count;
  }
  generic_drained_.fetch_add(count,std::memory_order_relaxed); return count;
}

size_t KernelCore::Step() {
  const bool fg = foreground_.load(std::memory_order_relaxed);
  if (fg != fg_applied_) {
    pipeline_->SetForeground(fg);
    fg_applied_ = fg;
  }

  // 控制面收件箱：验签应用（策略/命令），随后策略参数落到管线
  std::vector<std::vector<uint8_t>> inbox;
  {
    std::lock_guard<std::mutex> lk(control_mu_);
    inbox.swap(control_inbox_);
  }
  for (const auto& env : inbox) policy_->ApplyEnvelope(env.data(), env.size());
  policy_->Tick();
  const PolicyBundle& eff = policy_->effective();
  pipeline_->ApplyPolicy(eff.day_quota_bytes, eff.p1_flush_interval_ms);

  Event buf[kDrainBatch];
  size_t total = 0;
  while (total < kMaxDrainPerStep) {
    const size_t n = ring_.PopBatch(buf, kDrainBatch);
    if (n == 0) break;
    for (size_t i = 0; i < n; ++i) Dispatch(buf[i]);
    total += n;
  }

  if (!trace_raw_.empty() && MonoMs() - trace_first_ms_ >= opt_.trace_chunk_max_age_ms) {
    FlushTraceChunk();
  }
  DrainLogInbox();  // 日志落盘（环形本地全都记；上行只走回捞命令）
  MaybeSnapshot(/*force=*/false);
  if (eff.healer_enabled) {
    healer_->Tick();  // MAPE 环步进；总开关关闭 = 不开新疗程（粗粒度 kill switch）
  }
  const size_t generic_count=DrainGeneric();
  pipeline_->Poll();
  generic_wal_dropped_.store(pipeline_->stats().generic_records_dropped,std::memory_order_relaxed);

  drained_ += total;
  return total + generic_count;
}

void KernelCore::Shutdown() {
  Event buf[kDrainBatch];
  for (;;) {
    const size_t n = ring_.PopBatch(buf, kDrainBatch);
    if (n == 0) break;
    for (size_t i = 0; i < n; ++i) Dispatch(buf[i]);
    drained_ += n;
  }
  DrainGeneric();
  FlushTraceChunk();
  DrainLogInbox();
  MaybeSnapshot(/*force=*/true);
  pipeline_->FlushAll();
  pipeline_->Poll();
  generic_wal_dropped_.store(pipeline_->stats().generic_records_dropped,std::memory_order_relaxed);
}

KernelCore::SelfStats KernelCore::self_stats() const {
  return SelfStats{drained_, ring_.dropped(), metrics_.unknown_key_dropped(),
                   trace_sampled_out_, log_inbox_dropped_.load(std::memory_order_relaxed)};
}

// ============================ ObsKernel（线程壳） ============================

std::unique_ptr<ObsKernel> ObsKernel::Start(const std::string& dir, IChannel* channel,
                                            const SchemaRegistry* schema,
                                            const KernelOptions& opt, std::string* err) {
  std::unique_ptr<ObsKernel> k(new ObsKernel());
  k->clock_ = &RealClock::Instance();
  k->opt_ = opt;
  k->core_ = KernelCore::Open(dir, k->clock_, channel, schema, opt, err);
  if (!k->core_) return nullptr;
  k->worker_ = std::thread([kernel = k.get()] {
    while (!kernel->stop_.load(std::memory_order_acquire)) {
      const size_t n = kernel->core_->Step();
      if (n == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kernel->opt_.poll_interval_ms));
      }
    }
    kernel->core_->Shutdown();  // 终局冲刷在消费线程内完成：单消费者不变量始终成立
  });
  return k;
}

ObsKernel::~ObsKernel() { Stop(); }

void ObsKernel::Stop() {
  if (worker_.joinable()) {
    stop_.store(true, std::memory_order_release);
    worker_.join();
  }
}

void ObsKernel::AddCounter(uint64_t key, uint16_t label_idx, uint64_t delta) {
  Event ev{};
  ev.ts_mono_ns = clock_->MonoNanos();
  ev.key = key;
  ev.value = delta;
  ev.label_idx = label_idx;
  ev.kind = static_cast<uint8_t>(EventKind::kMetric);
  core_->Emit(ev);
}

void ObsKernel::RecordHistogram(uint64_t key, uint16_t label_idx, uint64_t value) {
  Event ev{};
  ev.ts_mono_ns = clock_->MonoNanos();
  ev.key = key;
  ev.value = value;
  ev.label_idx = label_idx;
  ev.kind = static_cast<uint8_t>(EventKind::kMetric);
  core_->Emit(ev);
}

void ObsKernel::TraceStage(uint64_t msg_id, uint8_t stage, uint8_t status, uint32_t attr,
                           uint64_t value) {
  Event ev{};
  ev.ts_mono_ns = clock_->MonoNanos();
  ev.entity_id = msg_id;
  ev.stage = stage;
  ev.status = status;
  ev.attr_a = attr;
  ev.value = value;
  ev.kind = static_cast<uint8_t>(EventKind::kTrace);
  core_->Emit(ev);
}

void ObsKernel::EmitEvent(uint64_t key, uint8_t level, uint32_t a, uint32_t b) {
  Event ev{};
  ev.ts_mono_ns = clock_->MonoNanos();
  ev.key = key;
  ev.stage = level;
  ev.attr_a = a;
  ev.attr_b = b;
  ev.kind = static_cast<uint8_t>(EventKind::kEvent);
  core_->Emit(ev);
}

void ObsKernel::EmitHealthOp(HealthOp op, uint64_t key, uint64_t value) {
  Event ev{};
  ev.ts_mono_ns = clock_->MonoNanos();
  ev.key = key;
  ev.value = value;
  ev.stage = static_cast<uint8_t>(op);
  ev.kind = static_cast<uint8_t>(EventKind::kHealth);
  core_->Emit(ev);
}

void ObsKernel::SetHealthSignal(uint64_t signal_id, bool active) {
  EmitHealthOp(HealthOp::kSignal, signal_id, active ? 1 : 0);
}

void ObsKernel::SetGlobalOutage(bool outage) {
  EmitHealthOp(HealthOp::kGlobalOutage, 0, outage ? 1 : 0);
}

void ObsKernel::MuteHealScene(uint64_t scene_id, uint64_t ttl_ms) {
  EmitHealthOp(HealthOp::kMute, scene_id, ttl_ms);
}

void ObsKernel::SetHealPrecondition(uint64_t scene_id, bool ready) {
  EmitHealthOp(HealthOp::kPrecondition, scene_id, ready ? 1 : 0);
}

}  // namespace imobs
