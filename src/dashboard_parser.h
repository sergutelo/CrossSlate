#pragma once

#include <cstddef>
#include <cstdint>

enum class WeatherIcon : uint8_t {
  SUN,
  NIGHT,
  PARTLY_CLOUDY,
  CLOUDS,
  FOG,
  RAIN,
  SNOW,
  STORM
};

static constexpr int DASHBOARD_MAX_TASKS = 5;
static constexpr int DASHBOARD_TASK_LEN = 80;
static constexpr int DASHBOARD_PINNED_LEN = 36;  // 35 visible chars + NUL: the dashboard clips beyond that anyway

struct WeatherCache {
  bool valid;
  int temperatureTenths;
  int feelsLikeTenths;    // current.apparent_temperature * 10
  int tempMinTenths;      // daily.temperature_2m_min[0] * 10
  int tempMaxTenths;      // daily.temperature_2m_max[0] * 10
  int windKmhTenths;      // current.wind_speed_10m * 10
  int16_t windDirDeg;     // current.wind_direction_10m
  int uvMaxTenths;        // daily.uv_index_max[0] * 10
  int uvCurrentTenths;    // current.uv_index * 10 (true right now)
  bool uvCurrentKnown;
  uint8_t humidityPct;    // current.relative_humidity_2m
  char summary[48];
  char updated[32];
  char sunset[6];         // "20:44" extracted from daily.sunset[0]
  char sunrise[6];        // "07:44" extracted from daily.sunrise[0]
  WeatherIcon icon;
};

struct TaskList {
  int count;
  char items[DASHBOARD_MAX_TASKS][DASHBOARD_TASK_LEN];
};

bool parseWeatherJson(const char* json, WeatherCache& out);
void parseTasksText(const char* text, TaskList& out);
void parsePinnedText(const char* text, char* out, size_t outSize);
