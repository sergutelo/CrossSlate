#include "crossink_wifi_import.h"

#include <cstring>

#ifndef CROSSINK_WIFI_IMPORT_HOST_TEST
#include <mbedtls/base64.h>
#endif

namespace {
constexpr size_t MAC_LEN = 6;
constexpr size_t MAX_PASSWORD_LEN = 63;
constexpr size_t MIN_PASSWORD_LEN = 8;
constexpr size_t PAYLOAD_OVERHEAD = 8;  // CPV1 + checksum
constexpr size_t MAX_PAYLOAD_LEN = PAYLOAD_OVERHEAD + MAX_PASSWORD_LEN;
constexpr uint32_t FNV_OFFSET_BASIS = 2166136261UL;
constexpr uint32_t FNV_PRIME = 16777619UL;

uint32_t fnv1aForPassword(const uint8_t mac[MAC_LEN], const char* password, size_t length) {
  uint32_t hash = FNV_OFFSET_BASIS;
  for (size_t i = 0; i < MAC_LEN; ++i) {
    hash ^= mac[i];
    hash *= FNV_PRIME;
  }
  for (size_t i = 0; i < length; ++i) {
    hash ^= static_cast<uint8_t>(password[i]);
    hash *= FNV_PRIME;
  }
  return hash;
}

bool decodeBase64(const char* encoded, uint8_t* output, size_t outputSize, size_t* outputLength) {
#ifdef CROSSINK_WIFI_IMPORT_HOST_TEST
  if (!encoded || !output || !outputLength) return false;
  size_t encodedLength = std::strlen(encoded);
  if (encodedLength == 0 || (encodedLength % 4) != 0) return false;
  size_t written = 0;
  for (size_t i = 0; i < encodedLength; i += 4) {
    uint32_t group = 0;
    int padding = 0;
    for (size_t j = 0; j < 4; ++j) {
      const char c = encoded[i + j];
      int value = -1;
      if (c >= 'A' && c <= 'Z') value = c - 'A';
      else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
      else if (c >= '0' && c <= '9') value = c - '0' + 52;
      else if (c == '+') value = 62;
      else if (c == '/') value = 63;
      else if (c == '=') { value = 0; ++padding; }
      else return false;
      if (padding && j < 3) return false;
      group = (group << 6) | static_cast<uint32_t>(value);
    }
    if (padding > 2 || (padding && i + 4 != encodedLength)) return false;
    const size_t bytes = 3U - static_cast<size_t>(padding);
    if (written + bytes > outputSize) return false;
    for (size_t j = 0; j < bytes; ++j) output[written++] = static_cast<uint8_t>(group >> (16 - j * 8));
  }
  *outputLength = written;
  return true;
#else
  return mbedtls_base64_decode(output, outputSize, outputLength,
                               reinterpret_cast<const unsigned char*>(encoded), std::strlen(encoded)) == 0;
#endif
}
}  // namespace

bool crossinkDecodePassword(const char* encoded, const uint8_t mac[MAC_LEN],
                            char* passwordOut, size_t passwordOutSize) {
  if (!encoded || !mac || !passwordOut || passwordOutSize < MAX_PASSWORD_LEN + 1) return false;
  passwordOut[0] = '\0';
  const size_t encodedLength = std::strlen(encoded);
  if (encodedLength == 0 || encodedLength > 128) return false;

  uint8_t payload[MAX_PAYLOAD_LEN];
  size_t payloadLength = 0;
  if (!decodeBase64(encoded, payload, sizeof(payload), &payloadLength) ||
      payloadLength < PAYLOAD_OVERHEAD || payloadLength > sizeof(payload)) return false;
  for (size_t i = 0; i < payloadLength; ++i) payload[i] ^= mac[i % MAC_LEN];
  if (std::memcmp(payload, "CPV1", 4) != 0) return false;

  const size_t passwordLength = payloadLength - PAYLOAD_OVERHEAD;
  if (passwordLength < MIN_PASSWORD_LEN || passwordLength > MAX_PASSWORD_LEN) return false;
  for (size_t i = 0; i < passwordLength; ++i) {
    if (payload[PAYLOAD_OVERHEAD + i] < 0x20 || payload[PAYLOAD_OVERHEAD + i] > 0x7e) return false;
    passwordOut[i] = static_cast<char>(payload[PAYLOAD_OVERHEAD + i]);
  }
  passwordOut[passwordLength] = '\0';
  const uint32_t expected = static_cast<uint32_t>(payload[4]) |
                            (static_cast<uint32_t>(payload[5]) << 8) |
                            (static_cast<uint32_t>(payload[6]) << 16) |
                            (static_cast<uint32_t>(payload[7]) << 24);
  if (fnv1aForPassword(mac, passwordOut, passwordLength) != expected) {
    passwordOut[0] = '\0';
    return false;
  }
  return true;
}

