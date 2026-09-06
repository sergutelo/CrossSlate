// Host test for the city table and the Open-Meteo request builder.
// Build & run (repo root):
//   clang++ -std=c++17 -Wall -I. test/test_city_list.cpp src/weather_client.cpp src/dashboard_parser.cpp -o /tmp/t_cities && /tmp/t_cities
#include "../src/city_list.h"
#include "../src/weather_client.h"

#include <cassert>
#include <cstring>
#include <cstdio>

int main() {
  // ---- Table shape -------------------------------------------------------
  static_assert(CITY_COUNT == 11, "eleven selectable cities");
  assert(std::strcmp(CITIES[0].name, "Madrid") == 0);
  assert(std::strcmp(CITIES[3].name, "Zaragoza") == 0);
  assert(std::strcmp(CITIES[8].name, "Las Palmas de Gran Canaria") == 0);
  assert(std::strcmp(CITIES[10].name, "Tlalnepantla") == 0);

  // Coordinates are integer milli-degrees, negatives intact.
  assert(CITIES[0].latMilli == 40417 && CITIES[0].lonMilli == -3704);
  assert(CITIES[1].latMilli == 41387 && CITIES[1].lonMilli == 2169);
  assert(CITIES[2].lonMilli == -376);                      // Valencia
  assert(CITIES[4].latMilli == 37391 && CITIES[4].lonMilli == -5984);   // Sevilla
  assert(CITIES[8].latMilli == 28124 && CITIES[8].lonMilli == -15436);  // Las Palmas
  assert(CITIES[9].latMilli == 19433 && CITIES[9].lonMilli == -99133);  // CDMX
  assert(CITIES[10].latMilli == 19538 && CITIES[10].lonMilli == -99226);// Tlalnepantla

  // Every name must be non-empty and every coordinate plausible.
  for (int i = 0; i < CITY_COUNT; ++i) {
    assert(CITIES[i].name && CITIES[i].name[0]);
    assert(CITIES[i].latMilli > -90000 && CITIES[i].latMilli < 90000);
    assert(CITIES[i].lonMilli > -180000 && CITIES[i].lonMilli < 180000);
  }

  // Out-of-range indices fall back to Madrid, everywhere.
  assert(cityIndexClamped(-1) == 0);
  assert(cityIndexClamped(0) == 0);
  assert(cityIndexClamped(10) == 10);
  assert(cityIndexClamped(11) == 0);
  assert(cityIndexClamped(999) == 0);

  // ---- Request builder ---------------------------------------------------
  char req[400];

  // Madrid (default): matches the previous hardcoded request.
  assert(buildOpenMeteoRequest(0, req, sizeof(req)));
  assert(std::strstr(req, "GET /v1/forecast?latitude=40.417&longitude=-3.704"));
  assert(std::strstr(req, "HTTP/1.0\r\n"));
  assert(std::strstr(req, "Host: api.open-meteo.com\r\n"));
  assert(std::strstr(req, "Accept: application/json\r\n"));
  assert(std::strstr(req, "&timezone=Europe%2FMadrid"));
  assert(std::strstr(req, "&forecast_days=1"));
  assert(std::strstr(req, "\r\n\r\n"));
  assert(std::strlen(req) < sizeof(req) - 1);  // snprintf not truncated

  // Positive and negative coordinates format with exactly 3 decimals, no '+'.
  assert(buildOpenMeteoRequest(1, req, sizeof(req)));  // Barcelona
  assert(std::strstr(req, "latitude=41.387&longitude=2.169"));
  assert(!std::strstr(req, "+"));

  assert(buildOpenMeteoRequest(9, req, sizeof(req)));  // Ciudad de Mexico
  assert(std::strstr(req, "latitude=19.433&longitude=-99.133"));

  assert(buildOpenMeteoRequest(8, req, sizeof(req)));  // Las Palmas
  assert(std::strstr(req, "latitude=28.124&longitude=-15.436"));

  assert(buildOpenMeteoRequest(3, req, sizeof(req)));  // Zaragoza (.889 lon)
  assert(std::strstr(req, "longitude=-0.889"));

  assert(buildOpenMeteoRequest(2, req, sizeof(req)));  // Valencia (.376 lon)
  assert(std::strstr(req, "longitude=-0.376"));

  // Out-of-range index builds Madrid's request (fetch compatibility).
  assert(buildOpenMeteoRequest(42, req, sizeof(req)));
  assert(std::strstr(req, "latitude=40.417&longitude=-3.704"));
  assert(buildOpenMeteoRequest(-5, req, sizeof(req)));
  assert(std::strstr(req, "latitude=40.417&longitude=-3.704"));

  // Whole-degree boundaries must not print ".000" ambiguity problems:
  // all cities here have non-zero fractional parts, so check padding.
  assert(buildOpenMeteoRequest(7, req, sizeof(req)));  // Palma 2.650
  assert(std::strstr(req, "longitude=2.650"));

  // Buffer-too-small must fail cleanly, not truncate silently.
  char tiny[32];
  assert(!buildOpenMeteoRequest(0, tiny, sizeof(tiny)));
  char nullBuf[400];
  assert(!buildOpenMeteoRequest(0, nullptr, sizeof(nullBuf)));

  std::puts("test_city_list: all assertions passed");
  return 0;
}
