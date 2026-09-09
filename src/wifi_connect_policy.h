#pragma once

#include <cstdint>

using WifiNetworkAttempt = bool (*)(int index, uint32_t timeoutMs, void* context);

// Tries credentials in storage order. Each saved network gets the complete
// timeout window; failure of an unavailable first SSID must not starve later
// credentials.
bool wifiTrySavedNetworks(int credentialCount, uint32_t perNetworkTimeoutMs,
                          WifiNetworkAttempt attempt, void* context);
