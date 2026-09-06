#pragma once

#include "dashboard_parser.h"

#include <cstddef>
#include <cstdint>

// Pure, bounded parsers used by host tests and the embedded HTTPS client.
bool parseOpenMeteoJson(const char* json, WeatherCache& out);
bool parseOpenMeteoHttpResponse(const char* response, WeatherCache& out);
WeatherIcon weatherIconForWmo(int wmoCode, bool isDay);
const char* weatherSummaryForWmo(int wmoCode);
// Host-testable URL/request builder for a city index into CITIES[].
// Out-of-range index falls back to Madrid. Returns false on any
// truncation or out-of-range coordinate; requestBuf is always NUL-terminated.
bool buildOpenMeteoRequest(int cityIndex, char* requestBuf, size_t requestBufSize);

#ifdef ARDUINO
// Fetches the forecast for CITIES[cityIndex] (clamped to Madrid when out of
// range). Wi-Fi is always shut down before return.
bool weatherFetchCity(int cityIndex, WeatherCache& out, char* status, size_t statusSize);
#endif
