#include "imobs/generic_events.h"
#include "madobs/privacy.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
namespace {
uint32_t state = 0x49a7f421u;
unsigned iteration = 0;
uint32_t Next() {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr,                                                     \
                   "seed=0x49a7f421 iteration=%u state=%08x line=%d\n",        \
                   iteration, state, __LINE__);                                \
      std::abort();                                                            \
    }                                                                          \
  } while (0)
bool Same(const madobs_event_v1 &a, const madobs_event_v1 &b) {
  if (a.struct_size != b.struct_size || a.abi_version != b.abi_version ||
      a.sdk_id != b.sdk_id || a.sdk_version != b.sdk_version ||
      a.event_id != b.event_id || a.severity != b.severity ||
      a.monotonic_ns != b.monotonic_ns ||
      a.attribute_count != b.attribute_count || a.reserved != b.reserved)
    return false;
  for (uint32_t i = 0; i < a.attribute_count; ++i)
    if (a.attributes[i].key != b.attributes[i].key ||
        a.attributes[i].enum_value != b.attributes[i].enum_value)
      return false;
  return true;
}
void CleanTail(const madobs_event_v1 &event) {
  CHECK(event.reserved == 0);
  for (uint32_t i = event.attribute_count; i < 8; ++i)
    CHECK(event.attributes[i].key == 0 && event.attributes[i].enum_value == 0);
}
void DecodeCheck(const uint8_t *data, size_t size) {
  madobs_event_v1 output;
  std::memset(&output, 0xa5, sizeof(output));
  std::array<unsigned char, sizeof(output)> saved{};
  std::memcpy(saved.data(), &output, sizeof(output));
  if (!imobs::DecodeGenericEvent(data, size, &output)) {
    CHECK(std::memcmp(saved.data(), &output, sizeof(output)) == 0);
    return;
  }
  CHECK(imobs::ValidGenericEvent(&output));
  CleanTail(output);
  std::vector<uint8_t> canonical;
  imobs::EncodeGenericEvent(output, &canonical);
  CHECK(canonical.size() <= 128);
  madobs_event_v1 second{};
  CHECK(imobs::DecodeGenericEvent(canonical.data(), canonical.size(), &second));
  CHECK(Same(output, second));
}
} // namespace
int main() {
  // Explicit integer extremes and maximum arity, beyond pseudorandom coverage.
  madobs_event_v1 boundary{};
  boundary.struct_size = sizeof(boundary);
  boundary.abi_version = MADOBS_ABI_VERSION;
  boundary.sdk_id = boundary.sdk_version = boundary.event_id = UINT32_MAX;
  boundary.monotonic_ns = UINT64_MAX;
  boundary.severity = 5;
  boundary.attribute_count = 8;
  for (uint32_t i = 0; i < 8; ++i)
    boundary.attributes[i] = {UINT32_MAX - i, UINT32_MAX};
  std::vector<uint8_t> edge;
  imobs::EncodeGenericEvent(boundary, &edge);
  CHECK(edge.size() == 107);
  DecodeCheck(edge.data(), edge.size());
  // Raw decoding is structural, not private-gate canonicality approval.
  boundary.sdk_id = 0;
  edge.clear();
  imobs::EncodeGenericEvent(boundary, &edge);
  edge[2] = 0x80;
  edge.insert(edge.begin() + 3, 0);
  madobs_event_v1 nonminimal{};
  CHECK(imobs::DecodeGenericEvent(edge.data(), edge.size(), &nonminimal));
  CHECK(Same(boundary, nonminimal));
  std::vector<uint8_t> normalized;
  imobs::EncodeGenericEvent(nonminimal, &normalized);
  CHECK(normalized != edge);
  for (iteration = 0; iteration < 10000; ++iteration) {
    madobs_event_rule_v1 rule{};
    rule.sdk_id = Next();
    rule.sdk_version = Next();
    rule.event_id = Next();
    rule.max_severity = Next() % 6;
    rule.attribute_count = Next() % 9;
    const auto base = Next();
    for (uint32_t i = 0; i < rule.attribute_count; ++i) {
      auto &attribute = rule.attributes[i];
      attribute.key = base + i;
      attribute.value_count = 1 + Next() % 16;
      const auto value = Next();
      for (uint32_t j = 0; j < attribute.value_count; ++j)
        attribute.values[j] = value + j;
    }
    madobs_privacy_policy_v1 policy{sizeof(policy), MADOBS_ABI_VERSION, 1,
                                    &rule};
    CHECK(madobs_privacy_policy_valid(&policy));
    madobs_event_v1 input{};
    input.struct_size = sizeof(input);
    input.abi_version = MADOBS_ABI_VERSION;
    input.sdk_id = rule.sdk_id;
    input.sdk_version = rule.sdk_version;
    input.event_id = rule.event_id;
    input.severity = Next() % (rule.max_severity + 1);
    input.monotonic_ns = (uint64_t(Next()) << 32) | Next();
    input.attribute_count = Next() % 9;
    madobs_event_v1 expected = input;
    expected.attribute_count = 0;
    expected.monotonic_ns = 0;
    for (uint32_t i = 0; i < 8; ++i) {
      input.attributes[i] = {base + 100 + i,
                             Next()}; // inactive/unknown dimensions are poison
      if (i < input.attribute_count && i < rule.attribute_count) {
        input.attributes[i].key = rule.attributes[i].key;
        const auto slot = Next() % (rule.attributes[i].value_count + 1);
        if (slot < rule.attributes[i].value_count) {
          input.attributes[i].enum_value = rule.attributes[i].values[slot];
          expected.attributes[expected.attribute_count++] = input.attributes[i];
        } else
          input.attributes[i].enum_value =
              rule.attributes[i].values[0] + rule.attributes[i].value_count;
      }
    }
    madobs_event_v1 sanitized{};
    CHECK(madobs_sanitize_event(&policy, &input, &sanitized) == MADOBS_EMITTED);
    CHECK(Same(sanitized, expected));
    CleanTail(sanitized);
    auto alias = input;
    CHECK(madobs_sanitize_event(&policy, &alias, &alias) == MADOBS_EMITTED);
    CHECK(Same(alias, sanitized));
    CleanTail(alias);
    CHECK(madobs_sanitize_event(&policy, &alias, &alias) == MADOBS_EMITTED);
    CHECK(Same(alias, sanitized));
    CleanTail(alias);
    auto rejected = input;
    rejected.severity = rule.max_severity + 1;
    const auto before = sanitized;
    CHECK(madobs_sanitize_event(&policy, &rejected, &sanitized) ==
          MADOBS_INVALID);
    CHECK(Same(before, sanitized));
    CleanTail(sanitized);
    rejected = input;
    rejected.attribute_count = 2;
    rejected.attributes[1].key = rejected.attributes[0].key;
    CHECK(madobs_sanitize_event(&policy, &rejected, &sanitized) ==
          MADOBS_INVALID);
    CHECK(Same(before, sanitized));
    std::vector<uint8_t> wire;
    imobs::EncodeGenericEvent(input, &wire);
    CHECK(wire.size() <= 128);
    madobs_event_v1 decoded{};
    CHECK(imobs::DecodeGenericEvent(wire.data(), wire.size(), &decoded));
    CHECK(Same(input, decoded));
    CleanTail(decoded);
    for (size_t size = 0; size < wire.size(); ++size) {
      CHECK(!imobs::DecodeGenericEvent(wire.data(), size, &decoded));
      DecodeCheck(wire.data(), size);
    }
    for (unsigned mutation = 0; mutation < 4; ++mutation) {
      auto changed = wire;
      changed[Next() % changed.size()] ^=
          static_cast<uint8_t>(1 + Next() % 255);
      DecodeCheck(changed.data(), changed.size());
    }
    wire.push_back(0);
    CHECK(!imobs::DecodeGenericEvent(wire.data(), wire.size(), &decoded));
    std::vector<uint8_t> random(Next() % 161);
    for (auto &byte : random)
      byte = static_cast<uint8_t>(Next());
    if (random.size() >= 2 && iteration % 2 == 0) {
      random[0] = 'G';
      random[1] = 1;
    }
    DecodeCheck(random.data(), random.size());
  }
  std::puts("10000 deterministic privacy/codec boundary cases passed (seed "
            "0x49a7f421)");
}
