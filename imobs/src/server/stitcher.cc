#include "imobs/stitcher.h"

#include "imobs/batch_view.h"
#include "imobs/records.h"

namespace imobs {

size_t TraceStitcher::IngestBatch(const void* data, size_t len) {
  BatchView view;
  if (!ParseBatchView(data, len, &view)) return 0;
  size_t added = 0;
  for (const std::string_view rec : view.records) {
    if (rec.empty() || static_cast<uint8_t>(rec[0]) != kRecTrace) continue;
    uint64_t base = 0;
    std::vector<TraceEventWire> events;
    if (!DecodeTraceChunk(reinterpret_cast<const uint8_t*>(rec.data()), rec.size(), &base,
                          &events)) {
      continue;  // 单块损坏跳过：拼接不因一块脏数据整体失败
    }
    for (const TraceEventWire& ev : events) {
      Journey& j = journeys_[ev.msg_id];
      j.stages.insert(ev.stage);
      ++j.events;
      ++trace_events_;
      ++added;
    }
  }
  return added;
}

std::vector<uint8_t> TraceStitcher::StagesOf(uint64_t msg_id) const {
  const auto it = journeys_.find(msg_id);
  if (it == journeys_.end()) return {};
  return std::vector<uint8_t>(it->second.stages.begin(), it->second.stages.end());
}

bool TraceStitcher::Complete(uint64_t msg_id, const std::vector<uint8_t>& required) const {
  const auto it = journeys_.find(msg_id);
  if (it == journeys_.end()) return false;
  for (const uint8_t s : required) {
    if (it->second.stages.count(s) == 0) return false;
  }
  return true;
}

std::vector<uint64_t> TraceStitcher::JourneyIds(size_t limit) const {
  std::vector<uint64_t> out;
  out.reserve(std::min(limit, journeys_.size()));
  for (const auto& [id, j] : journeys_) {
    if (out.size() >= limit) break;
    out.push_back(id);
  }
  return out;
}

}  // namespace imobs
