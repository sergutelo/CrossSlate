#include "../src/dashboard_parser.h"

#include <cassert>
#include <cstring>

int main() {
  WeatherCache weather{};
  assert(parseWeatherJson("{\"temperature_c\":21.5,\"summary\":\"Soleado\",\"updated\":\"2026-09-01 09:30\"}", weather));
  assert(weather.valid);
  assert(weather.temperatureTenths == 215);
  assert(std::strcmp(weather.summary, "Soleado") == 0);
  assert(std::strcmp(weather.updated, "2026-09-01 09:30") == 0);

  WeatherCache invalid{};
  assert(!parseWeatherJson("{\"summary\":\"sin temperatura\"}", invalid));

  TaskList tasks{};
  parseTasksText(" Primera tarea \r\n\nSegunda tarea\nTercera\nCuarta\nQuinta\nSexta ignorada\n", tasks);
  assert(tasks.count == DASHBOARD_MAX_TASKS);
  assert(std::strcmp(tasks.items[0], "Primera tarea") == 0);
  assert(std::strcmp(tasks.items[1], "Segunda tarea") == 0);
  assert(std::strcmp(tasks.items[4], "Quinta") == 0);

  char pinned[DASHBOARD_PINNED_LEN];
  parsePinnedText("  Idea fijada para hoy  \r\nsegunda linea ignorada", pinned, sizeof(pinned));
  assert(std::strcmp(pinned, "Idea fijada para hoy") == 0);
  return 0;
}
