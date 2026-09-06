#include "dashboard.h"
#include "config.h"
#include "city_list.h"
#include "weather_client.h"

#include <SDCardManager.h>
#include <cstdio>
#include <cstring>

static DashboardData data{};
static constexpr char WEATHER_PATH[] = "/crossslate/weather.json";
static constexpr char WEATHER_TMP_PATH[] = "/crossslate/weather.tmp";
static constexpr char WEATHER_BAK_PATH[] = "/crossslate/weather.bak";
static constexpr char SETTINGS_PATH[] = "/crossslate/settings.json";
static constexpr char SETTINGS_TMP_PATH[] = "/crossslate/settings.tmp";

// Selected weather city; 0 = Madrid until settings.json says otherwise.
static int weatherCityIndex = 0;

int dashboardGetCityIndex() { return weatherCityIndex; }

const char* dashboardGetCityName() {
  return CITIES[cityIndexClamped(weatherCityIndex)].name;
}

static size_t readSmallFile(const char* path, char* buffer, size_t size) {
  if (!buffer || size < 2) return 0;
  buffer[0] = '\0';
  auto file = SdMan.open(path, O_RDONLY);
  if (!file) return 0;
  const int result = file.read(buffer, size - 1);
  file.close();
  if (result <= 0) return 0;
  buffer[result] = '\0';
  return static_cast<size_t>(result);
}

static bool writeWeatherAtomically(const WeatherCache& weather) {
  char json[512];
  const int absTenths = weather.temperatureTenths < 0 ? -weather.temperatureTenths : weather.temperatureTenths;
  const int absFeels = weather.feelsLikeTenths < 0 ? -weather.feelsLikeTenths : weather.feelsLikeTenths;
  const int absMin = weather.tempMinTenths < 0 ? -weather.tempMinTenths : weather.tempMinTenths;
  const int absMax = weather.tempMaxTenths < 0 ? -weather.tempMaxTenths : weather.tempMaxTenths;
  const int length = std::snprintf(json, sizeof(json),
      "{\"temperature_c\":%s%d.%d,\"feels_like_c\":%s%d.%d,\"temp_min_c\":%s%d.%d,\"temp_max_c\":%s%d.%d,"
      "\"humidity\":%u,\"wind_kmh\":%d.%d,\"wind_dir\":%d,\"uv_now\":%d.%d,\"uv_max\":%d.%d,"
      "\"summary\":\"%s\",\"updated\":\"%s\",\"sunset\":\"%s\",\"sunrise\":\"%s\",\"icon\":%d}",
      weather.temperatureTenths < 0 ? "-" : "", absTenths / 10, absTenths % 10,
      weather.feelsLikeTenths < 0 ? "-" : "", absFeels / 10, absFeels % 10,
      weather.tempMinTenths < 0 ? "-" : "", absMin / 10, absMin % 10,
      weather.tempMaxTenths < 0 ? "-" : "", absMax / 10, absMax % 10,
      static_cast<unsigned>(weather.humidityPct),
      weather.windKmhTenths / 10, weather.windKmhTenths % 10,
      static_cast<int>(weather.windDirDeg),
      weather.uvCurrentTenths / 10, weather.uvCurrentTenths % 10,
      weather.uvMaxTenths / 10, weather.uvMaxTenths % 10,
      weather.summary, weather.updated, weather.sunset, weather.sunrise, static_cast<int>(weather.icon));
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(json)) return false;
  auto file = SdMan.open(WEATHER_TMP_PATH, O_RDWR | O_CREAT | O_TRUNC);
  if (!file) return false;
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(json), static_cast<size_t>(length));
  file.close();
  if (written != static_cast<size_t>(length)) {
    SdMan.remove(WEATHER_TMP_PATH);
    return false;
  }
  if (SdMan.exists(WEATHER_BAK_PATH) && !SdMan.remove(WEATHER_BAK_PATH)) {
    SdMan.remove(WEATHER_TMP_PATH);
    return false;
  }
  if (SdMan.exists(WEATHER_PATH) && !SdMan.rename(WEATHER_PATH, WEATHER_BAK_PATH)) {
    SdMan.remove(WEATHER_TMP_PATH);
    return false;
  }
  if (!SdMan.rename(WEATHER_TMP_PATH, WEATHER_PATH)) {
    if (!SdMan.exists(WEATHER_PATH) && SdMan.exists(WEATHER_BAK_PATH)) SdMan.rename(WEATHER_BAK_PATH, WEATHER_PATH);
    return false;
  }
  auto verify = SdMan.open(WEATHER_PATH, O_RDONLY);
  const bool valid = verify && verify.size() == static_cast<size_t>(length);
  if (verify) verify.close();
  if (!valid) {
    SdMan.remove(WEATHER_PATH);
    if (SdMan.exists(WEATHER_BAK_PATH)) SdMan.rename(WEATHER_BAK_PATH, WEATHER_PATH);
  }
  return valid;
}

