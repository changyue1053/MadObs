#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <madlink/obs_adapter.h>
#include <madobs/private_runtime.hpp>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x))                                                                  \
      return __LINE__;                                                         \
  } while (0)
struct Host {
  std::shared_ptr<madobs::PrivateRuntime> runtime;
  std::atomic<unsigned> accepted{0}, rejected{0}, exported{0};
};
static void Emit(void *ctx, const madobs_event_v1 *e) {
  auto &h = *static_cast<Host *>(ctx);
  if (h.runtime->Emit(*e) == MADOBS_OK)
    ++h.accepted;
  else
    ++h.rejected;
}
static void Send(void *ctx, uint8_t, uint64_t, const uint8_t *, size_t,
                 madobs_send_done_fn done, void *dc) {
  ++static_cast<Host *>(ctx)->exported;
  done(dc, 1);
}
int main() {
  char parent[] = "/tmp/madobs_composition_XXXXXX";
  CHECK(mkdtemp(parent));
  const auto wal = std::string(parent) + "/wal";
  madobs_event_rule_v1 rule{};
  rule.sdk_id = 2;
  rule.sdk_version = 1;
  rule.event_id = 1;
  rule.max_severity = 1; // Deliberately omit attributes: remove all dimensions.
  madobs_privacy_policy_v1 policy{sizeof(policy), MADOBS_ABI_VERSION, 1, &rule};
  Host host;
  host.runtime =
      madobs::PrivateRuntime::Open(wal.c_str(), Send, &host, &policy);
  CHECK(host.runtime);
  madobs_observer_v1 observer{sizeof(observer), MADOBS_ABI_VERSION, &host,
                              Emit};
  auto counters = std::make_shared<madlink_obs::Counters>();
  auto sink = madlink_obs::MakeMetricsSink(observer, counters, 2);
  sink("conn.established", 999, {{"transport", "tcp"}, {"secret", "fixture"}});
  sink("conn.lost", 1, {}); // Adapter dispatches; private policy rejects.
  sink("unknown", 1, {});   // Adapter filters before dispatch.
  CHECK(host.accepted == 1 && host.rejected == 1);
  CHECK(counters->delivered == 2 && counters->filtered == 1);
  imlink::Config config;
  config.race.quic_enabled = false;
  config.metrics_sink = sink;
  auto client = imlink::LinkClient::Create(config);
  CHECK(client);
  client.reset(); // No Start: no network activity.
  imlink::LinkClient::WaitForPendingReleases();
  CHECK(host.runtime->Close() == MADOBS_OK);
  CHECK(host.exported > 0);
  host.runtime.reset();
  std::filesystem::remove_all(parent);
  return 0;
}
