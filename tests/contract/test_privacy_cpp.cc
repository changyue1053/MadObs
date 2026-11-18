#include "madobs/privacy.h"
int main() {
  const auto noop = madobs_noop_observer();
  madobs_event_v1 input{}, output{};
  madobs_privacy_policy_v1 deny{};
  deny.struct_size = sizeof(deny);
  deny.abi_version = MADOBS_ABI_VERSION;
  input.struct_size = sizeof(input);
  input.abi_version = MADOBS_ABI_VERSION;
  if (madobs_sanitize_event(&deny, &input, &output) != MADOBS_INVALID) return 1;
  return madobs_observer_emit_private(&noop, nullptr, nullptr) == MADOBS_NOOP ? 0 : 2;
}
