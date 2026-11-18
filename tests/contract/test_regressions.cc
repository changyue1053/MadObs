#include <thread>
#include <type_traits>

#include "imobs/healer.h"
#include "imobs/imobs_c.h"
#include "imobs/obs_kernel.h"
#include "imobs_test.h"
static_assert(!std::is_copy_constructible_v<imobs::Healer>);
static_assert(!std::is_move_constructible_v<imobs::Healer>);
#include <unistd.h>

namespace {
struct Delayed : imobs::IActionExecutor {
  DoneFn completion;
  void Execute(uint64_t, const std::string&, DoneFn d) override {
    completion = std::move(d);
  }
  void Verify(uint64_t, DoneFn d) override { d(true); }
};
void Begin(imobs::Healer& h, Delayed& e) {
  imobs::SceneSpec s;
  s.scene_id = 1;
  s.detector.all_of = {1};
  s.detector.debounce_ms = 0;
  s.detector.debounce_jitter_ms = 0;
  s.detector.diagnose_ms = 0;
  imobs::ActionSpec a;
  a.name = "repair";
  s.ladder = {a};
  ASSERT_TRUE(h.RegisterScene(s, &e));
  h.SetSignal(1, true);
  h.Tick();
  h.Tick();
  h.Tick();
  ASSERT_TRUE(bool(e.completion));
}
void Send(void*, uint8_t, uint64_t, const uint8_t*, size_t,
          imobs_send_done_fn done, void* ctx) {
  done(ctx, 1);
}
}  // namespace
TEST(Regression, WorkerCompletionOnlyMutatesOnTick) {
  imobs::VirtualClock clock;
  Delayed e;
  imobs::Healer h(&clock, {});
  Begin(h, e);
  std::thread t([&] { e.completion(true); });
  t.join();
  EXPECT_TRUE(h.state(1) == imobs::HealthState::kHealing);
  h.Tick();
  EXPECT_EQ(h.stats().heal_success, 1u);
}
TEST(Regression, CompletionAfterHealerDestructionIsSafe) {
  imobs::VirtualClock clock;
  Delayed e;
  {
    imobs::Healer h(&clock, {});
    Begin(h, e);
  }
  e.completion(true);
}
TEST(Regression, ControlDisabledWithoutExplicitSecret) {
  EXPECT_TRUE(imobs::KernelOptions{}.control_key.empty());
}
TEST(Regression, CleanupWorksWhileDisabled) {
  imobs_debug_reset_firewall();
  char dir[] = "/tmp/madobs_cleanup_XXXXXX";
  ASSERT_TRUE(mkdtemp(dir) != nullptr);
  auto h = imobs_create(dir, Send, nullptr);
  ASSERT_TRUE(h != 0);
  ASSERT_EQ(imobs_start(h), IMOBS_OK);
  for (int i = 0; i < 8; ++i) imobs_debug_force_internal_error();
  EXPECT_EQ(imobs_stop(h), IMOBS_OK);
  EXPECT_EQ(imobs_destroy(h), IMOBS_OK);
  imobs_debug_reset_firewall();
}

TEST(Regression, CompletionMailboxIsBoundedAndCounted) {
  imobs::VirtualClock clock;
  Delayed e;
  imobs::Healer h(&clock, {});
  Begin(h, e);
  for (int i = 0; i < 1030; ++i) e.completion(true);
  EXPECT_EQ(h.stats().completion_dropped, 6u);
  h.Tick();
  EXPECT_EQ(h.stats().heal_success, 1u);
}
TEST(Regression, ForgedControlWithMissingOrDevelopmentKeyIsRejected) {
  struct NullChannel : imobs::IChannel {
    void Send(imobs::Priority, uint64_t, std::string_view,
              SendCallback done) override {
      done({true});
    }
  } channel;
  for (const std::string key : {std::string{}, std::string("imobs-dev-key")}) {
    imobs::VirtualClock clock;
    imobs::SchemaRegistry schema;
    imobs::KernelOptions opt;
    opt.control_key = key;
    char dir[] = "/tmp/madobs_control_XXXXXX";
    ASSERT_TRUE(mkdtemp(dir) != nullptr);
    std::string err;
    auto core =
        imobs::KernelCore::Open(dir, &clock, &channel, &schema, opt, &err);
    ASSERT_TRUE(core != nullptr);
    imobs::PolicyBundle b;
    b.version = 99;
    std::vector<uint8_t> payload, envelope;
    imobs::EncodePolicyBundle(b, &payload);
    imobs::SignEnvelope(imobs::kEnvelopePolicy, payload, key, &envelope);
    core->SubmitControlEnvelope(envelope.data(), envelope.size());
    core->Step();
    EXPECT_TRUE(core->policy().effective().version != 99);
  }
}

TEST(Regression, WorkerCompletionOverlapsTickAndDestruction) {
  for (int iteration = 0; iteration < 100; ++iteration) {
    imobs::VirtualClock clock;
    Delayed e;
    auto h =
        std::make_unique<imobs::Healer>(&clock, imobs::Healer::EventSink{});
    Begin(*h, e);
    std::atomic<bool> go{false};
    auto done = e.completion;
    std::thread t([&] {
      while (!go.load()) std::this_thread::yield();
      done(true);
    });
    go = true;
    if (iteration % 2) {
      for (int i = 0; i < 20; ++i) h->Tick();
    }
    h.reset();
    t.join();
  }
}
IMOBS_TEST_MAIN
