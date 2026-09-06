#include "ui_renderer.h"

#include "reading_stats.h"
#include "pomodoro.h"
#include <SDCardManager.h>
#include <ctime>
#include "config.h"
#include "city_list.h"
#include "text_editor.h"
#include "file_manager.h"
#include "ble_keyboard.h"
#include "wifi_sync.h"
#include "dashboard.h"
#include "ota_boot_switch.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalDisplay.h>
#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <SDCardManager.h>
#include <ctime>

// External variables
extern bool autoReconnectEnabled;
extern bool darkMode;
extern bool cleanMode;
extern bool deleteConfirmPending;
extern WritingMode writingMode;
extern FontSize fontSize;
extern bool showWordCount;

// External functions
uint32_t getCurrentPasskey();
bool isDeviceScanning();
uint32_t getScanAgeMs();

// Font data includes
#include <builtinFonts/notosans_16_regular.h>
#include <builtinFonts/notosans_16_bold.h>
#include <builtinFonts/notosans_14_regular.h>
#include <builtinFonts/notosans_14_bold.h>
#include <builtinFonts/notosans_12_regular.h>
#include <builtinFonts/notosans_12_bold.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <builtinFonts/ubuntu_10_bold.h>

// Font objects (file-scoped)
static EpdFont ns16Regular(&notosans_16_regular);
static EpdFont ns16Bold(&notosans_16_bold);
static EpdFontFamily ns16Family(&ns16Regular, &ns16Bold);

static EpdFont ns14Regular(&notosans_14_regular);
static EpdFont ns14Bold(&notosans_14_bold);
static EpdFontFamily ns14Family(&ns14Regular, &ns14Bold);

static EpdFont ns12Regular(&notosans_12_regular);
static EpdFont ns12Bold(&notosans_12_bold);
static EpdFontFamily ns12Family(&ns12Regular, &ns12Bold);

static EpdFont u10Regular(&ubuntu_10_regular);
static EpdFont u10Bold(&ubuntu_10_bold);
static EpdFontFamily u10Family(&u10Regular, &u10Bold);

// Extern shared state (defined in main.cpp)
extern UIState currentState;
extern int mainMenuSelection;
extern int selectedFileIndex;
extern int settingsSelection;
extern int bluetoothDeviceSelection;
extern int pairedKeyboardSelection;
extern Orientation currentOrientation;
extern int charsPerLine;
extern char renameBuffer[];
extern int renameBufferLen;

// --- Cities menu state (shared with input_handler.cpp) ---
int citySelection = 0;              // cursor row in the Cities list
unsigned long citySavedAtMs = 0;    // millis() of last save; 0 = no banner

void rendererSetup(GfxRenderer& renderer) {
  renderer.insertFont(FONT_LARGE, ns16Family);
  renderer.insertFont(FONT_BODY, ns14Family);
  renderer.insertFont(FONT_UI, ns12Family);
  renderer.insertFont(FONT_SMALL, u10Family);
}

// ---------------------------------------------------------------------------
// Clipped draw helpers — use renderer.truncatedText() so NO pixel ever
// exceeds screen width.  This is how crosspoint-reader prevents GFX errors.
// ---------------------------------------------------------------------------

// Draw text that is guaranteed not to overflow the screen width.
// maxW = available pixel width from x to right edge (caller computes).
// Falls back to sw - x - 5 if maxW <= 0.
static void drawClippedText(GfxRenderer& r, int font, int x, int y,
                            const char* text, int maxW = 0,
                            bool black = true,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (!text || !text[0]) return;
  int sw = r.getScreenWidth();
  int sh = r.getScreenHeight();
  if (x < 0 || x >= sw || y < 0 || y >= sh) return;

  if (maxW <= 0) maxW = sw - x - 5;   // 5px right margin
  if (maxW <= 0) return;

  auto clipped = r.truncatedText(font, text, maxW, style);
  if (!clipped.empty()) {
    r.drawText(font, x, y, clipped.c_str(), black, style);
  }
}

// Draw right-aligned text (e.g. battery %, RSSI, settings values).
// Computes its own X from the measured text width.
static void drawRightText(GfxRenderer& r, int font, int rightEdge, int y,
                          const char* text, bool black = true,
                          EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (!text || !text[0]) return;
  // Use getTextWidth (bounding box) — same measurement truncatedText uses —
  // so the allocated space always matches what the truncation check expects.
  int tw = r.getTextWidth(font, text, style);
  if (tw <= 0) tw = 30;                    // safe fallback
  int x = rightEdge - tw;
  if (x < 5) x = 5;                        // don't go off left edge
  drawClippedText(r, font, x, y, text, rightEdge - x, black, style);
}

// Safe line — just clamp to screen
static void clippedLine(GfxRenderer& r, int x1, int y1, int x2, int y2,
                        bool state = true) {
  int sw = r.getScreenWidth();
  int sh = r.getScreenHeight();
  // Clamp rather than reject
  auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
  x1 = clamp(x1, 0, sw - 1);
  x2 = clamp(x2, 0, sw - 1);
  y1 = clamp(y1, 0, sh - 1);
  y2 = clamp(y2, 0, sh - 1);
  r.drawLine(x1, y1, x2, y2, state);
}

// Safe fillRect — clamp dimensions to screen
static void clippedFillRect(GfxRenderer& r, int x, int y, int w, int h,
                            bool state = true) {
  int sw = r.getScreenWidth();
  int sh = r.getScreenHeight();
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > sw) w = sw - x;
  if (y + h > sh) h = sh - y;
  if (w > 0 && h > 0) r.fillRect(x, y, w, h, state);
}

// ---------------------------------------------------------------------------
// Helper: draw battery percentage in top-right
// ---------------------------------------------------------------------------
static void drawBattery(GfxRenderer& renderer, HalGPIO& gpio) {
  int pct = gpio.getBatteryPercentage();
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  drawRightText(renderer, FONT_SMALL, renderer.getScreenWidth() - 8, 5, buf, !darkMode);
}

// Helper: draw BLE status
static void drawBleStatus(GfxRenderer& renderer, int x, int y) {
  const char* status = "";
  switch (getConnectionState()) {
    case BLEState::CONNECTED:    status = "KB Connected"; break;
    case BLEState::SCANNING:     status = "Scanning..."; break;
    case BLEState::CONNECTING:   status = "Connecting..."; break;
    case BLEState::DISCONNECTED: status = "KB Disconnected"; break;
  }
  drawClippedText(renderer, FONT_SMALL, x, y, status, 0, !darkMode);
}

// ===========================================================================
// Screen drawing functions
// ===========================================================================

static void drawWeatherIcon(GfxRenderer& r, WeatherIcon icon, int x, int y, bool tc) {
  // Deliberately geometric: it remains clear on monochrome e-ink without bitmaps.
  if (icon == WeatherIcon::SUN || icon == WeatherIcon::NIGHT || icon == WeatherIcon::PARTLY_CLOUDY) {
    clippedFillRect(r, x + 11, y + 11, 14, 14, tc);
    clippedLine(r, x + 18, y + 2, x + 18, y + 8, tc); clippedLine(r, x + 18, y + 28, x + 18, y + 34, tc);
    clippedLine(r, x + 2, y + 18, x + 8, y + 18, tc); clippedLine(r, x + 28, y + 18, x + 34, y + 18, tc);
  }
  if (icon == WeatherIcon::PARTLY_CLOUDY || icon == WeatherIcon::CLOUDS || icon == WeatherIcon::FOG) {
    clippedFillRect(r, x + 5, y + 20, 30, 8, tc);
    clippedFillRect(r, x + 11, y + 15, 16, 8, tc);
  }
  if (icon == WeatherIcon::FOG || icon == WeatherIcon::RAIN || icon == WeatherIcon::SNOW || icon == WeatherIcon::STORM) {
    clippedLine(r, x + 4, y + 30, x + 34, y + 30, tc); clippedLine(r, x + 8, y + 35, x + 30, y + 35, tc);
  }
  if (icon == WeatherIcon::STORM) clippedLine(r, x + 22, y + 28, x + 14, y + 39, tc);
}

// ===========================================================================
// Dashboard doodle
// ===========================================================================

