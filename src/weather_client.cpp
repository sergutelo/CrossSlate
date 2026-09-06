#include "weather_client.h"
#include "city_list.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* findJsonValue(const char* json, const char* key) {
  if (!json || !key) return nullptr;
  char needle[32];
  const int n = std::snprintf(needle, sizeof(needle), "\"%s\"", key);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(needle)) return nullptr;
  const char* p = std::strstr(json, needle);
  if (!p) return nullptr;
  p += n;
  while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
  if (*p++ != ':') return nullptr;
  while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
  return p;
}

static bool readNumber(const char* json, const char* key, double& out) {
  const char* p = findJsonValue(json, key);
  if (!p) return false;
  char* end = nullptr;
  out = std::strtod(p, &end);
  return end != p;
}

static bool readInt(const char* json, const char* key, int& out) {
  double value = 0;
  if (!readNumber(json, key, value) || value < -1000 || value > 1000) return false;
  out = static_cast<int>(value);
  return true;
}

static bool readTimestamp(const char* json, char* out, size_t outSize) {
  if (!out || outSize < 17) return false;
  const char* p = findJsonValue(json, "time");
  if (!p || *p++ != '\"' || std::strlen(p) < 16 || p[4] != '-' || p[7] != '-' ||
      p[10] != 'T' || p[13] != ':') return false;
  std::memcpy(out, p, 16);
  out[10] = ' ';
  out[16] = '\0';
  return true;
}

WeatherIcon weatherIconForWmo(int wmoCode, bool isDay) {
  if (wmoCode == 0) return isDay ? WeatherIcon::SUN : WeatherIcon::NIGHT;
  if (wmoCode == 1 || wmoCode == 2) return WeatherIcon::PARTLY_CLOUDY;
  if (wmoCode == 3) return WeatherIcon::CLOUDS;
  if (wmoCode == 45 || wmoCode == 48) return WeatherIcon::FOG;
  if ((wmoCode >= 51 && wmoCode <= 67) || (wmoCode >= 80 && wmoCode <= 82)) return WeatherIcon::RAIN;
  if ((wmoCode >= 71 && wmoCode <= 77) || (wmoCode >= 85 && wmoCode <= 86)) return WeatherIcon::SNOW;
  if (wmoCode >= 95 && wmoCode <= 99) return WeatherIcon::STORM;
  return WeatherIcon::CLOUDS;
}

const char* weatherSummaryForWmo(int wmoCode) {
  if (wmoCode == 0) return "Despejado";
  if (wmoCode == 1) return "Mayormente despejado";
  if (wmoCode == 2) return "Parcialmente nuboso";
  if (wmoCode == 3) return "Nublado";
  if (wmoCode == 45 || wmoCode == 48) return "Niebla";
  if (wmoCode >= 51 && wmoCode <= 57) return "Llovizna";
  if (wmoCode >= 61 && wmoCode <= 67) return "Lluvia";
  if (wmoCode >= 71 && wmoCode <= 77) return "Nieve";
  if (wmoCode >= 80 && wmoCode <= 82) return "Chubascos";
  if (wmoCode >= 85 && wmoCode <= 86) return "Chubascos de nieve";
  if (wmoCode >= 95 && wmoCode <= 99) return "Tormenta";
  return "Tiempo variable";
}

