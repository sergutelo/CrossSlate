#pragma once

#include "dashboard_parser.h"

struct DashboardData {
  WeatherCache weather;
  TaskList tasks;
  char pinned[DASHBOARD_PINNED_LEN];
  bool sdAvailable;
  bool hasDoodle;  // /crossslate/doodle.bmp exists; BMP itself is only opened at render time
  char weatherStatus[48];
};

void dashboardSetup();
bool dashboardReload();
bool dashboardRefreshWeather();
const DashboardData& dashboardGetData();
// Selected weather city: index into CITIES[] (city_list.h) and its name.
int dashboardGetCityIndex();
const char* dashboardGetCityName();
// Persists the selection to /crossslate/settings.json (atomic tmp->rename)
// and switches subsequent weather fetches to the new city. 0..CITY_COUNT-1.
bool dashboardSetCityIndex(int index);