// Draws /crossslate/doodle.bmp in the free space between the pinned note and
// the footer line, scaled to fit (sw-24) x availableH while preserving aspect
// ratio, centered horizontally. Streams the BMP row by row via Bitmap, so RAM
// usage stays at two row buffers regardless of image size. The SD is woken
// implicitly by SdMan's ensureReady() (same pattern as renderSleepWallpaper);
// it is put back to sleep before the footer is drawn. Any failure (missing
// file, bad headers) is a silent no-op: the dashboard must not depend on the
// doodle being present or valid.
static void drawDashboardDoodle(GfxRenderer& renderer, int yAfterPinned, int sw, int sh, bool tc) {
  const int footerLineY = sh - 30;
  const int availableH = footerLineY - yAfterPinned;
  if (availableH < 80) return;  // not enough room: leave the dashboard as-is

  auto file = SdMan.open("/crossslate/doodle.bmp", O_RDONLY);
  if (!file) return;  // missing file or SD unavailable: silent fallback

  Bitmap bitmap(file, /*dithering=*/true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    file.close();
    return;
  }
  const int iw = bitmap.getWidth(), ih = bitmap.getHeight();
  if (iw <= 0 || ih <= 0) {
    file.close();
    return;
  }

  // Scale down proportionally with integer math on the larger ratio, so the
  // doodle always fits the slot without ever touching the footer line.
  int drawW = iw, drawH = ih;
  const int maxW = sw - 24;
  const uint32_t ratioW = (static_cast<uint32_t>(iw) * 1000) / maxW;
  const uint32_t ratioH = (static_cast<uint32_t>(ih) * 1000) / availableH;
  if (ratioW >= ratioH) {
    drawW = maxW;
    drawH = static_cast<int>((static_cast<uint32_t>(ih) * 1000) / ratioW);
  } else {
    drawH = availableH;
    drawW = static_cast<int>((static_cast<uint32_t>(iw) * 1000) / ratioH);
  }
  if (drawW < 1) drawW = 1;
  if (drawH < 1) drawH = 1;

  const int x = (sw - drawW) / 2;
  const int y = yAfterPinned + ((availableH - drawH) / 2);
  renderer.drawBitmap(bitmap, x, y, drawW, drawH);
  file.close();  // only one SD reader may hold a file open

  SdMan.sleep();  // hand the bus back before the footer; next SD access re-inits
}

void drawDashboard(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const bool tc = !darkMode;
  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  drawClippedText(renderer, FONT_BODY, 12, 8, "CrossSlate", sw - 100, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 8, 36, sw - 8, 36, tc);

  char dateTime[48] = "Fecha/hora no sincronizada";
  const time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm local{};
    localtime_r(&now, &local);
    strftime(dateTime, sizeof(dateTime), "%d/%m/%Y  %H:%M", &local);
  }
  drawClippedText(renderer, FONT_UI, 12, 46, dateTime, sw - 24, tc, EpdFontFamily::BOLD);

  const DashboardData& data = dashboardGetData();
  int y = 66;
  {
    // The city name is right-aligned on the date line to free vertical space
    // for the doodle; "CLIMA" stays on its own section line below.
    const char* cityName = dashboardGetCityName();
    const int cityW = renderer.getTextWidth(FONT_SMALL, cityName, EpdFontFamily::BOLD);
    const int cityX = sw - 12 - cityW;
    if (cityX > 150) {  // keep clear of the date (ends ~140px worst case)
      drawClippedText(renderer, FONT_SMALL, cityX, 46, cityName, cityW + 4, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, FONT_SMALL, 12, y, "CLIMA", sw - 24, tc, EpdFontFamily::BOLD);
    } else {
      // Extremely long city name: fall back to the combined section title.
      char weatherTitle[64];
      snprintf(weatherTitle, sizeof(weatherTitle), "CLIMA \xC2\xB7 %s", cityName);
      drawClippedText(renderer, FONT_SMALL, 12, y, weatherTitle, sw - 24, tc, EpdFontFamily::BOLD);
    }
  }
  y += 20;
  if (data.weather.valid) {
    const bool landscape = sw > sh;
    const int cardX = 12, cardW = sw - 24;
    char buf[48];

    // ---- Card 1: summary + big temp + ST + min/max | humidity + wind ----
    const int c1y = y, c1h = 128;
    renderer.drawRect(cardX, c1y, cardW, c1h, tc);
    const int dividerX = cardX + (landscape ? cardW / 2 : (cardW * 3) / 5);
    clippedLine(renderer, dividerX, c1y + 8, dividerX, c1y + c1h - 8, tc);

    // Left half: sky state, big temperature, feels-like, min/max
    const int lw = dividerX - cardX - 12;
    drawClippedText(renderer, FONT_UI, cardX + 10, c1y + 12, data.weather.summary, lw, tc);
    const int absT = data.weather.temperatureTenths < 0 ? -data.weather.temperatureTenths : data.weather.temperatureTenths;
    snprintf(buf, sizeof(buf), "%s%d.%d°", data.weather.temperatureTenths < 0 ? "-" : "", absT / 10, absT % 10);
    drawClippedText(renderer, FONT_LARGE, cardX + 10, c1y + 36, buf, lw, tc, EpdFontFamily::BOLD);
    const int absF = data.weather.feelsLikeTenths < 0 ? -data.weather.feelsLikeTenths : data.weather.feelsLikeTenths;
    if (data.weather.feelsLikeTenths != 0) {
      snprintf(buf, sizeof(buf), "ST %s%d.%d°", data.weather.feelsLikeTenths < 0 ? "-" : "", absF / 10, absF % 10);
      drawClippedText(renderer, FONT_UI, cardX + 10, c1y + 72, buf, lw, tc);
    }
    if (data.weather.tempMinTenths != 0 || data.weather.tempMaxTenths != 0) {
      const int absMin = data.weather.tempMinTenths < 0 ? -data.weather.tempMinTenths : data.weather.tempMinTenths;
      const int absMax = data.weather.tempMaxTenths < 0 ? -data.weather.tempMaxTenths : data.weather.tempMaxTenths;
      snprintf(buf, sizeof(buf), "%d.%d - %d.%d°", absMin / 10, absMin % 10, absMax / 10, absMax % 10);
      drawClippedText(renderer, FONT_UI, cardX + 10, c1y + 98, buf, lw, tc);
    }

    // Right half: humidity + wind + cardinal
    const int rw = cardX + cardW - dividerX - 12;
    if (data.weather.humidityPct > 0) {
      snprintf(buf, sizeof(buf), "Hum. %u %%", static_cast<unsigned>(data.weather.humidityPct));
      drawClippedText(renderer, FONT_UI, dividerX + 10, c1y + 16, buf, rw, tc);
    }
    if (data.weather.windKmhTenths > 0) {
      snprintf(buf, sizeof(buf), "%d.%d km/h", data.weather.windKmhTenths / 10, data.weather.windKmhTenths % 10);
      drawClippedText(renderer, FONT_UI, dividerX + 10, c1y + 50, buf, rw, tc);
      static const char* const kCardinals[8] = {"N", "NE", "E", "SE", "S", "SO", "O", "NO"};
      const int idx = ((data.weather.windDirDeg + 22) % 360) / 45;
      snprintf(buf, sizeof(buf), "Dir: %s", kCardinals[idx]);
      drawClippedText(renderer, FONT_UI, dividerX + 10, c1y + 82, buf, rw, tc, EpdFontFamily::BOLD);
    }
    y = c1y + c1h + 14;

    // ---- Cards 2+3 side by side: UV and sunset ----
    const int half = (cardW - 12) / 2;
    const int c2h = 84;
    renderer.drawRect(cardX, y, half, c2h, tc);
    if (data.weather.uvCurrentKnown) {
      snprintf(buf, sizeof(buf), "%d.%d UV", data.weather.uvCurrentTenths / 10, data.weather.uvCurrentTenths % 10);
      drawClippedText(renderer, FONT_LARGE, cardX + 10, y + 14, buf, half - 20, tc, EpdFontFamily::BOLD);
      const int uvx10 = data.weather.uvCurrentTenths;
      const char* uvLabel = uvx10 <= 20 ? "Bajo" : uvx10 <= 55 ? "Moderado" : uvx10 <= 75 ? "Alto" : uvx10 <= 100 ? "Muy alto" : "Extremo";
      drawClippedText(renderer, FONT_UI, cardX + 10, y + 54, uvLabel, half - 20, tc);
    } else {
      drawClippedText(renderer, FONT_UI, cardX + 10, y + 32, "UV --", half - 20, tc);
    }

    const int c3x = cardX + half + 12, c3w = cardW - half - 12;
    renderer.drawRect(c3x, y, c3w, c2h, tc);
    if (data.weather.sunset[0]) {
      // Sunset icon, bold for e-ink: filled half-disc over a thick horizon plus
      // a solid down arrow. Stacked fillRect rows approximate the arc so it
      // stays visible at 1-bit without hairline strokes washing out.
      const int ix = c3x + 12, iy = y + 20;
      const int cx = ix + 14;  // disc center
      renderer.fillRect(cx - 12, iy + 10, 24, 3, tc);
      renderer.fillRect(cx - 10, iy + 6, 20, 3, tc);
      renderer.fillRect(cx - 6, iy + 3, 12, 3, tc);
      clippedLine(renderer, ix - 2, iy + 14, ix + 30, iy + 14, tc);                   // horizon
      clippedLine(renderer, ix - 2, iy + 15, ix + 30, iy + 15, tc);
      renderer.fillRect(ix + 38, iy + 2, 3, 10, tc);                                  // solid arrow
      renderer.fillRect(ix + 32, iy + 8, 9, 3, tc);
      clippedLine(renderer, ix + 32, iy + 8, ix + 36, iy + 12, tc);
      clippedLine(renderer, ix + 41, iy + 8, ix + 37, iy + 12, tc);
      drawClippedText(renderer, FONT_LARGE, ix + 52, y + 10, data.weather.sunset, c3w - 62, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, FONT_UI, ix + 52, y + 50, "atardecer", c3w - 62, tc);
    } else {
      drawClippedText(renderer, FONT_UI, c3x + 10, y + 32, "Sol --:--", c3w - 20, tc);
    }
    y += c2h + 12;


  } else if (data.weatherStatus[0] == '\0') {
    // No cache and no live status: explain how to get data instead of an empty block.
    drawClippedText(renderer, FONT_UI, 16, y + 10, "Sin cache. Pulsa Derecha.", sw - 32, tc);
    y += 34;
  }
  if (data.weatherStatus[0]) {
    drawClippedText(renderer, FONT_SMALL, 16, y, data.weatherStatus, sw - 32, tc);
    y += 18;
  }

  y += 6;  // breathing room before the tasks section
  drawClippedText(renderer, FONT_SMALL, 12, y, "PROXIMAS TAREAS", sw - 24, tc, EpdFontFamily::BOLD);
  y += 22;
  if (data.tasks.count == 0) {
    drawClippedText(renderer, FONT_UI, 16, y, "Sin tareas", sw - 32, tc);
    y += 24;
  } else {
    // 30 px per row keeps five tasks legible and prevents descenders from
    // colliding with the pinned-note divider.
    for (int i = 0; i < data.tasks.count && y < sh - 150; ++i) {
      char task[DASHBOARD_TASK_LEN + 4];
      snprintf(task, sizeof(task), "- %s", data.tasks.items[i]);
      drawClippedText(renderer, FONT_UI, 16, y, task, sw - 32, tc);
      y += 30;
    }
  }

  if (data.pinned[0] && y < sh - 66) {
    y += 10;
    clippedLine(renderer, 12, y, sw - 12, y, tc);
    y += 14;
    drawClippedText(renderer, FONT_SMALL, 12, y, "NOTA FIJADA", sw - 24, tc, EpdFontFamily::BOLD);
    y += 19;
    drawClippedText(renderer, FONT_UI, 16, y, data.pinned, sw - 32, tc);
    y += 24;  // clear the pinned text line so the doodle slot starts below it
  }

  drawDashboardDoodle(renderer, y, sw, sh, tc);

  clippedLine(renderer, 8, sh - 30, sw - 8, sh - 30, tc);
  drawClippedText(renderer, FONT_SMALL, 12, sh - 23,
                  "Enter: Menu  R: Clima  Esc: Recargar SD", sw - 24, tc);
  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

