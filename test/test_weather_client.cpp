#include "../src/weather_client.h"

#include <cassert>
#include <cstring>

int main() {
  WeatherCache weather{};
  const char* response =
      "HTTP/1.1 200 OK\r\n"
      "Content-Length: 89\r\n"
      "Content-Type: application/json\r\n\r\n"
      "{\"current\":{\"temperature_2m\":21.5,\"weather_code\":2,\"is_day\":1,\"time\":\"2026-09-03T10:15\"}}";
  assert(parseOpenMeteoHttpResponse(response, weather));
  assert(weather.valid);
  assert(weather.temperatureTenths == 215);
  assert(std::strcmp(weather.summary, "Parcialmente nuboso") == 0);
  assert(std::strcmp(weather.updated, "2026-09-03 10:15") == 0);
  assert(weather.icon == WeatherIcon::PARTLY_CLOUDY);

  WeatherCache unitsFirst{};
  assert(parseOpenMeteoJson(
      "{\"current_units\":{\"temperature_2m\":\"°C\",\"weather_code\":\"wmo code\"},"
      "\"current\":{\"temperature_2m\":32.9,\"weather_code\":0,\"is_day\":1,\"time\":\"2026-09-03T13:15\"}}",
      unitsFirst));
  assert(unitsFirst.temperatureTenths == 329);
  assert(unitsFirst.icon == WeatherIcon::SUN);

  // The dashboard must show UVI *now*, never the daily maximum after sunset.
  WeatherCache uvNow{};
  assert(parseOpenMeteoJson(
      "{\"current\":{\"temperature_2m\":20.0,\"uv_index\":0.0,\"weather_code\":0,\"is_day\":0,\"time\":\"2026-09-03T20:56\"},"
      "\"daily\":{\"uv_index_max\":[6.9]}}", uvNow));
  assert(uvNow.uvCurrentTenths == 0);
  assert(uvNow.uvMaxTenths == 69);

  WeatherCache night{};
  assert(parseOpenMeteoJson("{\"current\":{\"temperature_2m\":-1.2,\"weather_code\":0,\"is_day\":0,\"time\":\"2026-09-03T22:00\"}}", night));
  assert(night.temperatureTenths == -12);
  assert(night.icon == WeatherIcon::NIGHT);
  assert(std::strcmp(night.summary, "Despejado") == 0);

  assert(weatherIconForWmo(45, true) == WeatherIcon::FOG);
  assert(weatherIconForWmo(63, true) == WeatherIcon::RAIN);
  assert(weatherIconForWmo(75, true) == WeatherIcon::SNOW);
  assert(weatherIconForWmo(95, true) == WeatherIcon::STORM);

  WeatherCache bad{};
  assert(!parseOpenMeteoHttpResponse("HTTP/1.1 500 Error\r\nContent-Length: 2\r\n\r\n{}", bad));
  assert(!parseOpenMeteoJson("{\"current\":{\"weather_code\":0}}", bad));
  return 0;
}
