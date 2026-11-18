#include "imobs/generic_events.h"

#include <limits>

#include "imobs/wire.h"
#include "madobs/runtime.h"
namespace imobs {
bool ValidGenericEvent(const madobs_event_v1* e) {
  return e && e->struct_size >= sizeof(*e) && e->abi_version == 1 &&
         e->attribute_count <= 8 && e->severity <= 5 && e->reserved == 0;
}
void EncodeGenericEvent(const madobs_event_v1& e, std::vector<uint8_t>* out) {
  WireWriter w(out);
  w.U8('G');
  w.U8(1);
  w.Varint(e.sdk_id);
  w.Varint(e.sdk_version);
  w.Varint(e.event_id);
  w.Varint(e.severity);
  w.Fixed64(e.monotonic_ns);
  w.Varint(e.attribute_count);
  for (uint32_t i = 0; i < e.attribute_count; ++i) {
    w.Varint(e.attributes[i].key);
    w.Varint(e.attributes[i].enum_value);
  }
}
bool DecodeGenericEvent(const uint8_t* data, size_t len, madobs_event_v1* out) {
  if (!data || !out || len < 2 || len > 128) return false;
  WireReader r(data, len);
  uint8_t tag = 0, version = 0;
  if (!r.U8(&tag) || tag != 'G' || !r.U8(&version) || version != 1)
    return false;
  madobs_event_v1 e{};
  e.struct_size = sizeof(e);
  e.abi_version = 1;
  // u32 decoder rejects overlong/overflow values (including tenth-byte wrap).
  auto u32 = [&](uint32_t& v) {
    uint32_t result = 0;
    for (unsigned i = 0; i < 5; ++i) {
      uint8_t b = 0;
      if (!r.U8(&b) || (i == 4 && (b & 0xf0))) return false;
      result |= uint32_t(b & 0x7f) << (i * 7);
      if (!(b & 0x80)) {
        v = result;
        return true;
      }
    }
    return false;
  };
  if (!u32(e.sdk_id) || !u32(e.sdk_version) || !u32(e.event_id) ||
      !u32(e.severity) || !r.Fixed64(&e.monotonic_ns) ||
      !u32(e.attribute_count) || e.attribute_count > 8)
    return false;
  for (uint32_t i = 0; i < e.attribute_count; ++i)
    if (!u32(e.attributes[i].key) || !u32(e.attributes[i].enum_value))
      return false;
  if (r.remaining() != 0 || !ValidGenericEvent(&e)) return false;
  *out = e;
  return true;
}
}  // namespace imobs
madobs_status_t madobs_decode_event_record(const uint8_t* data, size_t len,
                                           madobs_event_v1* out,
                                           size_t out_size) {
  if (out_size < sizeof(madobs_event_v1)) return MADOBS_E_INVALID_ARG;
  return imobs::DecodeGenericEvent(data, len, out) ? MADOBS_OK
                                                   : MADOBS_E_INVALID_ARG;
}
