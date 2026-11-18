#include "imobs/pipeline.h"

#include <algorithm>

#include "imobs/codec.h"
#include "imobs/wire.h"

namespace imobs {
namespace {

constexpr uint8_t kBatchMagic[4] = {'I', 'M', 'B', '1'};
constexpr uint8_t kBatchMagicZ[4] = {'I', 'M', 'B', 'Z'};

const char* PartFile(Priority pri) {
  switch (pri) {
    case Priority::kP0: return "/p0.wal";
    case Priority::kP1: return "/p1.wal";
    default: return "/p2.wal";
  }
}

}  // namespace

std::unique_ptr<UploadPipeline> UploadPipeline::Open(const std::string& dir, IClock* clock,
                                                     IChannel* channel,
                                                     const PipelineOptions& opt,
                                                     std::string* err) {
  std::unique_ptr<UploadPipeline> p(new UploadPipeline());
  p->clock_ = clock;
  p->channel_ = channel;
  p->opt_ = opt;
  p->rng_.seed(opt.rng_seed);
  for (size_t i = 0; i < kPriorityCount; ++i) {
    p->parts_[i].wal = WalRing::Open(dir + PartFile(static_cast<Priority>(i)), opt.wal[i], err);
    if (!p->parts_[i].wal) return nullptr;
    p->parts_[i].last_flush_ms = p->MonoMs();
  }
  return p;
}

bool UploadPipeline::Submit(Priority pri, std::string_view record, bool generic) {
  if (record.empty()) return false;
  Part& part = parts_[static_cast<size_t>(pri)];
  WireWriter w(&part.buf);
  w.Varint(record.size());
  w.Bytes(record.data(), record.size());
  ++part.buf_records;
  if (generic) ++part.generic_records;
  if (pri == Priority::kP0) {
    MaybeFlush(pri, /*force=*/true);  // P0：逐条即时成批 + 持久化（sync_every_append）
  }
  return true;
}

// 批格式：[fixed64 batch_id][4B magic][u8 pri][fixed64 anchor_wall_ms]
//         [fixed64 anchor_mono_ns][varint n_records][记录帧 ×n]
// batch_id = Fnv1a64(batch_id 之后的全部字节)——内容派生：崩溃后重发的
// 是同一段字节，天然同 id，服务端按 id 去重即得"补报幂等"。
void UploadPipeline::MaybeFlush(Priority pri, bool force) {
  Part& part = parts_[static_cast<size_t>(pri)];
  if (part.buf_records == 0) return;
  const uint64_t now = MonoMs();
  if (!force) {
    const bool by_bytes =
        pri == Priority::kP1 && part.buf.size() >= opt_.p1_flush_bytes;
    const uint64_t interval =
        pri == Priority::kP1 ? opt_.p1_flush_interval_ms : opt_.p2_flush_interval_ms;
    const bool by_time = now - part.last_flush_ms >= interval;
    if (!by_bytes && !by_time) return;
  }

  // 压缩判定：P0 永远明文（单条即发 + 崩溃路径求简）；P1/P2 够大才压、
  // 压不动就回退明文——batch_id 对最终字节取哈希，两种形态幂等语义一致。
  std::vector<uint8_t> zbuf;
  const bool zed = opt_.compress && pri != Priority::kP0 &&
                   part.buf.size() >= opt_.compress_min_bytes &&
                   DeflateBytes(part.buf.data(), part.buf.size(), &zbuf);

  std::vector<uint8_t> body;
  body.reserve((zed ? zbuf.size() : part.buf.size()) + 40);
  WireWriter w(&body);
  w.Bytes(zed ? kBatchMagicZ : kBatchMagic, 4);
  w.U8(static_cast<uint8_t>(pri));
  w.Fixed64(static_cast<uint64_t>(clock_->WallMillis()));  // 批级时钟锚点（架构 §6）
  w.Fixed64(clock_->MonoNanos());
  w.Varint(part.buf_records);
  if (zed) {
    w.Varint(part.buf.size());  // raw_len：解压护栏 + 精确校验
    w.Bytes(zbuf.data(), zbuf.size());
  } else {
    w.Bytes(part.buf.data(), part.buf.size());
  }

  std::vector<uint8_t> full;
  full.reserve(body.size() + 8);
  WireWriter w2(&full);
  w2.Fixed64(Fnv1a64(body.data(), body.size()));
  w2.Bytes(body.data(), body.size());

  if (part.wal->Append(full.data(), static_cast<uint32_t>(full.size()))) {
    ++stats_.batches_built;
  } else {
    stats_.generic_records_dropped += part.generic_records;
    ++stats_.submit_dropped;  // P0 分区满（永不覆盖）：满溢即强信号，WAL 侧已另有账
  }
  part.buf.clear();
  part.buf_records = 0;
  part.generic_records = 0;
  part.last_flush_ms = now;
}

bool UploadPipeline::QuotaExhausted() {
  const int64_t day = clock_->WallMillis() / 86'400'000;
  if (day != quota_day_) {
    quota_day_ = day;
    quota_used_ = 0;
  }
  return quota_used_ >= opt_.day_quota_bytes;
}

void UploadPipeline::TrySend(Priority pri) {
  Part& part = parts_[static_cast<size_t>(pri)];
  if (part.in_flight) return;
  if (!foreground_ && pri != Priority::kP0) return;  // 后台降级：仅 P0（架构 §9 矩阵）
  const uint64_t now = MonoMs();
  if (now < part.next_try_ms) return;                // 退避窗口内
  if (pri != Priority::kP0 && QuotaExhausted()) {
    ++stats_.quota_deferred;                          // 预算耗尽：P1/P2 押在 WAL 里
    return;
  }

  bool found = false;
  WalRing::Cursor cursor;
  part.wal->ScanPending([&](const WalRing::Record& r) {
    cursor = r.end;
    part.sending.assign(r.payload.data(), r.payload.size());
    found = true;
    return false;  // 只取最早一批：分区内串行，顺序即 WAL 顺序
  });
  if (!found) return;

  uint64_t batch_id = 0;
  WireReader rd(part.sending.data(), part.sending.size());
  if (!rd.Fixed64(&batch_id)) {  // 批头损坏：跳过该批（消费水位越过它）并记账
    stats_.generic_records_dropped += part.generic_records;
    ++stats_.submit_dropped;
    part.wal->MarkConsumed(cursor);
    return;
  }

  part.in_flight = true;
  ++stats_.sends;
  quota_used_ += part.sending.size();  // 发出即计费：网络字节已经花掉，不看成败
  // 回调可在任意线程到达：只做入箱（shared_ptr 续命，晚于析构的孤儿结果安全丢弃）
  std::shared_ptr<CompletionInbox> inbox = inbox_;
  channel_->Send(pri, batch_id, part.sending, [inbox, pri, cursor](IChannel::Result r) {
    std::lock_guard<std::mutex> lk(inbox->mu);
    inbox->items.push_back({pri, cursor, r.ok});
  });
}

void UploadPipeline::DrainCompletions() {
  std::vector<Completion> items;
  {
    std::lock_guard<std::mutex> lk(inbox_->mu);
    items.swap(inbox_->items);
  }
  for (const Completion& c : items) OnSendResult(c.pri, c.cursor, c.ok);
}

void UploadPipeline::OnSendResult(Priority pri, WalRing::Cursor cursor, bool ok) {
  Part& part = parts_[static_cast<size_t>(pri)];
  part.in_flight = false;
  if (ok) {
    ++stats_.acks;
    part.attempts = 0;
    part.next_try_ms = 0;
    part.wal->MarkConsumed(cursor);  // ACK 才推进水位：at-least-once + 服务端按 id 去重
    return;
  }
  ++stats_.fails;
  ++part.attempts;
  const uint32_t shift = std::min(part.attempts - 1, 6u);
  const uint64_t window = std::min(opt_.retry_cap_ms, opt_.retry_base_ms << shift);
  const uint64_t jitter = 1 + rng_() % window;  // full-jitter：防重试波峰打服务端
  part.next_try_ms = MonoMs() + jitter;
}

void UploadPipeline::Poll() {
  DrainCompletions();  // 异步通道的结果：上一轮之后到达的
  for (size_t i = 0; i < kPriorityCount; ++i) {
    const auto pri = static_cast<Priority>(i);
    MaybeFlush(pri, /*force=*/false);
    TrySend(pri);
  }
  DrainCompletions();  // 同步通道的结果：本轮内到达的（保持既有可观测时序）
}

void UploadPipeline::FlushAll() {
  for (size_t i = 0; i < kPriorityCount; ++i) {
    MaybeFlush(static_cast<Priority>(i), /*force=*/true);
  }
}

uint64_t UploadPipeline::PendingBatches(Priority pri) const {
  uint64_t n = 0;
  parts_[static_cast<size_t>(pri)].wal->ScanPending([&](const WalRing::Record&) {
    ++n;
    return true;
  });
  return n;
}

}  // namespace imobs
