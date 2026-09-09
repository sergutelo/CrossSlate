#include "wifi_connect_policy.h"

bool wifiTrySavedNetworks(int credentialCount, uint32_t perNetworkTimeoutMs,
                          WifiNetworkAttempt attempt, void* context) {
  if (credentialCount <= 0 || perNetworkTimeoutMs == 0 || attempt == nullptr) return false;
  for (int index = 0; index < credentialCount; ++index) {
    if (attempt(index, perNetworkTimeoutMs, context)) return true;
  }
  return false;
}
