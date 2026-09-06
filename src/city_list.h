#pragma once

// Weather city table shared by the fetch path, the UI and the host tests.
// Coordinates are stored as integer milli-degrees (degrees * 1000) so the
// table lands in flash (.rodata) and no float formatting is ever needed:
// Open-Meteo accepts 3 decimal places (~100 m resolution).
#include <cstddef>
#include <cstdint>

struct CityEntry {
  const char* name;
  int32_t latMilli;  // latitude  * 1000 (negative = south)
  int32_t lonMilli;  // longitude * 1000 (negative = west)
};

static constexpr int CITY_COUNT = 11;
static constexpr CityEntry CITIES[CITY_COUNT] = {
    {"Madrid", 40417, -3704},
    {"Barcelona", 41387, 2169},
    {"Valencia", 39470, -376},
    {"Zaragoza", 41649, -889},
    {"Sevilla", 37391, -5984},
    {"Málaga", 36721, -4421},
    {"Murcia", 37992, -1131},
    {"Palma", 39570, 2650},
    {"Las Palmas de Gran Canaria", 28124, -15436},
    {"Ciudad de Mexico", 19433, -99133},
    {"Tlalnepantla", 19538, -99226},
};

// Any out-of-range index falls back to Madrid (index 0), matching the
// pre-Cities behaviour where the forecast was hardcoded to Madrid.
inline int cityIndexClamped(int index) {
  return (index >= 0 && index < CITY_COUNT) ? index : 0;
}
