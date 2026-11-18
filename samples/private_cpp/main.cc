#include <cstdlib>
#include <string>

#include "madobs/private_runtime.hpp"
static void Send(void*, uint8_t, uint64_t, const uint8_t*, size_t,
                 madobs_send_done_fn done, void* ctx) {
  done(ctx, 1);
}
int main() {
  char parent[] = "/tmp/madobs_private_consumer_XXXXXX";
  if (!mkdtemp(parent)) return 1;
  madobs_event_rule_v1 rule{};
  rule.event_id = 1;
  madobs_privacy_policy_v1 policy{sizeof(policy), 1, 1, &rule};
  const std::string path = std::string(parent) + "/wal";
  auto runtime =
      madobs::PrivateRuntime::Open(path.c_str(), Send, nullptr, &policy);
  if (!runtime) return 2;
  madobs_event_v1 event{};
  event.struct_size = sizeof(event);
  event.abi_version = 1;
  event.event_id = 1;
  if (runtime->Emit(event) != MADOBS_OK || runtime->Close() != MADOBS_OK)
    return 3;
  madobs_generic_stats_v1 stats{};
  if (runtime->Stats(stats) != MADOBS_OK || stats.drained != 1) return 4;
  auto recovered =
      madobs::PrivateRuntime::Open(path.c_str(), Send, nullptr, &policy);
  if (!recovered || recovered->Close() != MADOBS_OK) return 5;
  return 0;
}