bool parseOpenMeteoJson(const char* json, WeatherCache& out) {
  std::memset(&out, 0, sizeof(out));
  if (!json) return false;
  // Open-Meteo includes a current_units object before current. Scope every
  // lookup to current so temperature_2m resolves to its numeric value, not
  // the earlier "°C" unit string.
  const char* current = std::strstr(json, "\"current\"");
  if (!current) return false;
  current += sizeof("\"current\"") - 1;
  while (*current && std::isspace(static_cast<unsigned char>(*current))) ++current;
  if (*current++ != ':') return false;
  while (*current && std::isspace(static_cast<unsigned char>(*current))) ++current;
  if (*current++ != '{') return false;

  double temperature = 0;
  int wmo = 0, isDay = 0;
  if (!readNumber(current, "temperature_2m", temperature) || temperature < -100.0 || temperature > 100.0 ||
      !readInt(current, "weather_code", wmo) || !readInt(current, "is_day", isDay) ||
      !readTimestamp(current, out.updated, sizeof(out.updated))) return false;
  out.temperatureTenths = static_cast<int>(temperature * 10.0 + (temperature >= 0 ? 0.5 : -0.5));
  out.icon = weatherIconForWmo(wmo, isDay != 0);
  std::snprintf(out.summary, sizeof(out.summary), "%s", weatherSummaryForWmo(wmo));

  // Extended current fields (optional; old shapes keep zeros).
  double number = 0;
  if (readNumber(current, "apparent_temperature", number) && number > -100.0 && number < 100.0)
    out.feelsLikeTenths = static_cast<int>(number * 10.0 + (number >= 0 ? 0.5 : -0.5));
  if (readNumber(current, "relative_humidity_2m", number) && number >= 0 && number <= 100)
    out.humidityPct = static_cast<uint8_t>(number + 0.5);
  if (readNumber(current, "wind_speed_10m", number) && number >= 0 && number < 500)
    out.windKmhTenths = static_cast<int>(number * 10.0 + 0.5);
  if (readNumber(current, "wind_direction_10m", number) && number >= 0 && number < 360)
    out.windDirDeg = static_cast<int16_t>(number);
  if (readNumber(current, "uv_index", number) && number >= 0 && number < 30) {
    out.uvCurrentTenths = static_cast<int>(number * 10.0 + 0.5);
    out.uvCurrentKnown = true;
  }

  // Daily block: first array element of each requested variable.
  const char* daily = std::strstr(json, "\"daily\"");
  if (daily) {
    // Daily values are arrays: "temperature_2m_max":[37.2]. Skip the '[' and
    // read the first number inside.
    const auto arrayFirst = [&daily](const char* key, double& value) -> bool {
      const char* p = findJsonValue(daily, key);
      if (!p) return false;
      while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
      if (*p++ != '[') return false;
      while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
      char* end = nullptr;
      value = std::strtod(p, &end);
      return end != p;
    };
    if (arrayFirst("temperature_2m_max", number) && number > -100.0 && number < 100.0)
      out.tempMaxTenths = static_cast<int>(number * 10.0 + (number >= 0 ? 0.5 : -0.5));
    if (arrayFirst("temperature_2m_min", number) && number > -100.0 && number < 100.0)
      out.tempMinTenths = static_cast<int>(number * 10.0 + (number >= 0 ? 0.5 : -0.5));
    if (arrayFirst("uv_index_max", number) && number >= 0 && number < 30)
      out.uvMaxTenths = static_cast<int>(number * 10.0 + 0.5);
    const char* sunset = findJsonValue(daily, "sunset");
    if (sunset) {
      // "2026-09-03T20:44" -> copy only "20:44".
      const char* t = std::strchr(sunset, 'T');
      if (t && std::strlen(t) >= 6) {
        std::memcpy(out.sunset, t + 1, 5);
        out.sunset[5] = '\0';
      }
    }
    const char* sunrise = findJsonValue(daily, "sunrise");
    if (sunrise) {
      const char* t = std::strchr(sunrise, 'T');
      if (t && std::strlen(t) >= 6) {
        std::memcpy(out.sunrise, t + 1, 5);
        out.sunrise[5] = '\0';
      }
    }
  }
  out.valid = true;
  return true;
}

bool parseOpenMeteoHttpResponse(const char* response, WeatherCache& out) {
  if (!response) return false;
  // Accept any HTTP/1.x 200 status line (request uses HTTP/1.0, servers may
  // answer with either version).
  if (std::strncmp(response, "HTTP/1.", 7) != 0 || std::strncmp(response + 8, " 200", 4) != 0) return false;
  const char* separator = std::strstr(response, "\r\n\r\n");
  if (!separator) return false;
  const char* body = separator + 4;

  // Detect chunked transfer encoding before header end.
  const char* te = std::strstr(response, "Transfer-Encoding:");
  bool chunked = false;
  if (te && te < separator) {
    te += 18;
    while (*te == ' ') ++te;
    chunked = std::strncmp(te, "chunked", 7) == 0;
  }

  char plain[1535];
  const char* payload = body;
  if (chunked) {
    // Minimal dechunking: hex size line, CRLF, data, CRLF per chunk.
    size_t outLen = 0;
    const char* p = body;
    while (*p && outLen < sizeof(plain) - 1) {
      char* end = nullptr;
      const long size = std::strtol(p, &end, 16);
      if (end == p || size <= 0) break;
      p = end;
      while (*p == '\r' || *p == '\n') ++p;
      const size_t remaining = sizeof(plain) - 1 - outLen;
      const size_t chunk = (static_cast<size_t>(size) > remaining) ? remaining : static_cast<size_t>(size);
      std::memcpy(plain + outLen, p, chunk);
      outLen += chunk;
      p += static_cast<size_t>(size);
      while (*p == '\r' || *p == '\n') ++p;
    }
    plain[outLen] = '\0';
    payload = plain;
  }

  // HTTP/1.0 replies close the connection: the body ends at the captured data.
  // Parse whatever body we have; JSON parser validates the content itself.
  return parseOpenMeteoJson(payload, out);
}

// Timezone is fixed to Europe/Madrid so the timestamps returned for the
// Spanish and Mexican cities stay on the device owner's local wall clock.
static constexpr char OPEN_METEO_PATH_FMT[] =
    "/v1/forecast?latitude=%s%d.%03d&longitude=%s%d.%03d"
    "&current=temperature_2m,apparent_temperature,relative_humidity_2m,wind_speed_10m,wind_direction_10m,uv_index,weather_code,is_day"
    "&daily=temperature_2m_max,temperature_2m_min,uv_index_max,sunrise,sunset"
    "&forecast_days=1&timezone=Europe%%2FMadrid";

