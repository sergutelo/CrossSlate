#include "../src/wifi_connect_policy.h"

#include <cassert>
#include <cstdint>

namespace {
struct Scenario {
  int calls[4]{};
  uint32_t budgets[4]{};
  int callCount = 0;
  int successIndex = -1;
};

bool tryNetwork(int index, uint32_t timeoutMs, void* raw) {
  auto* scenario = static_cast<Scenario*>(raw);
  scenario->calls[scenario->callCount] = index;
  scenario->budgets[scenario->callCount] = timeoutMs;
  ++scenario->callCount;
  return index == scenario->successIndex;
}
}  // namespace

int main() {
  // Regression: when home Wi-Fi (slot 0) is unavailable, a phone hotspot in
  // slot 1 must still receive its own complete connection window.
  Scenario hotspotSecond{};
  hotspotSecond.successIndex = 1;
  assert(wifiTrySavedNetworks(2, 6000, tryNetwork, &hotspotSecond));
  assert(hotspotSecond.callCount == 2);
  assert(hotspotSecond.calls[0] == 0);
  assert(hotspotSecond.calls[1] == 1);
  assert(hotspotSecond.budgets[0] == 6000);
  assert(hotspotSecond.budgets[1] == 6000);

  // Stop immediately after the first successful credential.
  Scenario homeFirst{};
  homeFirst.successIndex = 0;
  assert(wifiTrySavedNetworks(4, 6000, tryNetwork, &homeFirst));
  assert(homeFirst.callCount == 1);

  return 0;
}
