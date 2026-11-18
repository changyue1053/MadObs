#include <unistd.h>
#include <zlib.h>

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <string>

#include "imobs/codec.h"
#include "imobs/generic_events.h"
#include "imobs/wire.h"
#include "madobs/private.h"
#include "madobs/runtime.h"
static bool fail_compress = false, fail_inflate = false;
extern "C" int __real_compress2(Bytef*, uLongf*, const Bytef*, uLong, int);
extern "C" int __real_uncompress(Bytef*, uLongf*, const Bytef*, uLong);
extern "C" int __wrap_compress2(Bytef* d, uLongf* n, const Bytef* s, uLong len,
                                int level) {
  return fail_compress ? Z_MEM_ERROR : __real_compress2(d, n, s, len, level);
}
extern "C" int __wrap_uncompress(Bytef* d, uLongf* n, const Bytef* s,
                                 uLong len) {
  return fail_inflate ? Z_MEM_ERROR : __real_uncompress(d, n, s, len);
}
static bool fail_write = false, fail_sync = false;
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t n) {
  if (fail_write) {
    errno = ENOSPC;
    return -1;
  }
  return __real_write(fd, data, n);
}
extern "C" int __wrap_fsync(int fd) {
  if (fail_sync) {
    errno = ENOSPC;
    return -1;
  }
  return __real_fsync(fd);
}
static void Send(void*, uint8_t, uint64_t, const uint8_t*, size_t,
                 madobs_send_done_fn done, void* ctx) {
  done(ctx, 1);
}
int main() {
  madobs_event_rule_v1 rule{};
  madobs_privacy_policy_v1 policy{sizeof(policy), 1, 1, &rule};
  madobs_event_v1 event{};
  event.struct_size = sizeof(event);
  event.abi_version = 1;
  std::vector<uint8_t> record, frames, zipped, body, full;
  imobs::EncodeGenericEvent(event, &record);
  imobs::WireWriter f(&frames);
  for (unsigned i = 0; i < 100; ++i) {
    f.Varint(record.size());
    f.Bytes(record.data(), record.size());
  }
  assert(imobs::DeflateBytes(frames.data(), frames.size(), &zipped));
  imobs::WireWriter b(&body);
  b.Bytes("IMBZ", 4);
  b.U8(1);
  b.Fixed64(0);
  b.Fixed64(0);
  b.Varint(100);
  b.Varint(frames.size());
  b.Bytes(zipped.data(), zipped.size());
  imobs::WireWriter w(&full);
  w.Fixed64(imobs::Fnv1a64(body.data(), body.size()));
  w.Bytes(body.data(), body.size());
  assert(madobs_check_private_batch(full.data(), full.size(), &policy) ==
         MADOBS_PRIVATE_BATCH_OK);
  fail_compress = true;
  assert(madobs_check_private_batch(full.data(), full.size(), &policy) ==
         MADOBS_PRIVATE_RESOURCE_FAILURE);
  assert(!madobs_validate_private_batch(full.data(), full.size(), &policy));
  fail_compress = false;
  fail_inflate = true;
  assert(madobs_check_private_batch(full.data(), full.size(), &policy) ==
         MADOBS_PRIVATE_RESOURCE_FAILURE);
  assert(!madobs_validate_private_batch(full.data(), full.size(), &policy));
  fail_inflate = false;
  assert(madobs_check_private_batch(full.data(), full.size(), &policy) ==
         MADOBS_PRIVATE_BATCH_OK);
  char parent[] = "/tmp/madobs_manifest_fault_XXXXXX";
  assert(mkdtemp(parent));
  const auto partial = std::string(parent) + "/partial";
  madobs_private_open_result reason = MADOBS_PRIVATE_OPEN_OK;
  fail_write = true;
  assert(!madobs_private_open_ex(partial.c_str(), Send, nullptr, &policy,
                                 &reason));
  assert(reason == MADOBS_PRIVATE_OPEN_WAL_IO_ERROR);
  fail_write = false;
  assert(!madobs_private_open_ex(partial.c_str(), Send, nullptr, &policy,
                                 &reason));
  assert(reason == MADOBS_PRIVATE_OPEN_POLICY_OR_MANIFEST_MISMATCH);
  const auto sync = std::string(parent) + "/sync";
  fail_sync = true;
  assert(
      !madobs_private_open_ex(sync.c_str(), Send, nullptr, &policy, &reason));
  assert(reason == MADOBS_PRIVATE_OPEN_WAL_IO_ERROR);
  // A valid cached manifest must not bypass a repeated sync failure on reopen.
  assert(
      !madobs_private_open_ex(sync.c_str(), Send, nullptr, &policy, &reason));
  assert(reason == MADOBS_PRIVATE_OPEN_WAL_IO_ERROR);
  fail_sync = false;
  auto recovered =
      madobs_private_open_ex(sync.c_str(), Send, nullptr, &policy, &reason);
  assert(recovered && reason == MADOBS_PRIVATE_OPEN_OK);
  assert(madobs_private_destroy(recovered) == MADOBS_OK);
  full.back() ^= 1;
  assert(madobs_check_private_batch(full.data(), full.size(), &policy) ==
         MADOBS_PRIVATE_BAD_WIRE);
}
