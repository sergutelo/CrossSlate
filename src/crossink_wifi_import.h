#pragma once

#include <cstddef>
#include <cstdint>

// Strictly validates and decodes a CrossInk CPV1 password_obf value. The output
// is only populated on success and is never logged or persisted to SD.
bool crossinkDecodePassword(const char* encoded, const uint8_t mac[6],
                            char* passwordOut, size_t passwordOutSize);

#ifdef ARDUINO
// Merges up to four validated CrossInk credentials into wifi_creds (skipping
// SSIDs already saved). It reads /.crosspoint/wifi.json and writes only NVS
// on success; existing CrossSlate networks are never removed or overwritten.
bool crossinkImportWifiCredentials();
#endif
