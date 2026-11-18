#include "imobs/wire.h"

namespace imobs {

void EncodeTraceEvent(const TraceEventWire& ev, std::vector<uint8_t>* out) {
  WireWriter w(out);
  w.Fixed64(ev.msg_id);
  w.U8(ev.stage);
  w.U8(ev.status);
  w.Varint(ev.ts_delta_ns);
  w.Varint(ev.n_attrs);
  for (uint8_t i = 0; i < ev.n_attrs; ++i) {
    w.Varint(ev.attrs[i].tag);
    w.Varint(ev.attrs[i].value);
  }
}

bool DecodeTraceEvent(WireReader& r, TraceEventWire* out) {
  TraceEventWire ev;
  uint64_t n = 0;
  if (!r.Fixed64(&ev.msg_id) || !r.U8(&ev.stage) || !r.U8(&ev.status) ||
      !r.Varint(&ev.ts_delta_ns) || !r.Varint(&n)) {
    return false;
  }
  if (n > 4) return false;  // 契约上限：越界即拒绝，不是截断
  ev.n_attrs = static_cast<uint8_t>(n);
  for (uint8_t i = 0; i < ev.n_attrs; ++i) {
    uint64_t tag = 0;
    if (!r.Varint(&tag) || tag > std::numeric_limits<uint32_t>::max()) return false;
    ev.attrs[i].tag = static_cast<uint32_t>(tag);
    if (!r.Varint(&ev.attrs[i].value)) return false;
  }
  *out = ev;
  return true;
}

TraceEventWire TraceFromEvent(const Event& ev, uint64_t base_ts_ns) {
  TraceEventWire out;
  out.msg_id = ev.entity_id;
  out.stage = ev.stage;
  out.status = ev.status;
  out.ts_delta_ns = ev.ts_mono_ns >= base_ts_ns ? ev.ts_mono_ns - base_ts_ns : 0;
  auto add = [&out](uint32_t tag, uint64_t value) {
    if (value != 0 && out.n_attrs < 4) {
      out.attrs[out.n_attrs++] = {tag, value};
    }
  };
  add(kAttrConnLow, ev.attr_a);
  add(kAttrExtra, ev.attr_b);
  add(kAttrLabelIdx, ev.label_idx);
  add(kAttrValue, ev.value);
  return out;
}

}  // namespace imobs
