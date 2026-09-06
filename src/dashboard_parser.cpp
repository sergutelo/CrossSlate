#include "dashboard_parser.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* findValue(const char* json, const char* key) {
  if (!json || !key) return nullptr;
  char needle[40];
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

static bool readJsonString(const char* json, const char* key, char* out, size_t outSize) {
  if (!out || outSize == 0) return false;
  out[0] = '\0';
  const char* p = findValue(json, key);
  if (!p || *p++ != '"') return false;
  size_t used = 0;
  while (*p && *p != '"') {
    char c = *p++;
    if (c == '\\') {
      c = *p++;
      if (!c) return false;
      if (c == 'n' || c == 'r' || c == 't') c = ' ';
    }
    if (used + 1 < outSize) out[used++] = c;
  }
  if (*p != '"') return false;
  out[used] = '\0';
  return true;
}

static bool readJsonNumber(const char* json, const char* key, double& out) {
  const char* p = findValue(json, key);
  if (!p) return false;
  char* end = nullptr;
  out = std::strtod(p, &end);
  return end != p;
}

static void extractTimeOfDay(const char* iso, char* out, size_t outSize) {
  // "2026-09-03T20:44" -> "20:44"
  out[0] = '\0';
  if (!iso || outSize < 6) return;
  const char* t = std::strchr(iso, 'T');
  if (!t || std::strlen(t) < 6) return;
  std::memcpy(out, t + 1, 5);
  out[5] = '\0';
}

bool parseWeatherJson(const char* json, WeatherCache& out) {
  std::memset(&out, 0, sizeof(out));
  const char* temp = findValue(json, "temperature_c");
  if (!temp) return false;
  char* end = nullptr;
  const double value = std::strtod(temp, &end);
  if (end == temp || value < -100.0 || value > 100.0) return false;
  out.temperatureTenths = static_cast<int>(value * 10.0 + (value >= 0 ? 0.5 : -0.5));
  if (!readJsonString(json, "summary", out.summary, sizeof(out.summary))) {
    std::strncpy(out.summary, "Sin descripcion", sizeof(out.summary) - 1);
  }
  readJsonString(json, "updated", out.updated, sizeof(out.updated));
  // Extended fields (v2 cache). Old caches without them degrade to zeros/dashes.
  double number = 0;
  if (readJsonNumber(json, "feels_like_c", number) && number > -100.0 && number < 100.0)
    out.feelsLikeTenths = static_cast<int>(number * 10.0 + (number >= 0 ? 0.5 : -0.5));
  if (readJsonNumber(json, "temp_min_c", number) && number > -100.0 && number < 100.0)
    out.tempMinTenths = static_cast<int>(number * 10.0 + (number >= 0 ? 0.5 : -0.5));
  if (readJsonNumber(json, "temp_max_c", number) && number > -100.0 && number < 100.0)
    out.tempMaxTenths = static_cast<int>(number * 10.0 + (number >= 0 ? 0.5 : -0.5));
  if (readJsonNumber(json, "wind_kmh", number) && number >= 0 && number < 500)
    out.windKmhTenths = static_cast<int>(number * 10.0 + 0.5);
  if (readJsonNumber(json, "wind_dir", number) && number >= 0 && number < 360)
    out.windDirDeg = static_cast<int16_t>(number);
  if (readJsonNumber(json, "uv_now", number) && number >= 0 && number < 30) {
    out.uvCurrentTenths = static_cast<int>(number * 10.0 + 0.5);
    out.uvCurrentKnown = true;
  }
  if (readJsonNumber(json, "uv_max", number) && number >= 0 && number < 30)
    out.uvMaxTenths = static_cast<int>(number * 10.0 + 0.5);
  if (readJsonNumber(json, "humidity", number) && number >= 0 && number <= 100)
    out.humidityPct = static_cast<uint8_t>(number + 0.5);
  char tod[16];
  if (readJsonString(json, "sunset", tod, sizeof(tod))) extractTimeOfDay(tod, out.sunset, sizeof(out.sunset));
  if (readJsonString(json, "sunrise", tod, sizeof(tod))) extractTimeOfDay(tod, out.sunrise, sizeof(out.sunrise));
  if (!out.sunset[0]) {
    // writeWeatherAtomically stores the plain "HH:MM"; accept either form.
    readJsonString(json, "sunset", out.sunset, sizeof(out.sunset));
  }
  if (!out.sunrise[0]) {
    readJsonString(json, "sunrise", out.sunrise, sizeof(out.sunrise));
  }
  out.icon = WeatherIcon::CLOUDS;
  const char* icon = findValue(json, "icon");
  if (icon) {
    char* iconEnd = nullptr;
    const long iconValue = std::strtol(icon, &iconEnd, 10);
    if (iconEnd != icon && iconValue >= static_cast<long>(WeatherIcon::SUN) &&
        iconValue <= static_cast<long>(WeatherIcon::STORM)) {
      out.icon = static_cast<WeatherIcon>(iconValue);
    }
  }
  out.valid = true;
  return true;
}

static void copyTrimmedLine(const char* begin, const char* end, char* out, size_t outSize) {
  while (begin < end && std::isspace(static_cast<unsigned char>(*begin))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(end[-1]))) --end;
  size_t len = static_cast<size_t>(end - begin);
  if (len >= outSize) len = outSize - 1;
  if (len) std::memcpy(out, begin, len);
  out[len] = '\0';
}

void parseTasksText(const char* text, TaskList& out) {
  std::memset(&out, 0, sizeof(out));
  if (!text) return;
  const char* line = text;
  while (*line && out.count < DASHBOARD_MAX_TASKS) {
    const char* end = line;
    while (*end && *end != '\n') ++end;
    char candidate[DASHBOARD_TASK_LEN];
    copyTrimmedLine(line, end, candidate, sizeof(candidate));
    if (candidate[0]) {
      std::strncpy(out.items[out.count], candidate, DASHBOARD_TASK_LEN - 1);
      ++out.count;
    }
    line = *end ? end + 1 : end;
  }
}

void parsePinnedText(const char* text, char* out, size_t outSize) {
  if (!out || outSize == 0) return;
  out[0] = '\0';
  if (!text) return;
  const char* end = text;
  while (*end && *end != '\n') ++end;
  copyTrimmedLine(text, end, out, outSize);
}