// ---------------------------------------------------------------------------
// Reading heatmap page (data: CrossInk's /.crosspoint/global_stats.bin)
// ---------------------------------------------------------------------------

// GitHub-style grid: 14 weeks x 7 days, oldest week on the left, Monday on
// the top row. Read day = filled 10x10 cell; today = solid; missed = outline.

// Cached today-as-epoch-day for mood checks (computed once per draw).
static uint32_t g_todayEpochDay = 0;
static uint32_t todayEpochDayCached() { return g_todayEpochDay; }

void drawReadingPage(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const bool tc = !darkMode;
  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  drawClippedText(renderer, FONT_BODY, 12, 8, "Utilidades", sw - 100, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 8, 36, sw - 8, 36, tc);

  const ReadingSnapshot snap = readingSnapshotLoad();

  // Today from the RTC (NTP-synced on boot); falls back to the anchor day.
  uint32_t today = snap.anchorDay + RS_HISTORY_DAYS - 1;
  const time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm utc{};
    gmtime_r(&now, &utc);
    today = readingEpochDayFromYMD(utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
  }
  g_todayEpochDay = today;

  char buf[64];

  // Streak cards side by side
  const int cardY = 52, cardH = 74, cardW = (sw - 36) / 2;
  renderer.drawRect(12, cardY, cardW, cardH, tc);
  renderer.drawRect(12 + cardW + 12, cardY, cardW, cardH, tc);
  drawClippedText(renderer, FONT_UI, 12 + 12, cardY + 10, "Racha actual", cardW - 24, tc);
  const uint16_t cur = readingCurrentStreak(snap, today);
  snprintf(buf, sizeof(buf), "%u dias", static_cast<unsigned>(cur));
  drawClippedText(renderer, FONT_LARGE, 12 + 12, cardY + 34, buf, cardW - 24, tc, EpdFontFamily::BOLD);
  drawClippedText(renderer, FONT_UI, 24 + cardW + 12, cardY + 10, "Record", cardW - 24, tc);
  const uint16_t best = readingLongestStreak(snap);
  snprintf(buf, sizeof(buf), "%u dias", static_cast<unsigned>(best));
  drawClippedText(renderer, FONT_LARGE, 24 + cardW + 12, cardY + 34, buf, cardW - 24, tc, EpdFontFamily::BOLD);

  // Heatmap: 14 weeks x 7 rows
  const int weeks = 14, cell = 14, gap = 9;
  const int gridW = weeks * (cell + gap) - gap;
  const int gridX = (sw - gridW) / 2;  // centered; labels sit in the left margin
  const int gridY = cardY + cardH + 52;  // extra clearance under the label
  drawClippedText(renderer, FONT_UI, 12, cardY + cardH + 14, "Ultimas 14 semanas", sw - 24, tc);

  const uint32_t windowDays = weeks * 7;
  // First day of the window, aligned so its column starts on Monday.
  const uint32_t startDay = today - windowDays + 1;
  const int startDow = static_cast<int>((startDay + 3) % 7);  // 1970-01-01 = Thursday
  // Column 0 covers the first (7 - startDow) days partially; simplest correct
  // approach: iterate absolute days and place by (week, dow) computed from date.
  for (uint32_t d = startDay; d <= today; ++d) {
    const int dow = static_cast<int>((d + 3) % 7);  // 0=Mon..6=Sun
    const int weekCol = static_cast<int>((d - startDay + startDow) / 7);
    if (weekCol >= weeks) continue;
    const int x = gridX + weekCol * (cell + gap);
    const int y = gridY + dow * (cell + gap);
    const bool read = snap.dayBit(d);
    const bool isToday = (d == today);
    if (isToday) {
      renderer.fillRect(x, y, cell, cell, tc);
    } else if (read) {
      renderer.drawRect(x, y, cell, cell, tc);
      renderer.fillRect(x + 3, y + 3, cell - 6, cell - 6, tc);
    } else {
      renderer.drawRect(x, y, cell, cell, tc);
    }
    // First day of a month: center dot as a calendar tick
    const time_t dt = static_cast<time_t>(d) * 86400u;
    struct tm dm{};
    gmtime_r(&dt, &dm);
    if (dm.tm_mday == 1) {
      renderer.fillRect(x + cell / 2 - 2, y + cell / 2 - 2, 4, 4, tc);
    }
  }

  // Weekday letters, vertical, left of the grid: L M X J V S D (one per row).
  // Each letter rendered rotated 90° CW is overkill; instead draw them BELOW
  // the grid under their column-of-month-start. Simpler: tiny letters stacked
  // left, using the rotated text helper for a compact column.
  {
    // Compact labels only on the rows that matter: L, Mi, V, D.
    const char* rowLabels[7] = {"L", "", "X", "", "V", "", "D"};
    for (int r = 0; r < 7; ++r) {
      if (rowLabels[r][0] == '\0') continue;
      drawClippedText(renderer, FONT_SMALL, gridX - 18, gridY + r * (cell + gap) + 4,
                      rowLabels[r], 18, tc);
    }
  }

  // Totals under the grid
  int ty = gridY + 7 * (cell + gap) + 18;
  const uint32_t hours = snap.totalReadingSeconds / 3600u;
  const uint32_t mins = (snap.totalReadingSeconds % 3600u) / 60u;
  snprintf(buf, sizeof(buf), "Total: %uh %um   \xC2\xB7   %u paginas", static_cast<unsigned>(hours),
           static_cast<unsigned>(mins), static_cast<unsigned>(snap.totalPagesTurned));
  drawClippedText(renderer, FONT_UI, 12, ty, buf, sw - 24, tc);
  ty += 34;

  // Pomodoro panel
  renderer.drawRect(12, ty, sw - 24, 104, tc);
  drawClippedText(renderer, FONT_UI, 24, ty + 10, "Pomodoro", sw - 48, tc, EpdFontFamily::BOLD);
  const uint32_t rem = pomodoro::remainingSeconds();
  snprintf(buf, sizeof(buf), "%02u:%02u", static_cast<unsigned>(rem / 60u), static_cast<unsigned>(rem % 60u));
  drawClippedText(renderer, FONT_LARGE, 24, ty + 36, buf, 200, tc, EpdFontFamily::BOLD);
  const char* stateLabel = "Enter: iniciar";
  switch (pomodoro::state()) {
    case pomodoro::State::Running: stateLabel = "Enter: pausa"; break;
    case pomodoro::State::Paused: stateLabel = "Enter: seguir"; break;
    case pomodoro::State::Finished: stateLabel = "\xC2\xA1Listo! Enter: reset"; break;
    default: break;
  }
  drawClippedText(renderer, FONT_UI, 24, ty + 70, stateLabel, sw - 48, tc);
  // Duration + completed count on the right
  snprintf(buf, sizeof(buf), "%umin", static_cast<unsigned>(pomodoro::totalSeconds() / 60u));
  drawClippedText(renderer, FONT_UI, sw - 120, ty + 38, buf, 100, tc, EpdFontFamily::BOLD);
  snprintf(buf, sizeof(buf), "Hoy: %u", static_cast<unsigned>(pomodoro::completedToday()));
  drawClippedText(renderer, FONT_UI, sw - 120, ty + 70, buf, 100, tc);

  if (!snap.valid) {
    drawClippedText(renderer, FONT_SMALL, 12, ty + 96, "(Sin datos de CrossInk: lee un libro y vuelve)", sw - 24, tc);
  }

  // ---- Crossi: the reading pet ----
  {
    const int petTop = ty + 118;
    const int petH = sh - 30 - petTop - 26;  // leave room for caption + footer
    if (petH > 90) {
      drawClippedText(renderer, FONT_UI, 12, petTop, "Crossi hoy est\xC3\xA1:", sw - 24, tc, EpdFontFamily::BOLD);

      // Mood: read today -> happy; missed 2+ days -> sleepy; else neutral.
      const uint32_t today = todayEpochDayCached();
      const bool readToday = snap.dayBit(today);
      const bool readYesterday = snap.dayBit(today - 1);
      const bool read2Ago = snap.dayBit(today - 2);
      const int mood = readToday ? 0 : (!readYesterday && !read2Ago ? 2 : 1);

      // Accessory: chosen once per entry into the page (static survives the
      // per-second countdown redraws) so Crossi's outfit doesn't flicker.
      static int accessory = -1;
      static uint32_t accessorySeedDay = 0;
      if (accessory < 0 || accessorySeedDay != today) {
        accessorySeedDay = today;
        const time_t nowAcc = time(nullptr);
        struct tm utcAcc{};
        gmtime_r(&nowAcc, &utcAcc);
        if (utcAcc.tm_mon == 11) {
          accessory = 3;  // Santa all December
        } else {
          const int roll = static_cast<int>(random(10));
          accessory = (roll < 5) ? 0 : (roll < 7 ? 1 : 2);  // 50% none, 20% bow, 30% top hat
        }
      }

      // Body: chunky rectangle centered (10% smaller than v22).
      const int bodyW = sw - 320;
      const int bodyH = petH - 70;
      const int bx = (sw - bodyW) / 2;
      const int by = petTop + 24;

      // Side "ears": outlined rectangles on left and right edges (negative look)
      const int earW = 20, earH = 108;  // triple-length arms
      renderer.drawRect(bx - earW / 2, by + bodyH / 4, earW, earH, tc);
      renderer.drawRect(bx + bodyW - earW / 2, by + bodyH / 4, earW, earH, tc);
      // Body: outline only (negative look: no black fill)
      renderer.drawRect(bx - 4, by - 4, bodyW + 8, bodyH + 8, tc);
      renderer.drawRect(bx, by, bodyW, bodyH, tc);

      // Feet: two stubby outlined rectangles below
      const int footW = 42, footH = 14;
      renderer.drawRect(bx + bodyW / 4 - footW / 2, by + bodyH, footW, footH, tc);
      renderer.drawRect(bx + 3 * bodyW / 4 - footW / 2, by + bodyH, footW, footH, tc);

      const int cx1 = bx + bodyW / 4;
      const int cx2 = bx + 3 * bodyW / 4;
      const int eyeY = by + bodyH / 3;
      if (mood == 0) {
        // Happy: >< eyes (two angled lines each)
        for (int k = 0; k < 4; ++k) {
          renderer.fillRect(cx1 - 12 + k * 4, eyeY + k * 3, 4, 4, tc);
          renderer.fillRect(cx1 + 8 - k * 4, eyeY + k * 3, 4, 4, tc);
          renderer.fillRect(cx2 - 12 + k * 4, eyeY + k * 3, 4, 4, tc);
          renderer.fillRect(cx2 + 8 - k * 4, eyeY + k * 3, 4, 4, tc);
        }
      } else if (mood == 2) {
        // Sleepy: horizontal closed-eye bars + Zzz
        renderer.fillRect(cx1 - 14, eyeY + 6, 28, 5, tc);
        renderer.fillRect(cx2 - 14, eyeY + 6, 28, 5, tc);
        drawClippedText(renderer, FONT_LARGE, bx + bodyW - 60, by - 16, "z z", 60, tc, EpdFontFamily::BOLD);
      } else {
        // Neutral: vertical rectangular eyes
        renderer.fillRect(cx1 - 7, eyeY, 14, 26, tc);
        renderer.fillRect(cx2 - 7, eyeY, 14, 26, tc);
      }
      // Mouth: small line
      renderer.fillRect(bx + bodyW / 2 - 14, by + 2 * bodyH / 3, 28, 4, tc);

      // Accessories
      if (accessory == 1) {
        // Bow: outlined
        renderer.fillRect(bx + bodyW / 2 - 40, by - 14, 18, 12, tc);
        renderer.fillRect(bx + bodyW / 2 + 22, by - 14, 18, 12, tc);
        renderer.drawRect(bx + bodyW / 2 - 8, by - 12, 16, 8, tc);
      } else if (accessory == 2) {
        // Top hat: outlined brim + cylinder
        renderer.drawRect(bx + bodyW / 2 - 52, by - 16, 104, 8, tc);
        renderer.drawRect(bx + bodyW / 2 - 32, by - 52, 64, 38, tc);
      } else if (accessory == 3) {
        // Santa hat: brim + stepped triangle (kept filled, small enough)
        renderer.fillRect(bx + bodyW / 2 - 48, by - 18, 96, 10, tc);
        for (int k = 0; k < 8; ++k) {
          renderer.fillRect(bx + bodyW / 2 - 44 + k * 6, by - 18 - (8 - k) * 4, 40 - k * 5, 4, tc);
        }
        renderer.fillRect(bx + bodyW / 2 + 40, by - 46, 12, 12, tc);
      }

      // Caption
      const char* caption = (mood == 0) ? "\xC2\xA1Feliz! \xC2\xA1Has le\xC3\ADdo hoy."
                          : (mood == 2) ? "Dormido... \xC2\xBFy la lectura?"
                          : "Tranquilo. A\xC3\BAn hay tiempo.";
      drawClippedText(renderer, FONT_UI, 12, petTop + petH - 2, caption, sw - 24, tc);
    }
  }

  clippedLine(renderer, 8, sh - 30, sw - 8, sh - 30, tc);
  drawClippedText(renderer, FONT_SMALL, 12, sh - 23, "Atr\xC3\xA1s: Men\xC3\xBA  Lados: \xC2\xB11min  Der 2s: Reset", sw - 24, tc);

  // Running countdown: fast refresh avoids the long black/white flash of a
  // full waveform; the 1-bit ghosting it leaves is cleared on the next full
  // refresh (any state change redraws).
  const bool running = pomodoro::state() == pomodoro::State::Running;
  // First draw after boot must use a full waveform: the panel still holds the
  // sleep-screen image and a fast refresh would leave it ghosted underneath.
  static bool firstDrawSinceBoot = true;
  const bool full = !running || firstDrawSinceBoot;
  firstDrawSinceBoot = false;
  renderer.beginRefresh(full ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
}

void drawMainMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;  // text color

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  // Title
  renderer.drawCenteredText(FONT_BODY, 30, "CrossSlate", tc, EpdFontFamily::BOLD);

  // The reciprocal boot action only exists when its alternate slot validates.
  static const char* baseMenuItems[] = {"Dashboard", "Utilidades", "Seleccionar nota", "Crear nota", "Ajustes", "Sincronizar"};
  const int baseCount = 6;
  int menuCount = baseCount + (otaBootCrossInkAvailable() ? 1 : 0);
  for (int i = 0; i < menuCount; i++) {
    int yPos = 90 + (i * 45);
    const char* label = (i < baseCount) ? baseMenuItems[i] : "Volver a CrossInk";
    if (i == mainMenuSelection) {
      clippedFillRect(renderer, 5, yPos - 5, sw - 10, 35, tc);
      drawClippedText(renderer, FONT_UI, 20, yPos, label, sw - 40, !tc);
    } else {
      drawClippedText(renderer, FONT_UI, 20, yPos, label, sw - 40, tc);
    }
  }

  // Footer
  constexpr int bm = 60;
  if (sh > bm + 40) {
    clippedLine(renderer, 10, sh - bm, sw - 10, sh - bm, tc);
    drawClippedText(renderer, FONT_SMALL, 20, sh - bm + 12, "Arrows: Navigate  Enter: Select", 0, tc);
    drawBleStatus(renderer, 20, sh - bm + 28);
  }
  drawBattery(renderer, gpio);

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawFileBrowser(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  // Header
  drawClippedText(renderer, FONT_SMALL, 10, 5, "Notes", 0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  int fc = getFileCount();
  int lineH = 30;
  int listTop = 42;
  int footerH = 28;  // one line of FONT_SMALL with safe bottom margin
  int maxVisible = (sh - listTop - footerH) / lineH;
  int startIdx = 0;
  if (fc > maxVisible && selectedFileIndex >= maxVisible) {
    startIdx = selectedFileIndex - maxVisible + 1;
  }

  if (fc == 0) {
    drawClippedText(renderer, FONT_UI, 20, listTop + 14, "No notes yet.", 0, tc);
    drawClippedText(renderer, FONT_SMALL, 20, listTop + 36, "Press Ctrl+N to create one.", 0, tc);
  }

  FileInfo* files = getFileList();
  for (int i = startIdx; i < fc && (i - startIdx) < maxVisible; i++) {
    int yPos = listTop + (i - startIdx) * lineH;

    if (i == selectedFileIndex) {
      clippedFillRect(renderer, 5, yPos - 3, sw - 10, lineH - 1, tc);
      drawClippedText(renderer, FONT_UI, 15, yPos, files[i].title, sw - 30, !tc);
    } else {
      drawClippedText(renderer, FONT_UI, 15, yPos, files[i].title, sw - 30, tc);
    }
  }

  // Footer
  clippedLine(renderer, 5, sh - footerH - 2, sw - 5, sh - footerH - 2, tc);
  if (deleteConfirmPending && fc > 0) {
    drawClippedText(renderer, FONT_SMALL, 10, sh - footerH + 4, "Delete? Enter:Yes  Esc:No", 0, tc);
  } else {
    drawClippedText(renderer, FONT_SMALL, 10, sh - footerH + 4,
                    "Ctrl+N:Title  Ctrl+D:Delete", 0, tc);
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

// Helper: draw a single editor line from the buffer
static void drawEditorLine(GfxRenderer& renderer, int lineIdx, int x, int yPos,
                           int maxW, bool tc) {
  char* buf = editorGetBuffer();
  size_t bufLen = editorGetLength();
  int totalLines = editorGetLineCount();

  int lineStart = editorGetLinePosition(lineIdx);
  int lineEnd = (lineIdx + 1 < totalLines) ? editorGetLinePosition(lineIdx + 1) : (int)bufLen;
  int dispEnd = lineEnd;
  if (dispEnd > lineStart && buf[dispEnd - 1] == '\n') dispEnd--;

  int len = dispEnd - lineStart;
  if (len > 0) {
    char lineBuf[256];
    int copyLen = (len < (int)sizeof(lineBuf) - 1) ? len : (int)sizeof(lineBuf) - 1;
    strncpy(lineBuf, buf + lineStart, copyLen);
    lineBuf[copyLen] = '\0';
    drawClippedText(renderer, editorFontId(fontSize), x, yPos, lineBuf, maxW, tc);
  }
}

// Helper: draw cursor at the given screen position
static void drawEditorCursor(GfxRenderer& renderer, int cursorY, int lineHeight,
                             int sw, bool tc) {
  int curLine = editorGetCursorLine();
  int curCol = editorGetCursorCol();
  char* buf = editorGetBuffer();

  int lineStart = editorGetLinePosition(curLine);
  char prefix[256];
  int prefixLen = (curCol < (int)sizeof(prefix) - 1) ? curCol : (int)sizeof(prefix) - 1;
  strncpy(prefix, buf + lineStart, prefixLen);
  prefix[prefixLen] = '\0';

  int cursorX = 10 + renderer.getTextAdvanceX(editorFontId(fontSize), prefix);
  int cursorW = renderer.getSpaceWidth(editorFontId(fontSize));
  if (cursorW < 2) cursorW = 8;

  if (cursorX >= 0 && cursorX + cursorW <= sw && cursorY >= 0 && cursorY + lineHeight <= renderer.getScreenHeight()) {
    renderer.fillRect(cursorX, cursorY, cursorW, lineHeight, tc);
  }
}

// Get the mode indicator string for the current writing mode
static const char* getModeIndicator() {
  switch (writingMode) {
    case WritingMode::TYPEWRITER: return "[T]";
    case WritingMode::PAGINATION: return "[P]";
    default:                      return "[S]";
  }
}

// Helper: draw the standard editor header, returns textAreaTop
// centerText is optional text drawn centered in the header (e.g. "Page 1/3")
static int drawEditorHeader(GfxRenderer& renderer, HalGPIO& gpio, int sw, bool tc,
                            const char* centerText = nullptr) {
  if (cleanMode) return 8;

  const char* title = editorGetCurrentTitle();
  char headerBuf[64];
  if (editorHasUnsavedChanges()) {
    snprintf(headerBuf, sizeof(headerBuf), "%s *", title);
  } else {
    strncpy(headerBuf, title, sizeof(headerBuf) - 1);
    headerBuf[sizeof(headerBuf) - 1] = '\0';
  }
  // Mode indicator — fixed position, right-anchored before battery
  const char* modeInd = getModeIndicator();
  int modeW = renderer.getTextAdvanceX(FONT_SMALL, modeInd);
  int modeX = sw - 70 - modeW;
  drawClippedText(renderer, FONT_SMALL, modeX, 5, modeInd, modeW + 5, tc);

  // Word count — drawn to the left of the mode indicator
  int titleMaxW = modeX - 10;
  if (showWordCount) {
    int wc = editorGetWordCount();
    char wcBuf[24];
    if (wc == 1) snprintf(wcBuf, sizeof(wcBuf), "1 word");
    else         snprintf(wcBuf, sizeof(wcBuf), "%d words", wc);
    int wcW = renderer.getTextAdvanceX(FONT_SMALL, wcBuf);
    int wcX = modeX - 8 - wcW;
    if (wcX > 10) {
      drawClippedText(renderer, FONT_SMALL, wcX, 5, wcBuf, wcW + 5, tc);
      titleMaxW = wcX - 10;
    }
  }

  // Title — stops before word count (or mode indicator if word count hidden)
  drawClippedText(renderer, FONT_SMALL, 10, 5, headerBuf, titleMaxW, tc, EpdFontFamily::BOLD);

  // Centered text (e.g. page indicator)
  if (centerText) {
    int ctW = renderer.getTextAdvanceX(FONT_SMALL, centerText);
    drawClippedText(renderer, FONT_SMALL, (sw - ctW) / 2, 5, centerText, ctW + 5, tc);
  }

  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);
  return 38;
}

void drawTextEditor(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  int lineHeight = renderer.getLineHeight(editorFontId(fontSize));
  if (lineHeight <= 0) lineHeight = 20;
  int totalLines = editorGetLineCount();
  int curLine = editorGetCursorLine();

  // --- TYPEWRITER MODE ---
  if (writingMode == WritingMode::TYPEWRITER) {
    // In clean mode (Ctrl+Z): just text on blank screen, no header
    int textAreaTop = cleanMode ? 0 : drawEditorHeader(renderer, gpio, sw, tc);

    // Center the current line vertically
    int textAreaHeight = sh - textAreaTop;
    int centerY = textAreaTop + (textAreaHeight / 2) - (lineHeight / 2);

    // Draw only the current line
    if (curLine < totalLines) {
      drawEditorLine(renderer, curLine, 10, centerY, sw - 20, tc);
    }

    // Draw cursor
    drawEditorCursor(renderer, centerY, lineHeight, sw, tc);

    editorSetVisibleLines(1);

    renderer.beginRefresh(HalDisplay::FAST_REFRESH);
    return;
  }

  // --- PAGINATION MODE ---
  if (writingMode == WritingMode::PAGINATION) {
    // Pre-compute page info for the header
    // Use a temporary linesPerPage estimate (will be exact since header height is fixed)
    int tempTextTop = cleanMode ? 8 : 38;
    int tempLinesPerPage = (sh - 5 - tempTextTop) / lineHeight;
    if (tempLinesPerPage < 1) tempLinesPerPage = 1;
    int currentPage = curLine / tempLinesPerPage;
    int totalPages = (totalLines + tempLinesPerPage - 1) / tempLinesPerPage;
    if (totalPages < 1) totalPages = 1;

    char pageStr[16];
    snprintf(pageStr, sizeof(pageStr), "Pg %d/%d", currentPage + 1, totalPages);
    int textAreaTop = drawEditorHeader(renderer, gpio, sw, tc, pageStr);

    int textAreaBottom = sh - 5;
    int textAreaHeight = textAreaBottom - textAreaTop;
    int linesPerPage = textAreaHeight / lineHeight;
    if (linesPerPage < 1) linesPerPage = 1;

    // Recompute with actual linesPerPage if it differs
    currentPage = curLine / linesPerPage;
    int pageStart = currentPage * linesPerPage;

    editorSetVisibleLines(linesPerPage);

    // Draw lines for this page
    for (int i = 0; i < linesPerPage && (pageStart + i) < totalLines; i++) {
      int yPos = textAreaTop + (i * lineHeight);
      drawEditorLine(renderer, pageStart + i, 10, yPos, sw - 20, tc);
    }

    // Draw cursor if on this page
    if (curLine >= pageStart && curLine < pageStart + linesPerPage) {
      int cursorY = textAreaTop + ((curLine - pageStart) * lineHeight);
      drawEditorCursor(renderer, cursorY, lineHeight, sw, tc);
    }

    renderer.beginRefresh(HalDisplay::FAST_REFRESH);
    return;
  }

  // --- NORMAL MODE ---
  int textAreaTop = drawEditorHeader(renderer, gpio, sw, tc);

  int textAreaBottom = sh - 5;
  int textAreaHeight = textAreaBottom - textAreaTop;
  int visibleLines = textAreaHeight / lineHeight;

  editorSetVisibleLines(visibleLines);

  int vpStart = editorGetViewportStart();
  char* buf = editorGetBuffer();
  size_t bufLen = editorGetLength();

  // Draw visible lines
  for (int i = 0; i < visibleLines && (vpStart + i) < totalLines; i++) {
    int yPos = textAreaTop + (i * lineHeight);
    drawEditorLine(renderer, vpStart + i, 10, yPos, sw - 20, tc);
  }

  // Draw cursor
  if (curLine >= vpStart && curLine < vpStart + visibleLines) {
    int cursorY = textAreaTop + ((curLine - vpStart) * lineHeight);
    drawEditorCursor(renderer, cursorY, lineHeight, sw, tc);
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawRenameScreen(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  drawClippedText(renderer, FONT_SMALL, 10, 5, "Edit Title", 0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  drawClippedText(renderer, FONT_SMALL, 20, 42, "Note title:", 0, tc);
  int boxY = 64, boxH = 36;
  int textY = boxY + 8;
  renderer.drawRect(15, boxY, sw - 30, boxH, tc);
  drawClippedText(renderer, FONT_UI, 20, textY, renameBuffer, sw - 50, tc);

  // Cursor — thin bar aligned with text
  int cursorX = 20 + renderer.getTextAdvanceX(FONT_UI, renameBuffer);
  if (cursorX + 2 < sw - 15)
    renderer.fillRect(cursorX, textY, 2, 16, tc);

  // Footer
  clippedLine(renderer, 5, sh - 36, sw - 5, sh - 36, tc);
  drawClippedText(renderer, FONT_SMALL, 10, sh - 30, "Enter: Confirm   Esc: Cancel", 0, tc);

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawSettingsMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  drawClippedText(renderer, FONT_SMALL, 10, 5, "Settings", 0, !darkMode, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, !darkMode);

  // Setting items: Orientation, Dark Mode, Writing Mode, Font Size, Bluetooth, Paired Keyboards, Cities
  static const char* labels[] = {
    "Orientation", "Dark Mode", "Writing Mode", "Font Size", "Bluetooth", "Paired Keyboards", "Cities"
  };
  const int SETTINGS_COUNT = 7;

  // Compute line height to fit all items — use smaller spacing if needed
  int lineH = 38;
  int listTop = 50;
  if (listTop + SETTINGS_COUNT * lineH > sh - 70) {
    lineH = (sh - 70 - listTop) / SETTINGS_COUNT;
    if (lineH < 24) lineH = 24;
  }

  for (int i = 0; i < SETTINGS_COUNT; i++) {
    int yPos = listTop + (i * lineH);
    bool sel = (i == settingsSelection);

    if (sel) {
      clippedFillRect(renderer, 5, yPos - 5, sw - 10, lineH - 6, !darkMode);
      drawClippedText(renderer, FONT_UI, 15, yPos, labels[i], sw / 2 - 15, darkMode);
    } else {
      drawClippedText(renderer, FONT_UI, 15, yPos, labels[i], sw / 2 - 15, !darkMode);
    }

    // Value on the right
    char val[32] = "";
    if (i == 0) {
      switch (currentOrientation) {
        case Orientation::PORTRAIT:      strcpy(val, "Portrait"); break;
        case Orientation::LANDSCAPE_CW:  strcpy(val, "Landscape CW"); break;
        case Orientation::PORTRAIT_INV:  strcpy(val, "Inverted"); break;
        case Orientation::LANDSCAPE_CCW: strcpy(val, "Landscape CCW"); break;
      }
    } else if (i == 1) {
      strcpy(val, darkMode ? "Dark" : "Light");
    } else if (i == 2) {
      switch (writingMode) {
        case WritingMode::NORMAL:     strcpy(val, "Normal"); break;
        case WritingMode::TYPEWRITER: strcpy(val, "Typewriter"); break;
        case WritingMode::PAGINATION: strcpy(val, "Pagination"); break;
      }
    } else if (i == 3) {
      switch (fontSize) {
        case FontSize::SMALL:  strcpy(val, "Small"); break;
        case FontSize::MEDIUM: strcpy(val, "Medium"); break;
        default:               strcpy(val, "Large"); break;
      }
    } else if (i == 5) {
      int kbCount = getPairedKeyboardCount();
      if (kbCount == 0) strcpy(val, "None");
      else if (kbCount == 1) strcpy(val, "1 keyboard");
      else snprintf(val, sizeof(val), "%d keyboards", kbCount);
    } else if (i == 6) {
      snprintf(val, sizeof(val), "%s", dashboardGetCityName());
    }

    if (val[0] != '\0') {
      drawRightText(renderer, FONT_UI, sw - 20, yPos, val, sel ? darkMode : !darkMode);
    }
  }

  // Footer
  constexpr int bm = 60;
  if (sh > bm + 30) {
    clippedLine(renderer, 10, sh - bm, sw - 10, sh - bm, !darkMode);
    drawClippedText(renderer, FONT_SMALL, 20, sh - bm + 12,
                    "Arrows:Navigate  Enter:Change  Esc:Back", 0, !darkMode);
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawCitiesMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  drawClippedText(renderer, FONT_SMALL, 10, 5, "Cities", 0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  const int current = dashboardGetCityIndex();
  // Scrolling window of visible rows so all 11 cities stay reachable on the
  // small portrait panel (same pattern as the file browser).
  constexpr int lineH = 26;
  const int listTop = 42;
  const int footerH = 48;
  int maxVisible = (sh - listTop - footerH) / lineH;
  if (maxVisible > CITY_COUNT) maxVisible = CITY_COUNT;
  if (maxVisible < 1) maxVisible = 1;
  int startIdx = 0;
  if (CITY_COUNT > maxVisible && citySelection >= maxVisible) {
    startIdx = citySelection - maxVisible + 1;
  }

  for (int i = startIdx; i < CITY_COUNT && (i - startIdx) < maxVisible; i++) {
    const int yPos = listTop + (i - startIdx) * lineH;
    const bool sel = (i == citySelection);

    // '>' marks the city the weather currently uses (independent of which
    // row the cursor is on).
    char row[48];
    snprintf(row, sizeof(row), "%c %s", i == current ? '>' : ' ', CITIES[i].name);

    if (sel) {
      clippedFillRect(renderer, 5, yPos - 3, sw - 10, lineH - 1, tc);
      drawClippedText(renderer, FONT_UI, 15, yPos, row, sw - 30, !tc);
    } else {
      drawClippedText(renderer, FONT_UI, 15, yPos, row, sw - 30, tc);
    }
  }

  // Transient confirmation banner after a save ("Ciudad: X" for ~1.5 s).
  if (citySavedAtMs != 0 && millis() - citySavedAtMs < 1500) {
    char saved[64];
    snprintf(saved, sizeof(saved), "Ciudad: %s", CITIES[cityIndexClamped(citySelection)].name);
    clippedLine(renderer, 10, sh - footerH, sw - 10, sh - footerH, tc);
    drawClippedText(renderer, FONT_UI, 10, sh - footerH + 10, saved, sw - 20, tc, EpdFontFamily::BOLD);
    drawClippedText(renderer, FONT_SMALL, 10, sh - footerH + 30,
                    "R en el panel actualiza el clima", 0, tc);
  } else {
    constexpr int bm = footerH;
    if (sh > bm + 30) {
      clippedLine(renderer, 10, sh - bm, sw - 10, sh - bm, tc);
      drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 12,
                      "Up/Down:Navigate  Enter:Select", 0, tc);
      drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 26, "Esc:Back sin guardar", 0, tc);
    }
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawBluetoothSettings(GfxRenderer& renderer, HalGPIO& gpio) {
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  renderer.clearScreen();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  // Header
  drawClippedText(renderer, FONT_SMALL, 10, 5, "Bluetooth Devices", 0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  // Connection status
  const char* status = "";
  switch (getConnectionState()) {
    case BLEState::CONNECTED:    status = "Connected to keyboard"; break;
    case BLEState::SCANNING:     status = "Scanning for devices..."; break;
    case BLEState::CONNECTING:   status = "Connecting..."; break;
    case BLEState::DISCONNECTED: status = "Not connected"; break;
  }
  drawClippedText(renderer, FONT_SMALL, 10, 45, status, sw / 2 - 10, tc);

  // Paired device info
  std::string storedAddr, storedName;
  if (getStoredDevice(storedAddr, storedName)) {
    char pairedStr[64];
    snprintf(pairedStr, sizeof(pairedStr), "Paired: %s", storedName.c_str());
    drawClippedText(renderer, FONT_SMALL, sw / 2, 45, pairedStr, sw / 2 - 10, tc);
  }

  // Passkey display
  uint32_t passkey = getCurrentPasskey();
  if (passkey > 0) {
    char passkeyStr[32];
    drawClippedText(renderer, FONT_UI, 20, 100, "PAIRING CODE:", 0, tc, EpdFontFamily::BOLD);
    snprintf(passkeyStr, sizeof(passkeyStr), "%06lu", passkey);
    drawClippedText(renderer, FONT_BODY, 20, 130, passkeyStr, 0, tc, EpdFontFamily::BOLD);
    drawClippedText(renderer, FONT_SMALL, 20, 160, "Type this code on your keyboard", 0, tc);
    drawClippedText(renderer, FONT_SMALL, 20, 180, "then press Enter", 0, tc);
  } else if (isDeviceScanning()) {
    static uint8_t dotPhase = 0;
    static uint32_t lastAnimMs = 0;
    if (millis() - lastAnimMs > 900) {
      dotPhase = (dotPhase + 1) % 4;
      lastAnimMs = millis();
    }
    std::string dots(dotPhase, '.');
    char scanningStr[64];
    int deviceCount = getDiscoveredDeviceCount();
    snprintf(scanningStr, sizeof(scanningStr), "Searching for devices%s", dots.c_str());
    drawClippedText(renderer, FONT_SMALL, 10, 60, scanningStr, sw / 2 - 10, tc);

    char foundStr[32];
    snprintf(foundStr, sizeof(foundStr), "Found: %d", deviceCount);
    drawClippedText(renderer, FONT_SMALL, sw / 2, 60, foundStr, sw / 2 - 10, tc);
  }

  // Device list
  int deviceCount = getDiscoveredDeviceCount();
  if (deviceCount > 0) {
    BleDeviceInfo* devices = getDiscoveredDevices();

    char headerStr[64];
    snprintf(headerStr, sizeof(headerStr), "Available devices: %d", deviceCount);
    drawClippedText(renderer, FONT_SMALL, 10, 70, headerStr, 0, tc, EpdFontFamily::BOLD);

    // Show up to 10 devices (pagination via scrolling)
    int maxDevicesToShow = 10;
    int startIndex = 0;
    if (bluetoothDeviceSelection >= maxDevicesToShow) {
      startIndex = bluetoothDeviceSelection - maxDevicesToShow + 1;
    }
    int devicesToShow = (deviceCount - startIndex < maxDevicesToShow)
                        ? deviceCount - startIndex : maxDevicesToShow;

    for (int i = 0; i < devicesToShow; i++) {
      int deviceIndex = startIndex + i;
      int yPos = 90 + (i * 30);

      // Stop drawing if we'd go into the footer zone
      if (yPos > sh - 100) break;

      bool isSelected = (bluetoothDeviceSelection == deviceIndex);
      bool isConnected = (getCurrentDeviceAddress() == devices[deviceIndex].address);

      const char* displayName = devices[deviceIndex].name.empty()
                                ? devices[deviceIndex].address.c_str()
                                : devices[deviceIndex].name.c_str();

      // Available width: leave room for RSSI on the right (~80px)
      int nameMaxW = sw - 100;

      if (isSelected || isConnected) {
        clippedFillRect(renderer, 5, yPos - 5, sw - 10, 25, tc);
        drawClippedText(renderer, FONT_UI, 15, yPos, displayName, nameMaxW, !tc);
      } else {
        drawClippedText(renderer, FONT_UI, 15, yPos, displayName, nameMaxW, tc);
      }

      // RSSI on the right
      char rssiStr[16];
      snprintf(rssiStr, sizeof(rssiStr), "%ddBm", devices[deviceIndex].rssi);
      drawRightText(renderer, FONT_SMALL, sw - 10, yPos, rssiStr, tc);
    }

    // Page indicator
    if (deviceCount > maxDevicesToShow) {
      char navHint[32];
      int pageNum = (bluetoothDeviceSelection / maxDevicesToShow) + 1;
      int totalPages = (deviceCount + maxDevicesToShow - 1) / maxDevicesToShow;
      snprintf(navHint, sizeof(navHint), "Page %d/%d", pageNum, totalPages);
      int navY = 90 + (devicesToShow * 30);
      if (navY < sh - 100)
        drawClippedText(renderer, FONT_SMALL, 15, navY, navHint, 0, tc);
    }
  } else {
    drawClippedText(renderer, FONT_UI, 20, 80, "No devices found", 0, tc);
    drawClippedText(renderer, FONT_SMALL, 20, 100, "Press Enter to scan for devices", 0, tc);
  }

  // Footer
  constexpr int bm = 60;
  if (sh > bm + 30) {
    clippedLine(renderer, 10, sh - bm, sw - 10, sh - bm, tc);
    drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 8,  "Enter:Connect  Right:Scan", 0, tc);
    drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 22, "Left:Disconnect  Esc:Back", 0, tc);
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawPairedKeyboardsMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  drawClippedText(renderer, FONT_SMALL, 10, 5, "Paired Keyboards", 0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  int count = getPairedKeyboardCount();
  if (count == 0) {
    drawClippedText(renderer, FONT_UI, 20, 60, "No paired keyboards", 0, tc);
    drawClippedText(renderer, FONT_SMALL, 20, 85, "Go to Bluetooth to scan and connect", 0, tc);
  } else {
    std::string currentAddr = getCurrentDeviceAddress();
    int lineH = 38;
    int listTop = 44;

    for (int i = 0; i < count; i++) {
      std::string addr, name; uint8_t addrType;
      getPairedKeyboard(i, addr, name, addrType);

      int yPos = listTop + (i * lineH);
      bool sel = (i == pairedKeyboardSelection);
      bool active = (!currentAddr.empty() && currentAddr == addr);

      if (sel) {
        clippedFillRect(renderer, 5, yPos - 4, sw - 10, lineH - 4, tc);
        drawClippedText(renderer, FONT_UI, 15, yPos, name.c_str(), sw - 90, !tc);
      } else {
        drawClippedText(renderer, FONT_UI, 15, yPos, name.c_str(), sw - 90, tc);
      }

      if (active) {
        drawRightText(renderer, FONT_SMALL, sw - 10, yPos + 2, "active", sel ? !tc : tc);
      } else if (!active && i == getLastUsedKeyboardIndex() && currentAddr.empty()) {
        drawRightText(renderer, FONT_SMALL, sw - 10, yPos + 2, "last", sel ? !tc : tc);
      }
    }
  }

  constexpr int bm = 52;
  if (sh > bm + 30) {
    clippedLine(renderer, 10, sh - bm, sw - 10, sh - bm, tc);
    drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 8,  "Enter:Connect  D:Forget", 0, tc);
    drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 22, "Left:Disconnect  Esc:Back", 0, tc);
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

// Helper: draw signal strength indicator (1-4 bars)
static void drawSignalBars(GfxRenderer& r, int x, int y, int rssi, bool color) {
  // RSSI to bars: > -50 = 4, > -65 = 3, > -75 = 2, else 1
  int bars = (rssi > -50) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : 1;
  for (int i = 0; i < 4; i++) {
    int bh = 4 + i * 3;  // bar heights: 4, 7, 10, 13
    int by = y + 13 - bh;
    if (i < bars) {
      clippedFillRect(r, x + i * 5, by, 3, bh, color);
    } else {
      clippedFillRect(r, x + i * 5, by + bh - 2, 3, 2, color);
    }
  }
}

void drawSyncScreen(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  // Header
  drawClippedText(renderer, FONT_SMALL, 10, 5, "Sync", 0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  SyncState state = getSyncState();

  switch (state) {
    case SyncState::SCANNING: {
      drawClippedText(renderer, FONT_UI, 20, 80, "Scanning for networks...", sw - 40, tc);
      break;
    }

    case SyncState::NETWORK_LIST: {
      int nc = getNetworkCount();
      int sel = getSelectedNetwork();

      if (nc == 0) {
        const char* st = getSyncStatusText();
        drawClippedText(renderer, FONT_UI, 20, 60, st[0] ? st : "No networks found", sw - 40, tc);
        drawClippedText(renderer, FONT_SMALL, 20, 90, "Enter: Rescan  Esc: Back", 0, tc);
      } else {
        drawClippedText(renderer, FONT_SMALL, 10, 38, "Select network:", 0, tc);

        int lineH = 28;
        int listTop = 56;
        int footerH = 28;
        int maxVisible = (sh - listTop - footerH) / lineH;
        int startIdx = 0;
        if (nc > maxVisible && sel >= maxVisible) {
          startIdx = sel - maxVisible + 1;
        }

        for (int i = startIdx; i < nc && (i - startIdx) < maxVisible; i++) {
          int yPos = listTop + (i - startIdx) * lineH;
          bool isSel = (i == sel);

          // Build display string: signal indicator + lock + saved + SSID
          char label[48];
          snprintf(label, sizeof(label), "%s%s%s",
                   isNetworkEncrypted(i) ? "* " : "  ",
                   isNetworkSaved(i) ? "+ " : "",
                   getNetworkSSID(i));

          if (isSel) {
            clippedFillRect(renderer, 5, yPos - 3, sw - 10, lineH - 2, tc);
            drawClippedText(renderer, FONT_UI, 15, yPos, label, sw - 50, !tc);
            drawSignalBars(renderer, sw - 30, yPos, getNetworkRSSI(i), !tc);
          } else {
            drawClippedText(renderer, FONT_UI, 15, yPos, label, sw - 50, tc);
            drawSignalBars(renderer, sw - 30, yPos, getNetworkRSSI(i), tc);
          }
        }
      }

      // Footer
      constexpr int bm = 28;
      clippedLine(renderer, 10, sh - bm - 2, sw - 10, sh - bm - 2, tc);
      drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 4,
                      "*=encrypted +=saved  Enter:Select  Esc:Back", 0, tc);
      break;
    }

    case SyncState::PASSWORD_ENTRY: {
      int sel = getSelectedNetwork();
      char heading[48];
      snprintf(heading, sizeof(heading), "Password for %s", getNetworkSSID(sel));
      drawClippedText(renderer, FONT_SMALL, 20, 42, heading, sw - 40, tc);

      // Password field box
      renderer.drawRect(15, 62, sw - 30, 30, tc);

      // Show dots for password characters (privacy)
      int pLen = getPasswordLen();
      char dots[64];
      for (int i = 0; i < pLen; i++) dots[i] = '*';
      dots[pLen] = '\0';
      drawClippedText(renderer, FONT_UI, 20, 66, dots, sw - 50, tc);

      // Cursor
      int cursorX = 20 + renderer.getTextAdvanceX(FONT_UI, dots);
      int cursorW = renderer.getSpaceWidth(FONT_UI);
      if (cursorW < 2) cursorW = 8;
      if (cursorX + cursorW < sw)
        renderer.fillRect(cursorX, 66, cursorW, 20, tc);

      drawClippedText(renderer, FONT_SMALL, 20, 110, "Enter: Connect   Esc: Cancel", 0, tc);
      break;
    }

    case SyncState::CONNECTING: {
      const char* st = getSyncStatusText();
      drawClippedText(renderer, FONT_UI, 20, 80, st, sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 110, "Esc: Cancel", 0, tc);
      break;
    }

    case SyncState::SYNCING: {
      const char* ip = getSyncStatusText();
      drawClippedText(renderer, FONT_SMALL, 20, 42, ip, sw - 40, tc, EpdFontFamily::BOLD);

      int sent    = getSyncFilesSent();
      bool pcConn = isPcConnected();

      // Build the stage list. Static so string pointers remain valid after this scope.
      static char fileStageText[MAX_FILES][16];
      static struct { const char* text; bool done; } stages[MAX_FILES + 3];
      int numStages = 0;

      auto push = [&](const char* text, bool done) {
        if (numStages < MAX_FILES + 3) stages[numStages++] = { text, done };
      };

      push("Connected to WiFi", true);
      push("PC connected",      pcConn);

      if (pcConn) {
        for (int i = 0; i < sent && i < MAX_FILES; i++) {
          snprintf(fileStageText[i], sizeof(fileStageText[i]), "File %d sent", i + 1);
          push(fileStageText[i], true);
        }
        push("Sync complete", false);
      }

      // Display with auto-scroll: keep last [x] + next [-] in view
      constexpr int lineH   = 22;
      constexpr int listTop = 62;
      constexpr int footerH = 32;
      int maxVisible = (sh - listTop - footerH) / lineH;

      int lastDone = 0;
      for (int i = 0; i < numStages; i++) {
        if (stages[i].done) lastDone = i;
      }
      int startIdx = 0;
      if (numStages > maxVisible) {
        startIdx = lastDone - maxVisible + 2;
        if (startIdx < 0) startIdx = 0;
        if (startIdx + maxVisible > numStages) startIdx = numStages - maxVisible;
      }

      for (int i = startIdx; i < numStages && (i - startIdx) < maxVisible; i++) {
        int yPos = listTop + (i - startIdx) * lineH;
        char line[52];
        snprintf(line, sizeof(line), "%s %s",
                 stages[i].done ? "[x]" : "[-]", stages[i].text);
        drawClippedText(renderer, FONT_SMALL, 15, yPos, line, sw - 25, tc);
      }

      // Footer
      constexpr int bm = 28;
      clippedLine(renderer, 10, sh - bm - 2, sw - 10, sh - bm - 2, tc);
      char countStr[32];
      snprintf(countStr, sizeof(countStr), "Sent: %d   Esc: Cancel", sent);
      drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 4, countStr, sw - 20, tc);
      break;
    }

    case SyncState::DONE: {
      const char* summary = getSyncStatusText();
      drawClippedText(renderer, FONT_SMALL, 20, 50, "Sync Complete", 0, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, FONT_UI, 20, 85, summary, sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 125, "Returning to menu...", 0, tc);
      break;
    }

    case SyncState::CONNECT_FAILED: {
      drawClippedText(renderer, FONT_UI, 20, 80, "Connection failed", sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 110, "Enter: Retry   Esc: Back", 0, tc);
      break;
    }

    case SyncState::SAVE_PROMPT: {
      const char* ip = getSyncStatusText();
      drawClippedText(renderer, FONT_SMALL, 20, 50, "Connected!", 0, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, FONT_UI, 20, 80, ip, sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 120, "Save password?", 0, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, FONT_SMALL, 20, 145, "Enter/Up: Yes   Down/Esc: No", 0, tc);
      break;
    }

    case SyncState::FORGET_PROMPT: {
      drawClippedText(renderer, FONT_UI, 20, 80, "Saved password failed", sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 120, "Forget saved password?", 0, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, FONT_SMALL, 20, 145, "Enter/Up: Yes   Down/Esc: No", 0, tc);
      break;
    }
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

