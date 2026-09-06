#include "../src/crossink_wifi_import.h"

#include <cassert>
#include <cstdint>
#include <cstring>

namespace {
constexpr char BASE64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

uint32_t fnv1a(const uint8_t* mac, const char* plaintext) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < 6; ++i) {
    hash ^= mac[i];
    hash *= 16777619UL;
  }
  for (const char* p = plaintext; *p; ++p) {
    hash ^= static_cast<uint8_t>(*p);
    hash *= 16777619UL;
  }
  return hash;
}

void base64Encode(const uint8_t* data, size_t len, char* out, size_t outSize) {
  assert(outSize >= ((len + 2) / 3) * 4 + 1);
  size_t input = 0;
  size_t output = 0;
  while (input < len) {
    const uint32_t a = data[input++];
    const bool hasB = input < len;
    const uint32_t b = hasB ? data[input++] : 0;
    const bool hasC = input < len;
    const uint32_t c = hasC ? data[input++] : 0;
    const uint32_t group = (a << 16) | (b << 8) | c;
    out[output++] = BASE64[(group >> 18) & 0x3f];
    out[output++] = BASE64[(group >> 12) & 0x3f];
    out[output++] = hasB ? BASE64[(group >> 6) & 0x3f] : '=';
    out[output++] = hasC ? BASE64[group & 0x3f] : '=';
  }
  out[output] = '\0';
}

void encodeFixture(const uint8_t* mac, const char* plaintext, char* out, size_t outSize,
                   bool invalidPrefix = false, bool corruptChecksum = false) {
  uint8_t payload[72]{};
  const size_t plaintextLen = std::strlen(plaintext);
  assert(plaintextLen <= 64);
  std::memcpy(payload, "CPV1", 4);
  if (invalidPrefix) payload[0] = 'X';
  const uint32_t checksum = fnv1a(mac, plaintext);
  payload[4] = static_cast<uint8_t>(checksum);
  payload[5] = static_cast<uint8_t>(checksum >> 8);
  payload[6] = static_cast<uint8_t>(checksum >> 16);
  payload[7] = static_cast<uint8_t>(checksum >> 24);
  if (corruptChecksum) payload[4] ^= 0x01;
  std::memcpy(payload + 8, plaintext, plaintextLen);
  for (size_t i = 0; i < plaintextLen + 8; ++i) payload[i] ^= mac[i % 6];
  base64Encode(payload, plaintextLen + 8, out, outSize);
}
}  // namespace

int main() {
  const uint8_t mac[6] = {0x24, 0x6f, 0x28, 0x01, 0x02, 0x03};
  char encoded[128]{};
  char password[64]{};

  encodeFixture(mac, "unitpass8", encoded, sizeof(encoded));
  assert(crossinkDecodePassword(encoded, mac, password, sizeof(password)));
  assert(std::strcmp(password, "unitpass8") == 0);

  // A valid base64 payload with a modified checksum must fail validation.
  encodeFixture(mac, "unitpass8", encoded, sizeof(encoded), false, true);
  assert(!crossinkDecodePassword(encoded, mac, password, sizeof(password)));

  // A correctly encoded payload with a non-CPV1 marker must be rejected.
  encodeFixture(mac, "unitpass8", encoded, sizeof(encoded), true);
  assert(!crossinkDecodePassword(encoded, mac, password, sizeof(password)));

  char tooLong[65];
  std::memset(tooLong, 'x', 64);
  tooLong[64] = '\0';
  encodeFixture(mac, tooLong, encoded, sizeof(encoded));
  assert(!crossinkDecodePassword(encoded, mac, password, sizeof(password)));
  return 0;
}