#ifdef ARDUINO
#include <Preferences.h>
#include <SDCardManager.h>
#include <esp_mac.h>
#include <mbedtls/base64.h>

namespace {
constexpr char CROSSINK_WIFI_PATH[] = "/.crosspoint/wifi.json";
constexpr size_t MAX_WIFI_FILE_SIZE = 4096;
constexpr size_t CROSSINK_MAX_SSID_LEN = 32;
constexpr size_t MAX_SAVED_NETWORKS = 4;

struct ImportCandidate {
  char ssid[CROSSINK_MAX_SSID_LEN + 1];
  char password[MAX_PASSWORD_LEN + 1];
};

static char wifiFileBuffer[MAX_WIFI_FILE_SIZE + 1];
static ImportCandidate importCandidates[MAX_SAVED_NETWORKS];

void skipWhitespace(const char*& p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
}

bool skipJsonString(const char*& p) {
  if (*p++ != '\"') return false;
  while (*p && *p != '\"') {
    unsigned char c = static_cast<unsigned char>(*p++);
    if (c < 0x20) return false;
    if (c == '\\') {
      c = static_cast<unsigned char>(*p++);
      if (c == '\0' || (c != '\"' && c != '\\' && c != '/' && c != 'b' && c != 'f' &&
                         c != 'n' && c != 'r' && c != 't')) return false;
    }
  }
  if (*p++ != '\"') return false;
  return true;
}

bool readJsonString(const char*& p, char* out, size_t outSize) {
  if (!out || outSize == 0 || *p++ != '\"') return false;
  size_t length = 0;
  while (*p && *p != '\"') {
    unsigned char c = static_cast<unsigned char>(*p++);
    if (c < 0x20) return false;
    if (c == '\\') {
      c = static_cast<unsigned char>(*p++);
      if (c == '\0') return false;
      switch (c) {
        case '\"': case '\\': case '/': break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        default: return false;  // Reject unicode and unknown escapes rather than truncate them.
      }
    }
    if (length + 1 >= outSize) return false;
    out[length++] = static_cast<char>(c);
  }
  if (*p++ != '\"') return false;
  out[length] = '\0';
  return true;
}

bool skipJsonValue(const char*& p, int depth = 0) {
  if (depth > 8) return false;
  skipWhitespace(p);
  if (*p == '\"') return skipJsonString(p);
  if (*p == '{' || *p == '[') {
    const char open = *p++;
    const char close = open == '{' ? '}' : ']';
    skipWhitespace(p);
    if (*p == close) { ++p; return true; }
    while (*p) {
      if (open == '{') {
        char key[2];
        if (!readJsonString(p, key, sizeof(key))) return false;
        skipWhitespace(p);
        if (*p++ != ':') return false;
      }
      if (!skipJsonValue(p, depth + 1)) return false;
      skipWhitespace(p);
      if (*p == close) { ++p; return true; }
      if (*p++ != ',') return false;
      skipWhitespace(p);
    }
    return false;
  }
  if (std::strncmp(p, "true", 4) == 0) { p += 4; return true; }
  if (std::strncmp(p, "false", 5) == 0) { p += 5; return true; }
  if (std::strncmp(p, "null", 4) == 0) { p += 4; return true; }
  const char* start = p;
  if (*p == '-') ++p;
  while (*p >= '0' && *p <= '9') ++p;
  if (*p == '.') { ++p; while (*p >= '0' && *p <= '9') ++p; }
  return p != start;
}

bool validSsid(const char* ssid) {
  const size_t length = std::strlen(ssid);
  if (length == 0 || length > CROSSINK_MAX_SSID_LEN) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(ssid[i]);
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

bool readCandidateObject(const char*& p, const uint8_t mac[MAC_LEN], ImportCandidate& out) {
  if (*p++ != '{') return false;
  bool hasSsid = false;
  bool hasPassword = false;
  char encoded[129]{};
  out.ssid[0] = '\0';
  out.password[0] = '\0';
  skipWhitespace(p);
  while (*p != '}') {
    char key[20];
    if (!readJsonString(p, key, sizeof(key))) return false;
    skipWhitespace(p);
    if (*p++ != ':') return false;
    skipWhitespace(p);
    if (std::strcmp(key, "ssid") == 0) {
      if (!readJsonString(p, out.ssid, sizeof(out.ssid))) return false;
      hasSsid = true;
    } else if (std::strcmp(key, "password_obf") == 0) {
      if (!readJsonString(p, encoded, sizeof(encoded))) return false;
      hasPassword = true;
    } else if (!skipJsonValue(p)) {
      return false;
    }
    skipWhitespace(p);
    if (*p == '}') break;
    if (*p++ != ',') return false;
    skipWhitespace(p);
  }
  ++p;
  return hasSsid && hasPassword && validSsid(out.ssid) &&
         crossinkDecodePassword(encoded, mac, out.password, sizeof(out.password));
}

bool hasExistingNetwork(Preferences& prefs) {
  if (prefs.getInt("wifi_count", 0) > 0) return true;
  char ssid[CROSSINK_MAX_SSID_LEN + 1];
  for (size_t i = 0; i < MAX_SAVED_NETWORKS; ++i) {
    char key[16];
    snprintf(key, sizeof(key), "wifi_ssid_%u", static_cast<unsigned>(i));
    if (prefs.getString(key, ssid, sizeof(ssid)) > 0) return true;
  }
  return false;
}
}  // namespace

bool crossinkImportWifiCredentials() {
  auto file = SdMan.open(CROSSINK_WIFI_PATH, O_RDONLY);
  if (!file.isOpen()) return false;
  const int bytesRead = file.read(wifiFileBuffer, MAX_WIFI_FILE_SIZE);
  const bool tooLarge = bytesRead == static_cast<int>(MAX_WIFI_FILE_SIZE) && file.available();
  file.close();
  if (bytesRead <= 0 || tooLarge) return false;
  wifiFileBuffer[bytesRead] = '\0';

  uint8_t mac[MAC_LEN]{};
  if (esp_efuse_mac_get_default(mac) != ESP_OK) return false;

  const char* p = wifiFileBuffer;
  char lastConnectedSsid[CROSSINK_MAX_SSID_LEN + 1]{};
  size_t candidateCount = 0;
  bool foundCredentials = false;
  skipWhitespace(p);
  if (*p++ != '{') return false;
  skipWhitespace(p);
  while (*p != '}') {
    char key[24];
    if (!readJsonString(p, key, sizeof(key))) return false;
    skipWhitespace(p);
    if (*p++ != ':') return false;
    skipWhitespace(p);
    if (std::strcmp(key, "lastConnectedSsid") == 0) {
      if (!readJsonString(p, lastConnectedSsid, sizeof(lastConnectedSsid))) return false;
    } else if (std::strcmp(key, "credentials") == 0) {
      if (*p++ != '[') return false;
      foundCredentials = true;
      skipWhitespace(p);
      while (*p != ']') {
        ImportCandidate candidate{};
        if (!readCandidateObject(p, mac, candidate)) return false;
        bool duplicate = false;
        for (size_t i = 0; i < candidateCount; ++i) {
          if (std::strcmp(importCandidates[i].ssid, candidate.ssid) == 0) { duplicate = true; break; }
        }
        if (!duplicate && candidateCount < MAX_SAVED_NETWORKS) importCandidates[candidateCount++] = candidate;
        skipWhitespace(p);
        if (*p == ']') break;
        if (*p++ != ',') return false;
        skipWhitespace(p);
      }
      ++p;
    } else if (!skipJsonValue(p)) {
      return false;
    }
    skipWhitespace(p);
    if (*p == '}') break;
    if (*p++ != ',') return false;
    skipWhitespace(p);
  }
  ++p;
  skipWhitespace(p);
  if (*p != '\0' || !foundCredentials || candidateCount == 0) return false;

  if (validSsid(lastConnectedSsid)) {
    for (size_t i = 1; i < candidateCount; ++i) {
      if (std::strcmp(importCandidates[i].ssid, lastConnectedSsid) == 0) {
        const ImportCandidate preferred = importCandidates[i];
        for (size_t j = i; j > 0; --j) importCandidates[j] = importCandidates[j - 1];
        importCandidates[0] = preferred;
        break;
      }
    }
  }

  Preferences prefs;
  if (!prefs.begin("wifi_creds", false)) return false;

  // Merge semantics: keep whatever the user saved manually in CrossSlate and
  // add CrossInk networks that are not present yet (matched by SSID). A plain
  // "only import when empty" rule meant later CrossInk edits (a second
  // network, a changed password) never reached CrossSlate.
  const int existingCount = prefs.getInt("wifi_count", 0);
  size_t merged = 0;
  bool written = true;
  for (size_t i = 0; i < candidateCount && written; ++i) {
    bool exists = false;
    for (int j = 0; j < existingCount; ++j) {
      char ssidKey[16];
      char ssid[CROSSINK_MAX_SSID_LEN + 1] = {0};
      snprintf(ssidKey, sizeof(ssidKey), "wifi_ssid_%d", j);
      prefs.getString(ssidKey, ssid, sizeof(ssid));
      if (std::strcmp(ssid, importCandidates[i].ssid) == 0) { exists = true; break; }
    }
    if (exists) continue;
    if (existingCount + static_cast<int>(merged) >= static_cast<int>(MAX_SAVED_NETWORKS)) break;
    const size_t slot = static_cast<size_t>(existingCount + merged);
    char ssidKey[16], passKey[16];
    snprintf(ssidKey, sizeof(ssidKey), "wifi_ssid_%u", static_cast<unsigned>(slot));
    snprintf(passKey, sizeof(passKey), "wifi_pass_%u", static_cast<unsigned>(slot));
    written = prefs.putString(ssidKey, importCandidates[i].ssid) > 0 &&
              prefs.putString(passKey, importCandidates[i].password) > 0;
    if (written) ++merged;
  }

  bool countWritten = true;
  if (merged > 0) {
    countWritten = prefs.putInt("wifi_count", existingCount + static_cast<int>(merged)) > 0;
  }
  // Roll back the newly added slots if any write failed.
  if (!written || !countWritten) {
    for (int j = existingCount; j < MAX_SAVED_NETWORKS; ++j) {
      char ssidKey[16], passKey[16];
      snprintf(ssidKey, sizeof(ssidKey), "wifi_ssid_%d", j);
      snprintf(passKey, sizeof(passKey), "wifi_pass_%d", j);
      prefs.remove(ssidKey);
      prefs.remove(passKey);
    }
    prefs.putInt("wifi_count", existingCount);
    prefs.end();
    return false;
  }
  prefs.end();
  return merged > 0;
}
#endif
