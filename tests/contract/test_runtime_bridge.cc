#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "imobs/batch_view.h"
#include "imobs/codec.h"
#include "imobs/generic_events.h"
#include "imobs/obs_kernel.h"
#include "imobs/wire.h"
#include "imobs_test.h"
#include "madobs/privacy.h"
#include "madobs/runtime.h"
namespace {
struct Capture {
  std::mutex mu;
  std::vector<madobs_event_v1> events;
};
void Send(void* ctx, uint8_t, uint64_t, const uint8_t* bytes, size_t len,
          madobs_send_done_fn done, void* dc) {
  auto& c = *static_cast<Capture*>(ctx);
  imobs::BatchView batch;
  if (imobs::ParseBatchView(bytes, len, &batch)) {
    std::lock_guard<std::mutex> l(c.mu);
    for (auto rec : batch.records) {
      madobs_event_v1 e{};
      if (madobs_decode_event_record(
              reinterpret_cast<const uint8_t*>(rec.data()), rec.size(), &e,
              sizeof(e)) == MADOBS_OK)
        c.events.push_back(e);
    }
  }
  done(dc, 1);
}
madobs_event_v1 Event() {
  madobs_event_v1 e{};
  e.struct_size = sizeof(e);
  e.abi_version = 1;
  e.sdk_id = 7;
  e.sdk_version = 23;
  e.event_id = 51;
  e.severity = 4;
  e.monotonic_ns = 987654321;
  e.attribute_count = 8;
  for (unsigned i = 0; i < 8; ++i) e.attributes[i] = {i, 100 + i};
  return e;
}
}  // namespace
TEST(Bridge, AllFieldsReachHostExporterThroughMadAbi) {
  Capture c;
  char dir[] = "/tmp/madobs_bridge_XXXXXX";
  ASSERT_TRUE(mkdtemp(dir) != nullptr);
  auto h = madobs_create(dir, Send, &c);
  ASSERT_TRUE(h != 0);
  auto e = Event();
  EXPECT_EQ(madobs_emit_generic_event(h, &e), MADOBS_E_BAD_STATE);
  ASSERT_EQ(madobs_start(h), MADOBS_OK);
  madobs_observer_binding_v1 binding{};
  madobs_observer_v1 observer{};
  ASSERT_EQ(madobs_observer_for_runtime(h, &binding, &observer), MADOBS_OK);
  EXPECT_EQ(madobs_observer_emit(&observer, &e), MADOBS_EMITTED);
  ASSERT_EQ(madobs_stop(h), MADOBS_OK);
  EXPECT_EQ(madobs_start(h), MADOBS_E_BAD_STATE);
  madobs_generic_stats_v1 stats{};
  ASSERT_EQ(madobs_get_generic_stats(h, &stats), MADOBS_OK);
  EXPECT_EQ(stats.enqueued, 1u);
  EXPECT_EQ(stats.drained, 1u);
  EXPECT_EQ(stats.queue_dropped, 0u);
  ASSERT_EQ(c.events.size(), size_t{1});
  const auto& got = c.events[0];
  EXPECT_EQ(got.sdk_id, e.sdk_id);
  EXPECT_EQ(got.sdk_version, e.sdk_version);
  EXPECT_EQ(got.event_id, e.event_id);
  EXPECT_EQ(got.severity, e.severity);
  EXPECT_EQ(got.monotonic_ns, e.monotonic_ns);
  EXPECT_EQ(got.attribute_count, 8u);
  for (unsigned i = 0; i < 8; ++i) {
    EXPECT_EQ(got.attributes[i].key, i);
    EXPECT_EQ(got.attributes[i].enum_value, 100 + i);
  }
  EXPECT_EQ(madobs_emit_generic_event(h, &e), MADOBS_E_BAD_STATE);
  ASSERT_EQ(madobs_destroy(h), MADOBS_OK);
  EXPECT_EQ(madobs_destroy(h), MADOBS_E_INVALID_HANDLE);
  EXPECT_EQ(madobs_emit_generic_event(h, &e), MADOBS_E_INVALID_HANDLE);
  // Binding storage is still alive. A stale sequential runtime dispatch is
  // safely ignored.
  EXPECT_EQ(madobs_observer_emit(&observer, &e), MADOBS_EMITTED);
}
TEST(Bridge, BoundedQueueAndConcurrentProducers) {
  imobs::GenericEventQueue q;
  auto e = Event();
  for (unsigned i = 0; i < 1024; ++i) ASSERT_TRUE(q.Push(e));
  EXPECT_FALSE(q.Push(e));
  EXPECT_EQ(q.dropped.load(), 1u);
  for (unsigned i = 0; i < 1024; ++i) ASSERT_TRUE(q.Pop(e));
  EXPECT_FALSE(q.Pop(e));
  constexpr unsigned n = 10000, producers = 4;
  std::atomic<unsigned> active{producers};
  std::vector<unsigned char> expected(n * producers), seen(n * producers);
  std::vector<std::thread> threads;
  for (unsigned p = 0; p < producers; ++p)
    threads.emplace_back([&, p] {
      auto v = Event();
      for (unsigned i = 0; i < n; ++i) {
        v.event_id = p * n + i;
        expected[v.event_id] = q.Push(v);
      }
      --active;
    });
  unsigned consumed = 0;
  bool duplicate = false;
  for (;;) {
    if (!q.Pop(e)) {
      if (active.load() == 0) break;
      std::this_thread::yield();
      continue;
    }
    if (e.event_id >= seen.size() || seen[e.event_id])
      duplicate = true;
    else
      seen[e.event_id] = 1;
    ++consumed;
  }
  for (auto& t : threads) t.join();
  // Drain a possible final published slot.
  while (q.Pop(e)) {
    if (seen[e.event_id]) duplicate = true;
    seen[e.event_id] = 1;
    ++consumed;
  }
  EXPECT_FALSE(duplicate);
  EXPECT_TRUE(expected == seen);
  EXPECT_EQ(q.enqueued.load(), uint64_t(consumed + 1024));
}
TEST(Bridge, DecoderBoundsAndMalformedInputs) {
  auto e = Event();
  std::vector<uint8_t> bytes;
  imobs::EncodeGenericEvent(e, &bytes);
  madobs_event_v1 out{};
  out.event_id = 999;
  for (size_t n = 0; n < bytes.size(); ++n)
    EXPECT_EQ(madobs_decode_event_record(bytes.data(), n, &out, sizeof(out)),
              MADOBS_E_INVALID_ARG);
  EXPECT_EQ(out.event_id, 999u);
  EXPECT_EQ(madobs_decode_event_record(bytes.data(), bytes.size(), &out,
                                       sizeof(out) - 1),
            MADOBS_E_INVALID_ARG);
  std::minstd_rand rng(17);
  std::vector<uint8_t> fuzz(129);
  for (unsigned i = 0; i < 10000; ++i) {
    for (auto& b : fuzz) b = rng();
    (void)madobs_decode_event_record(fuzz.data(), rng() % 130, &out,
                                     sizeof(out));
  }
}
TEST(Bridge, FailedExportSurvivesRuntimeRestartViaWal) {
  char dir[] = "/tmp/madobs_bridge_wal_XXXXXX";
  ASSERT_TRUE(mkdtemp(dir) != nullptr);
  auto fail = [](void*, uint8_t, uint64_t, const uint8_t*, size_t,
                 madobs_send_done_fn done, void* dc) { done(dc, 0); };
  auto h = madobs_create(dir, fail, nullptr);
  ASSERT_TRUE(h != 0);
  ASSERT_EQ(madobs_start(h), MADOBS_OK);
  auto e = Event();
  ASSERT_EQ(madobs_emit_generic_event(h, &e), MADOBS_OK);
  ASSERT_EQ(madobs_destroy(h), MADOBS_OK);
  Capture c;
  h = madobs_create(dir, Send, &c);
  ASSERT_TRUE(h != 0);
  ASSERT_EQ(madobs_start(h), MADOBS_OK);
  ASSERT_EQ(madobs_stop(h), MADOBS_OK);
  ASSERT_EQ(madobs_destroy(h), MADOBS_OK);
  ASSERT_TRUE(!c.events.empty());
  EXPECT_EQ(c.events.front().event_id, e.event_id);
}
TEST(Bridge, FullWalLossCountUsesActualPersistenceResult) {
  struct Failed : imobs::IChannel {
    void Send(imobs::Priority, uint64_t, std::string_view,
              SendCallback done) override {
      done({false});
    }
  } channel;
  imobs::VirtualClock clock;
  imobs::SchemaRegistry schema;
  imobs::KernelOptions opt;
  opt.pipeline.wal[0].segment_size = 512;
  opt.pipeline.wal[0].segment_count = 2;
  char dir[] = "/tmp/madobs_full_wal_XXXXXX";
  ASSERT_TRUE(mkdtemp(dir) != nullptr);
  std::string err;
  auto core =
      imobs::KernelCore::Open(dir, &clock, &channel, &schema, opt, &err);
  ASSERT_TRUE(core != nullptr);
  auto e = Event();
  for (unsigned i = 0; i < 100; ++i) ASSERT_TRUE(core->EmitGeneric(e));
  core->Step();
  EXPECT_TRUE(core->GenericWalDropped() > 0);
  EXPECT_EQ(core->GenericWalDropped(), core->pipeline().stats().submit_dropped);
  EXPECT_EQ(core->GenericDrained(), 100u);
}
TEST(Bridge, DelayedP1FlushAccountsRecordsNotBatches) {
  struct Failed : imobs::IChannel {
    void Send(imobs::Priority, uint64_t, std::string_view,
              SendCallback done) override {
      done({false});
    }
  } channel;
  imobs::VirtualClock clock;
  imobs::SchemaRegistry schema;
  imobs::KernelOptions opt;
  opt.pipeline.compress = false;
  opt.pipeline.wal[1].segment_size = 512;
  opt.pipeline.wal[1].segment_count = 2;
  char dir[] = "/tmp/madobs_p1_wal_XXXXXX";
  ASSERT_TRUE(mkdtemp(dir) != nullptr);
  std::string err;
  auto core =
      imobs::KernelCore::Open(dir, &clock, &channel, &schema, opt, &err);
  ASSERT_TRUE(core != nullptr);
  auto e = Event();
  e.severity = 1;
  for (unsigned i = 0; i < 100; ++i) ASSERT_TRUE(core->EmitGeneric(e));
  core->Step();
  EXPECT_EQ(core->GenericWalDropped(),
            0u);  // buffered, not yet a durable-success claim
  core->Shutdown();
  EXPECT_EQ(core->GenericWalDropped(),
            100u);  // one oversized batch, 100 lost records
  EXPECT_EQ(core->pipeline().stats().submit_dropped, 1u);
}
TEST(Bridge, PrivacyMinimizationSurvivesWalReplayToExporter) {
  char dir[] = "/tmp/madobs_private_wal_XXXXXX";
  ASSERT_TRUE(mkdtemp(dir) != nullptr);
  auto fail = [](void*, uint8_t, uint64_t, const uint8_t*, size_t,
                 madobs_send_done_fn done, void* dc) { done(dc, 0); };
  auto h = madobs_create(dir, fail, nullptr);
  ASSERT_TRUE(h != 0);
  ASSERT_EQ(madobs_start(h), MADOBS_OK);
  madobs_observer_binding_v1 binding{};
  madobs_observer_v1 observer{};
  ASSERT_EQ(madobs_observer_for_runtime(h, &binding, &observer), MADOBS_OK);
  auto e = Event();
  madobs_event_rule_v1 rule{};
  rule.sdk_id = e.sdk_id;
  rule.sdk_version = e.sdk_version;
  rule.event_id = e.event_id;
  rule.max_severity = 4;
  rule.attribute_count = 2;
  rule.attributes[0].key = 0;
  rule.attributes[0].value_count = 1;
  rule.attributes[0].values[0] = 100;
  rule.attributes[1].key = 1;
  rule.attributes[1].value_count = 1;
  rule.attributes[1].values[0] = 0;  // input 101 is rejected
  madobs_privacy_policy_v1 policy{sizeof(policy), MADOBS_ABI_VERSION, 1, &rule};
  EXPECT_EQ(madobs_observer_emit_private(&observer, &policy, &e),
            MADOBS_EMITTED);
  e.event_id = 999;
  EXPECT_EQ(madobs_observer_emit_private(&observer, &policy, &e),
            MADOBS_INVALID);
  ASSERT_EQ(madobs_stop(h), MADOBS_OK);
  madobs_generic_stats_v1 stats{};
  ASSERT_EQ(madobs_get_generic_stats(h, &stats), MADOBS_OK);
  EXPECT_EQ(stats.enqueued, 1u);
  ASSERT_EQ(madobs_destroy(h), MADOBS_OK);
  Capture c;
  h = madobs_create(dir, Send, &c);
  ASSERT_TRUE(h != 0);
  ASSERT_EQ(madobs_start(h), MADOBS_OK);
  ASSERT_EQ(madobs_stop(h), MADOBS_OK);
  ASSERT_EQ(madobs_destroy(h), MADOBS_OK);
  ASSERT_TRUE(!c.events.empty());
  for (const auto& got : c.events) {
    EXPECT_EQ(got.event_id, 51u);
    EXPECT_EQ(got.monotonic_ns, 0u);
    EXPECT_EQ(got.attribute_count, 1u);
    EXPECT_EQ(got.attributes[0].key, 0u);
    EXPECT_EQ(got.attributes[0].enum_value, 100u);
    for (unsigned i = 1; i < 8; ++i) {
      EXPECT_EQ(got.attributes[i].key, 0u);
      EXPECT_EQ(got.attributes[i].enum_value, 0u);
    }
  }
}
TEST(Bridge, PrivateBatchRejectsIgnoredCompressedSuffixAndMalformedWire) {
  auto event = Event();
  event.monotonic_ns = 0;
  event.attribute_count = 0;
  madobs_event_rule_v1 rule{};
  rule.sdk_id = event.sdk_id;
  rule.sdk_version = event.sdk_version;
  rule.event_id = event.event_id;
  rule.max_severity = 4;
  madobs_privacy_policy_v1 policy{sizeof(policy), 1, 1, &rule};
  std::vector<uint8_t> record, frames, compressed;
  imobs::EncodeGenericEvent(event, &record);
  imobs::WireWriter f(&frames);
  for (unsigned i = 0; i < 100; ++i) {
    f.Varint(record.size());
    f.Bytes(record.data(), record.size());
  }
  ASSERT_TRUE(imobs::DeflateBytes(frames.data(), frames.size(), &compressed));
  const auto make = [&](bool zipped, bool suffix) {
    std::vector<uint8_t> body, full;
    imobs::WireWriter b(&body);
    b.Bytes(zipped ? "IMBZ" : "IMB1", 4);
    b.U8(1);
    b.Fixed64(123);
    b.Fixed64(456);
    b.Varint(100);
    if (zipped) {
      b.Varint(frames.size());
      b.Bytes(compressed.data(), compressed.size());
    } else
      b.Bytes(frames.data(), frames.size());
    if (suffix) b.Bytes("PRIVATE_UNPARSED_SUFFIX", 23);
    imobs::WireWriter w(&full);
    w.Fixed64(imobs::Fnv1a64(body.data(), body.size()));
    w.Bytes(body.data(), body.size());
    return full;
  };
  for (bool zipped : {false, true}) {
    auto good = make(zipped, false);
    EXPECT_EQ(madobs_validate_private_batch(good.data(), good.size(), &policy),
              1);
    auto bad = make(zipped, true);
    EXPECT_EQ(madobs_validate_private_batch(bad.data(), bad.size(), &policy),
              0);
    for (size_t n = 0; n < good.size(); ++n)
      EXPECT_EQ(madobs_validate_private_batch(good.data(), n, &policy), 0);
    good[0] ^= 1;
    EXPECT_EQ(madobs_validate_private_batch(good.data(), good.size(), &policy),
              0);
  }
}
IMOBS_TEST_MAIN