// Built outside ARDUINO so the host tests exercise the exact bytes sent to
// Open-Meteo. Uses snprintf only, no heap. Kept above the ARDUINO block so it
// compiles in both firmware and host-test builds.
bool buildOpenMeteoRequest(int cityIndex, char* requestBuf, size_t requestBufSize) {
  if (!requestBuf || requestBufSize < 64) return false;
  const CityEntry& city = CITIES[cityIndexClamped(cityIndex)];
  // Sign comes out of the milli-degree value itself; positive values get no
  // prefix (a literal '+' in a query value risks being decoded as a space).
  const char* latSign = city.latMilli < 0 ? "-" : "";
  const char* lonSign = city.lonMilli < 0 ? "-" : "";
  const int32_t absLat = city.latMilli < 0 ? -city.latMilli : city.latMilli;
  const int32_t absLon = city.lonMilli < 0 ? -city.lonMilli : city.lonMilli;
  char path[320];
  const int pathLen = std::snprintf(path, sizeof(path), OPEN_METEO_PATH_FMT,
      latSign, absLat / 1000, static_cast<int>(absLat % 1000),
      lonSign, absLon / 1000, static_cast<int>(absLon % 1000));
  if (pathLen <= 0 || static_cast<size_t>(pathLen) >= sizeof(path)) return false;
  const int length = std::snprintf(requestBuf, requestBufSize,
      "GET %s HTTP/1.0\r\n"
      "Host: api.open-meteo.com\r\n"
      "Accept: application/json\r\n"
      "\r\n",
      path);
  if (length <= 0 || static_cast<size_t>(length) >= requestBufSize) return false;
  return true;
}

#ifdef ARDUINO
#include "wifi_sync.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>

static constexpr char OPEN_METEO_HOST[] = "api.open-meteo.com";
// ISRG Root X1, obtained from https://letsencrypt.org/certs/isrgrootx1.pem.txt.
static constexpr char OPEN_METEO_ROOT_CA[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----)PEM";

bool weatherFetchCity(int cityIndex, WeatherCache& out, char* status, size_t statusSize) {
  if (status && statusSize) std::snprintf(status, statusSize, "Conectando Wi-Fi...");
  if (!wifiWeatherConnectSaved(6000, status, statusSize)) return false;
  bool ok = false;
  WiFiClientSecure client;
  client.setCACert(OPEN_METEO_ROOT_CA);
  // TLS handshake on the C3 at 80 MHz routinely exceeds 2 s; a short socket
  // timeout made connect() fail before the exchange even started.
  client.setTimeout(8000);
  client.setHandshakeTimeout(10);
  // Static buffer: ~250 chars of URL plus headers, no heap, no String.
  static char request[400];
  if (!buildOpenMeteoRequest(cityIndex, request, sizeof(request))) {
    if (status && statusSize) std::snprintf(status, statusSize, "Peticion invalida");
    client.stop();
    wifiWeatherDisconnect();
    return false;
  }
  if (status && statusSize) std::snprintf(status, statusSize, "Consultando Open-Meteo...");
  if (client.connect(OPEN_METEO_HOST, 443)) {
    client.print(request);
    char response[1535];
    size_t used = 0;
    const unsigned long deadline = millis() + 4000;
    while (millis() < deadline && client.connected() && used < sizeof(response) - 1) {
      while (client.available() && used < sizeof(response) - 1) response[used++] = static_cast<char>(client.read());
      delay(5);
    }
    while (client.available() && used < sizeof(response) - 1) response[used++] = static_cast<char>(client.read());
    response[used] = '\0';
    ok = parseOpenMeteoHttpResponse(response, out);
    if (!ok && status && statusSize) {
      // Show the response's first bytes so a rejected payload is identifiable
      // from the screen alone (status line, chunked marker, etc.).
      char head[24];
      size_t headLen = 0;
      while (headLen < sizeof(head) - 1 && response[headLen] >= 0x20 && response[headLen] < 0x7f) {
        head[headLen] = response[headLen];
        ++headLen;
      }
      head[headLen] = '\0';
      std::snprintf(status, statusSize, "HTTP invalida (%uB) [%s]", static_cast<unsigned>(used), head);
    }
  } else if (status && statusSize) {
    std::snprintf(status, statusSize, "Fallo conexion TLS");
  }
  client.stop();
  wifiWeatherDisconnect();
  // Keep the specific diagnostic set above; only fall back to the generic
  // message when no stage reported a reason.
  if (!ok && status && statusSize && status[0] == '\0') std::snprintf(status, statusSize, "Clima no disponible");
  return ok;
}
#endif
