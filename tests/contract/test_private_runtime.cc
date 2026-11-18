#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "imobs/batch_view.h"
#include "imobs_test.h"
#include "madobs/private_runtime.hpp"
namespace {
struct Sink {
  std::atomic<unsigned> exported{0}, bad{0};
};
void Send(void* ctx, uint8_t, uint64_t, const uint8_t* data, size_t len,
          madobs_send_done_fn done, void* dc) {
  auto& sink = *static_cast<Sink*>(ctx);
  imobs::BatchView batch;
  if (imobs::ParseBatchView(data, len, &batch))
    for (auto rec : batch.records) {
      madobs_event_v1 event{};
      if (madobs_decode_event_record(
              reinterpret_cast<const uint8_t*>(rec.data()), rec.size(), &event,
              sizeof(event)) != MADOBS_OK)
        continue;
      if (event.event_id != 9 || event.monotonic_ns != 0 ||
          event.attribute_count != 1 || event.attributes[0].key != 1 ||
          event.attributes[0].enum_value != 2)
        ++sink.bad;
      ++sink.exported;
    }
  done(dc, 1);
}
madobs_event_v1 Event() {
  madobs_event_v1 e{};
  e.struct_size = sizeof(e);
  e.abi_version = 1;
  e.event_id = 9;
  e.severity = 4;
  e.monotonic_ns = 123456;
  e.attribute_count = 2;
  e.attributes[0] = {1, 2};
  e.attributes[1] = {999, 999};
  return e;
}
std::shared_ptr<madobs::PrivateRuntime> Open(Sink& sink) {
  char dir[] = "/tmp/madobs_owned_XXXXXX";
  if (!mkdtemp(dir)) return {};
  madobs_event_rule_v1 rule{};
  rule.event_id = 9;
  rule.max_severity = 4;
  rule.attribute_count = 1;
  rule.attributes[0].key = 1;
  rule.attributes[0].value_count = 1;
  rule.attributes[0].values[0] = 2;
  madobs_privacy_policy_v1 policy{sizeof(policy), 1, 1, &rule};
  // Stack policy expires at return. Binding must retain an independent copy.
  return madobs::PrivateRuntime::Open((std::string(dir) + "/private").c_str(),
                                      Send, &sink, &policy);
}
}  // namespace
TEST(PrivateRuntime, OwnsPolicyAndEnforcesPrivateEntry) {
  Sink sink;
  auto runtime = Open(sink);
  ASSERT_TRUE(runtime != nullptr);
  auto e = Event();
  EXPECT_EQ(runtime->Emit(e), MADOBS_OK);
  e.event_id = 10;
  EXPECT_EQ(runtime->Emit(e), MADOBS_E_SCHEMA_REJECTED);
  EXPECT_EQ(runtime->Close(), MADOBS_OK);
  EXPECT_EQ(sink.exported.load(), 1u);
  EXPECT_EQ(sink.bad.load(), 0u);
  e = Event();
  EXPECT_EQ(runtime->Emit(e), MADOBS_E_BAD_STATE);
  EXPECT_EQ(runtime->Close(), MADOBS_OK);
  EXPECT_TRUE(madobs::PrivateRuntime::Open("/tmp", Send, &sink, nullptr) ==
              nullptr);
}
TEST(PrivateRuntime, ConcurrentEmitAndCloseHaveNoOrphanAdmission) {
  for (unsigned repetition = 0; repetition < 20; ++repetition) {
    Sink sink;
    auto runtime = Open(sink);
    ASSERT_TRUE(runtime != nullptr);
    std::atomic<unsigned> accepted{0}, unexpected{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t)
      threads.emplace_back([runtime, &accepted, &unexpected, &go] {
        auto e = Event();
        while (!go.load()) std::this_thread::yield();
        for (unsigned i = 0; i < 300; ++i) {
          auto result = runtime->Emit(e);
          if (result == MADOBS_OK)
            ++accepted;
          else if (result != MADOBS_E_BAD_STATE &&
                   result != MADOBS_E_QUEUE_FULL)
            ++unexpected;
        }
      });
    go = true;
    while (accepted.load() < 1) std::this_thread::yield();
    EXPECT_EQ(runtime->Close(), MADOBS_OK);
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(unexpected.load(), 0u);
    EXPECT_EQ(sink.bad.load(), 0u);
    madobs_generic_stats_v1 stats{};
    EXPECT_EQ(runtime->Stats(stats), MADOBS_OK);
    EXPECT_EQ(stats.enqueued, accepted.load());
    EXPECT_EQ(stats.drained, accepted.load());
    EXPECT_EQ(stats.wal_dropped, 0u);
  }
}
TEST(PrivateRuntime, RejectsExistingRawWalAndPolicyReopen) {
  Sink sink;
  char parent[] = "/tmp/madobs_owned_existing_XXXXXX";
  ASSERT_TRUE(mkdtemp(parent) != nullptr);
  madobs_event_rule_v1 rule{};
  rule.event_id = 9;
  rule.max_severity = 4;
  madobs_privacy_policy_v1 policy{sizeof(policy), 1, 1, &rule};
  // Persist a raw record with a precise timestamp and unregistered attributes.
  auto fail = [](void*, uint8_t, uint64_t, const uint8_t*, size_t,
                 madobs_send_done_fn done, void* ctx) { done(ctx, 0); };
  auto raw = madobs_create(parent, fail, nullptr);
  ASSERT_TRUE(raw != 0);
  ASSERT_EQ(madobs_start(raw), MADOBS_OK);
  auto e = Event();
  ASSERT_EQ(madobs_emit_generic_event(raw, &e), MADOBS_OK);
  ASSERT_EQ(madobs_destroy(raw), MADOBS_OK);
  EXPECT_TRUE(madobs::PrivateRuntime::Open(parent, Send, &sink, &policy) ==
              nullptr);
  const auto fresh = std::string(parent) + "/fresh";
  auto runtime =
      madobs::PrivateRuntime::Open(fresh.c_str(), Send, &sink, &policy);
  ASSERT_TRUE(runtime != nullptr);
  ASSERT_EQ(runtime->Close(), MADOBS_OK);
  policy.event_count =
      0;  // tighter policy, even empty prior WAL must not be reused
  EXPECT_TRUE(madobs::PrivateRuntime::Open(fresh.c_str(), Send, &sink,
                                           &policy) == nullptr);
  EXPECT_EQ(sink.exported.load(), 0u);
}
TEST(PrivateRuntime, ConcurrentCloseWaitsForSameFinalStats) {
  Sink sink;
  auto runtime = Open(sink);
  ASSERT_TRUE(runtime != nullptr);
  auto event = Event();
  ASSERT_EQ(runtime->Emit(event), MADOBS_OK);
  std::atomic<unsigned> bad{0};
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < 8; ++i)
    threads.emplace_back([runtime, &bad] {
      if (runtime->Close() != MADOBS_OK) ++bad;
      madobs_generic_stats_v1 stats{};
      if (runtime->Stats(stats) != MADOBS_OK || stats.drained != 1) ++bad;
    });
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(bad.load(), 0u);
}
TEST(PrivateRuntime, RejectsSymlinksAndAllowsOnlyOneDirectoryOwner) {
  Sink sink;
  char parent[] = "/tmp/madobs_owned_paths_XXXXXX";
  ASSERT_TRUE(mkdtemp(parent) != nullptr);
  const auto link = std::string(parent) + "/link";
  ASSERT_EQ(symlink(parent, link.c_str()), 0);
  const auto dangling = std::string(parent) + "/dangling";
  ASSERT_EQ(symlink("absent-target", dangling.c_str()), 0);
  madobs_event_rule_v1 rule{};
  madobs_privacy_policy_v1 policy{sizeof(policy), 1, 1, &rule};
  EXPECT_TRUE(madobs::PrivateRuntime::Open(link.c_str(), Send, &sink,
                                           &policy) == nullptr);
  EXPECT_TRUE(madobs::PrivateRuntime::Open(dangling.c_str(), Send, &sink,
                                           &policy) == nullptr);
  const auto path = std::string(parent) + "/single-owner";
  std::atomic<unsigned> owners{0}, attempted{0};
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < 8; ++i)
    threads.emplace_back([&] {
      auto runtime =
          madobs::PrivateRuntime::Open(path.c_str(), Send, &sink, &policy);
      ++attempted;
      if (runtime) {
        ++owners;
        while (attempted.load() != 8) std::this_thread::yield();
        runtime->Close();
      }
    });
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(owners.load(), 1u);
}
TEST(PrivateRuntime, ExactPolicyRestartsAfterProcessExitWithoutClose) {
  Sink sink;
  char parent[] = "/tmp/madobs_crash_policy_XXXXXX";
  ASSERT_TRUE(mkdtemp(parent) != nullptr);
  const auto path = std::string(parent) + "/wal";
  madobs_event_rule_v1 rule{};
  rule.event_id = 9;
  rule.max_severity = 4;
  rule.attribute_count = 1;
  rule.attributes[0].key = 1;
  rule.attributes[0].value_count = 1;
  rule.attributes[0].values[0] = 2;
  madobs_privacy_policy_v1 policy{sizeof(policy), 1, 1, &rule};
  auto fail = [](void*, uint8_t, uint64_t, const uint8_t*, size_t,
                 madobs_send_done_fn done, void* ctx) { done(ctx, 0); };
  const pid_t child = fork();
  ASSERT_TRUE(child >= 0);
  if (child == 0) {
    auto runtime =
        madobs::PrivateRuntime::Open(path.c_str(), fail, nullptr, &policy);
    if (!runtime || runtime->Emit(Event()) != MADOBS_OK) _exit(1);
    for (unsigned i = 0; i < 1000; ++i) {
      madobs_generic_stats_v1 stats{};
      if (runtime->Stats(stats) == MADOBS_OK && stats.drained == 1)
        _exit(0);  // no destructor/Close
      usleep(1000);
    }
    _exit(2);
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  auto runtime =
      madobs::PrivateRuntime::Open(path.c_str(), Send, &sink, &policy);
  ASSERT_TRUE(runtime != nullptr);
  const pid_t contender = fork();
  ASSERT_TRUE(contender >= 0);
  if (contender == 0) {
    execl("/proc/self/exe", "test_private_runtime", "--contend", path.c_str(),
          static_cast<char*>(nullptr));
    _exit(4);
  }
  int contender_status = 0;
  ASSERT_EQ(waitpid(contender, &contender_status, 0), contender);
  EXPECT_TRUE(WIFEXITED(contender_status) &&
              WEXITSTATUS(contender_status) == 0);
  ASSERT_EQ(runtime->Close(), MADOBS_OK);
  EXPECT_EQ(sink.exported.load(), 1u);
  EXPECT_EQ(sink.bad.load(), 0u);
  // Both tightening and relaxation need explicit migration, never reinterpret
  // WAL.
  rule.max_severity = 3;
  EXPECT_TRUE(madobs::PrivateRuntime::Open(path.c_str(), Send, &sink,
                                           &policy) == nullptr);
  rule.max_severity = 5;
  EXPECT_TRUE(madobs::PrivateRuntime::Open(path.c_str(), Send, &sink,
                                           &policy) == nullptr);
  rule.max_severity = 4;
  auto same = madobs::PrivateRuntime::Open(path.c_str(), Send, &sink, &policy);
  ASSERT_TRUE(same != nullptr);
  ASSERT_EQ(same->Close(), MADOBS_OK);
  const auto manifest = path + "/.madobs-policy-v1";
  int fd = open(manifest.c_str(), O_WRONLY);
  ASSERT_TRUE(fd >= 0);
  ASSERT_EQ(write(fd, "BAD", 3), 3);
  close(fd);
  EXPECT_TRUE(madobs::PrivateRuntime::Open(path.c_str(), Send, &sink,
                                           &policy) == nullptr);
}
TEST(PrivateRuntime, ReplayGateRejectsRawContaminationOfBoundDirectory) {
  Sink sink;
  char parent[] = "/tmp/madobs_contaminated_XXXXXX";
  ASSERT_TRUE(mkdtemp(parent) != nullptr);
  const auto path = std::string(parent) + "/wal";
  madobs_event_rule_v1 rule{};
  rule.event_id = 9;
  rule.max_severity = 4;
  rule.attribute_count = 1;
  rule.attributes[0].key = 1;
  rule.attributes[0].value_count = 1;
  rule.attributes[0].values[0] = 2;
  madobs_privacy_policy_v1 policy{sizeof(policy), 1, 1, &rule};
  auto fail = [](void*, uint8_t, uint64_t, const uint8_t*, size_t,
                 madobs_send_done_fn done, void* ctx) { done(ctx, 0); };
  auto runtime =
      madobs::PrivateRuntime::Open(path.c_str(), fail, nullptr, &policy);
  ASSERT_TRUE(runtime != nullptr);
  ASSERT_EQ(runtime->Close(), MADOBS_OK);
  auto raw = madobs_create(path.c_str(), fail, nullptr);
  ASSERT_TRUE(raw != 0);
  ASSERT_EQ(madobs_start(raw), MADOBS_OK);
  auto e = Event();
  ASSERT_EQ(madobs_emit_generic_event(raw, &e), MADOBS_OK);
  ASSERT_EQ(madobs_destroy(raw), MADOBS_OK);
  auto restored =
      madobs::PrivateRuntime::Open(path.c_str(), Send, &sink, &policy);
  ASSERT_TRUE(restored != nullptr);
  ASSERT_EQ(restored->Close(), MADOBS_OK);
  EXPECT_EQ(sink.exported.load(), 0u);
  EXPECT_TRUE(restored->RejectedBatches() > 0);
  const auto gate = restored->GateStats();
  EXPECT_EQ(gate.rejected_attempts, restored->RejectedBatches());
  EXPECT_EQ(gate.rejected_by_priority[0], gate.rejected_attempts);
  EXPECT_EQ(gate.last_priority, 0u);
  EXPECT_EQ(gate.last_reason,
            static_cast<uint32_t>(MADOBS_PRIVATE_UNSAFE_RECORD));
  // Operational recovery resumes FUTURE events in a fresh directory, never
  // relabels, deletes or bypasses the blocked historical WAL.
  const auto fresh = path + "-fresh";
  auto replacement =
      madobs::PrivateRuntime::Open(fresh.c_str(), Send, &sink, &policy);
  ASSERT_TRUE(replacement != nullptr);
  EXPECT_EQ(replacement->Emit(Event()), MADOBS_OK);
  EXPECT_EQ(replacement->Close(), MADOBS_OK);
  EXPECT_EQ(sink.exported.load(), 1u);
  auto still_blocked =
      madobs::PrivateRuntime::Open(path.c_str(), Send, &sink, &policy);
  ASSERT_TRUE(still_blocked != nullptr);
  EXPECT_EQ(still_blocked->Close(), MADOBS_OK);
  EXPECT_TRUE(still_blocked->RejectedBatches() > 0);
  EXPECT_EQ(sink.exported.load(), 1u);  // blocked bytes were never forwarded
}
TEST(PrivateRuntime, CHandlesRemainSafeDuringEmitAndDestroy) {
  char parent[] = "/tmp/madobs_c_handles_XXXXXX";
  ASSERT_TRUE(mkdtemp(parent) != nullptr);
  const auto path = std::string(parent) + "/wal";
  Sink sink;
  madobs_event_rule_v1 rule{};
  rule.event_id = 9;
  rule.max_severity = 4;
  madobs_privacy_policy_v1 policy{sizeof(policy), 1, 1, &rule};
  const auto h = madobs_private_open(path.c_str(), Send, &sink, &policy);
  ASSERT_TRUE(h != 0);
  std::atomic<bool> go{false};
  std::atomic<unsigned> unexpected{0};
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < 4; ++i)
    threads.emplace_back([&] {
      auto event = Event();
      while (!go.load()) std::this_thread::yield();
      for (unsigned j = 0; j < 500; ++j) {
        const auto status = madobs_private_emit(h, &event);
        if (status != MADOBS_OK && status != MADOBS_E_INVALID_HANDLE &&
            status != MADOBS_E_BAD_STATE && status != MADOBS_E_QUEUE_FULL)
          ++unexpected;
      }
    });
  go = true;
  EXPECT_EQ(madobs_private_destroy(h), MADOBS_OK);
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(unexpected.load(), 0u);
  EXPECT_EQ(madobs_private_stop(h), MADOBS_E_INVALID_HANDLE);
}
int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--contend") {
    Sink sink;
    madobs_event_rule_v1 rule{};
    rule.event_id = 9;
    rule.max_severity = 4;
    rule.attribute_count = 1;
    rule.attributes[0].key = 1;
    rule.attributes[0].value_count = 1;
    rule.attributes[0].values[0] = 2;
    madobs_privacy_policy_v1 policy{sizeof(policy), 1, 1, &rule};
    return madobs::PrivateRuntime::Open(argv[2], Send, &sink, &policy) ? 3 : 0;
  }
  return imobs_test::RunAll();
}