// Compact {"city":<index>} write with the same tmp->rename atomicity as
// writeWeatherAtomically, minus the .bak dance: a tiny file that regenerates
// from the in-RAM index, so on rename failure the tmp is removed and retried
// on the next selection.
bool writeSettingsAtomically(int cityIndex) {
  char json[32];
  const int length = std::snprintf(json, sizeof(json), "{\"city\":%d}", cityIndex);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(json)) return false;
  auto file = SdMan.open(SETTINGS_TMP_PATH, O_RDWR | O_CREAT | O_TRUNC);
  if (!file) return false;
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(json), static_cast<size_t>(length));
  file.close();
  if (written != static_cast<size_t>(length)) {
    SdMan.remove(SETTINGS_TMP_PATH);
    return false;
  }
  if (SdMan.exists(SETTINGS_PATH) && !SdMan.remove(SETTINGS_PATH)) {
    SdMan.remove(SETTINGS_TMP_PATH);
    return false;
  }
  if (!SdMan.rename(SETTINGS_TMP_PATH, SETTINGS_PATH)) {
    SdMan.remove(SETTINGS_TMP_PATH);
    return false;
  }
  return true;
}

bool dashboardSetCityIndex(int index) {
  if (index < 0 || index >= CITY_COUNT) return false;
  weatherCityIndex = index;
  if (!(SdMan.ready() || SdMan.begin())) return false;
  const bool saved = writeSettingsAtomically(index);
  SdMan.sleep();
  if (!saved) DBG_PRINTLN("[DASH] No se pudo guardar settings.json");
  return saved;
}

void dashboardSetup() {
  if (!SdMan.exists("/crossslate") && !SdMan.mkdir("/crossslate")) {
    DBG_PRINTLN("[DASH] No /crossslate");
  }
  dashboardReload();
}

bool dashboardReload() {
  char status[sizeof(data.weatherStatus)];
  std::memcpy(status, data.weatherStatus, sizeof(status));
  const int savedCity = weatherCityIndex;
  std::memset(&data, 0, sizeof(data));
  std::memcpy(data.weatherStatus, status, sizeof(data.weatherStatus));
  weatherCityIndex = savedCity;  // the selection outlives a dashboard reload
  data.sdAvailable = SdMan.ready() || SdMan.begin();
  if (!data.sdAvailable) {
    std::snprintf(data.weatherStatus, sizeof(data.weatherStatus), "SD no disponible");
    return false;
  }
  // settings.json is tiny and only read here; a missing/corrupt file or an
  // out-of-range index keeps whatever selection is already in RAM (default 0).
  char settingsJson[64];
  if (readSmallFile(SETTINGS_PATH, settingsJson, sizeof(settingsJson))) {
    const char* key = std::strstr(settingsJson, "\"city\"");
    if (key) {
      const char* value = std::strchr(key + 6, ':');
      if (value) {
        const int parsed = std::atoi(value + 1);
        if (parsed >= 0 && parsed < CITY_COUNT) weatherCityIndex = parsed;
      }
    }
  }
  char weatherJson[512];
  if (readSmallFile(WEATHER_PATH, weatherJson, sizeof(weatherJson))) parseWeatherJson(weatherJson, data.weather);
  char tasksText[512];
  if (readSmallFile("/crossslate/tasks.txt", tasksText, sizeof(tasksText))) parseTasksText(tasksText, data.tasks);
  char pinnedText[DASHBOARD_PINNED_LEN + 32];
  if (readSmallFile("/crossslate/pinned.txt", pinnedText, sizeof(pinnedText)))
    parsePinnedText(pinnedText, data.pinned, sizeof(data.pinned));
  // Existence check only: the doodle BMP is loaded at render time, never here,
  // to avoid holding its pixel data in RAM (RAM budget is tight).
  data.hasDoodle = SdMan.exists("/crossslate/doodle.bmp");
  SdMan.sleep();
  return true;
}

bool dashboardRefreshWeather() {
  if (!dashboardReload()) return false;  // retain any cache already on SD before attempting network.
  std::snprintf(data.weatherStatus, sizeof(data.weatherStatus), "Actualizando clima...");
  WeatherCache fetched{};
  char status[sizeof(data.weatherStatus)] = "";
  const bool fetchedOk = weatherFetchCity(weatherCityIndex, fetched, status, sizeof(status));
  if (!fetchedOk) {
    if (status[0]) std::snprintf(data.weatherStatus, sizeof(data.weatherStatus), "%s", status);
    return false;
  }
  if (!(SdMan.ready() || SdMan.begin()) || !writeWeatherAtomically(fetched)) {
    SdMan.sleep();
    std::snprintf(data.weatherStatus, sizeof(data.weatherStatus), "No se pudo guardar clima");
    return false;
  }
  SdMan.sleep();
  data.weather = fetched;
  data.weatherStatus[0] = '\0';  // timestamp in the card is the only freshness indicator.
  return true;
}

const DashboardData& dashboardGetData() { return data; }
