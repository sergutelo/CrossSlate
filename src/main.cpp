#include <Arduino.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <GfxRenderer.h>
#include <esp_pm.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <Preferences.h>
#include "sd_backup.h"

#include "config.h"
#include "ble_keyboard.h"
#include "input_handler.h"
#include "text_editor.h"
#include "file_manager.h"
#include "ui_renderer.h"
#include "pomodoro.h"
#include "wifi_sync.h"
#include "dashboard.h"
#include "ota_boot_switch.h"

// Enum for sleep reasons
enum class SleepReason {
  POWER_LONGPRESS,
  IDLE_TIMEOUT,
  MENU_ACTION
};

// Forward declarations
void renderSleepScreen();
void enterDeepSleep(SleepReason reason);
bool renderSleepWallpaper(int sw, int sh);

// External variables
extern bool autoReconnectEnabled;

// --- Hardware objects ---
HalDisplay display;
GfxRenderer renderer(display);
HalGPIO gpio;


// --- Persistent settings (NVS) ---
static Preferences uiPrefs;

// --- Shared UI state ---
UIState currentState = UIState::DASHBOARD;
int mainMenuSelection = 0;
int selectedFileIndex = 0;
int settingsSelection = 0;
int bluetoothDeviceSelection = 0;
int pairedKeyboardSelection = 0;
Orientation currentOrientation = Orientation::PORTRAIT;
int charsPerLine = 40;
bool screenDirty = true;

// Rename buffer
char renameBuffer[MAX_FILENAME_LEN] = "";
int renameBufferLen = 0;

// UI mode flags
bool darkMode = false;
bool cleanMode = false;
bool deleteConfirmPending = false;
WritingMode writingMode = WritingMode::NORMAL;
FontSize fontSize = FontSize::LARGE;
bool showWordCount = true;

// --- Screen update ---
static void updateScreen() {
  if (!screenDirty) return;
  screenDirty = false;

  // Apply orientation
  static Orientation lastOrientation = Orientation::PORTRAIT;
  if (currentOrientation != lastOrientation) {
    GfxRenderer::Orientation gfxOrient = GfxRenderer::Portrait;
    switch (currentOrientation) {
      case Orientation::PORTRAIT:      gfxOrient = GfxRenderer::Portrait; break;
      case Orientation::LANDSCAPE_CW:  gfxOrient = GfxRenderer::LandscapeClockwise; break;
      case Orientation::PORTRAIT_INV:  gfxOrient = GfxRenderer::PortraitInverted; break;
      case Orientation::LANDSCAPE_CCW: gfxOrient = GfxRenderer::LandscapeCounterClockwise; break;
    }
    renderer.setOrientation(gfxOrient);
    lastOrientation = currentOrientation;
  }

  // Auto-compute chars per line from font metrics so text always fills the screen
  {
    int sw = renderer.getScreenWidth();
    int textAreaWidth = sw - 20;  // 10px margins each side
    int avgCharW = renderer.getTextAdvanceX(editorFontId(fontSize), "abcdefghijklmnopqrstuvwxyz") / 26;
    if (avgCharW > 0) charsPerLine = textAreaWidth / avgCharW;
  }
  editorSetCharsPerLine(charsPerLine);

  switch (currentState) {
    case UIState::DASHBOARD:         drawDashboard(renderer, gpio); break;
    case UIState::MAIN_MENU:         drawMainMenu(renderer, gpio); break;
    case UIState::READING_PAGE:      drawReadingPage(renderer, gpio); break;
    case UIState::FILE_BROWSER:      drawFileBrowser(renderer, gpio); break;
    case UIState::TEXT_EDITOR:       drawTextEditor(renderer, gpio); break;
    case UIState::RENAME_FILE:       drawRenameScreen(renderer, gpio); break;
    case UIState::SETTINGS:          drawSettingsMenu(renderer, gpio); break;
    case UIState::CITY_SELECTION:    drawCitiesMenu(renderer, gpio); break;
    case UIState::BLUETOOTH_SETTINGS: drawBluetoothSettings(renderer, gpio); break;
    case UIState::PAIRED_KEYBOARDS:   drawPairedKeyboardsMenu(renderer, gpio); break;
    case UIState::WIFI_SYNC:          drawSyncScreen(renderer, gpio); break;
    default: break;
  }
}

void setup() {
  DBG_INIT();
  DBG_PRINTLN("CrossSlate starting...");
  // Confirm a manually selected OTA image before any deep-sleep-capable work.
  otaBootConfirmRunningImage();

  setCpuFrequencyMhz(80);

  gpio.begin();
  display.begin();

  renderer.setFadingFix(true);  // Power down display analog circuits after each refresh — reduces idle drain
  rendererSetup(renderer);

  // Load persisted UI settings from NVS early so startup screen uses saved orientation
  uiPrefs.begin("ui_prefs", false);
  currentOrientation = static_cast<Orientation>(uiPrefs.getUChar("orient", 0));
  darkMode = uiPrefs.getBool("darkMode", false);
  // Pomodoro survives deep sleep: deadline persisted as wall-clock epoch.
  {
    const uint8_t doneSaved = uiPrefs.getUChar("pomDone", 0);
    const uint32_t daySaved = uiPrefs.getULong("pomDay", 0);
    const uint32_t total = uiPrefs.getULong("pomTotal", 25u * 60u);
    const time_t now = time(nullptr);
    const uint32_t today = (now > 1700000000) ? static_cast<uint32_t>(now / 86400u) : daySaved;
    const uint8_t done = (daySaved != 0 && today != daySaved) ? 0 : doneSaved;  // new day resets "Hoy"

    if (uiPrefs.getBool("pomActive", false)) {
      const time_t deadline = static_cast<time_t>(uiPrefs.getULong("pomDeadline", 0));
      const uint32_t remainSaved = uiPrefs.getULong("pomRemain", total);
      const bool clockOk = deadline > 1700000000 && now > 1700000000;
      if (!clockOk) {
        // RTC not synced yet (fresh wake): resume with the seconds saved at
        // sleep time. tick() re-bases the deadline once NTP lands.
        pomodoro::restoreRunning(total, remainSaved ? remainSaved : total, done);
      } else if (deadline > 1700000000 && now < deadline) {
        const uint32_t rem = static_cast<uint32_t>(deadline - now);
        pomodoro::restoreRunning(total, rem, done);
      } else if (deadline > 1700000000) {
        pomodoro::restoreFinished(total, done);
      } else {
        pomodoro::restoreRunning(total, remainSaved ? remainSaved : total, done);
      }
    } else {
      pomodoro::restoreIdle(uiPrefs.getULong("pomTotal", total));
      // restoreIdle zeroes completedToday internally; re-apply the saved count
    }
    pomodoro::setCompletedToday(done);
    pomodoro::setDayStamp(today);
  }
  // Return to the page active before sleep (only for the stable pages)
  {
    const uint8_t page = uiPrefs.getUChar("lastPage", static_cast<uint8_t>(UIState::DASHBOARD));
    if (page == static_cast<uint8_t>(UIState::READING_PAGE) || page == static_cast<uint8_t>(UIState::DASHBOARD)) {
      currentState = static_cast<UIState>(page);
    }
  }
  writingMode = static_cast<WritingMode>(uiPrefs.getUChar("writeMode", 0));
  fontSize = static_cast<FontSize>(uiPrefs.getUChar("fontSize", 2));
  showWordCount = uiPrefs.getBool("showWC", true);

  // Apply saved orientation
  {
    GfxRenderer::Orientation gfxOrient = GfxRenderer::Portrait;
    switch (currentOrientation) {
      case Orientation::PORTRAIT:      gfxOrient = GfxRenderer::Portrait; break;
      case Orientation::LANDSCAPE_CW:  gfxOrient = GfxRenderer::LandscapeClockwise; break;
      case Orientation::PORTRAIT_INV:  gfxOrient = GfxRenderer::PortraitInverted; break;
      case Orientation::LANDSCAPE_CCW: gfxOrient = GfxRenderer::LandscapeCounterClockwise; break;
    }
    renderer.setOrientation(gfxOrient);
  }

  editorInit();
  inputSetup();
  fileManagerSetup();
  dashboardSetup();

  // Restore UI prefs from SD backup if NVS was wiped by a firmware flash
  if (!uiPrefs.isKey("orient")) {
    static char uiBuf[128];
    if (sdReadFile("/microslate/ui_prefs.json", uiBuf, sizeof(uiBuf))) {
      int o  = jsonGetInt(uiBuf, "orient");
      int d  = jsonGetInt(uiBuf, "dark");
      int wm = jsonGetInt(uiBuf, "writeMode");
      int fs = jsonGetInt(uiBuf, "fontSize");
      int wc = jsonGetInt(uiBuf, "showWC");
      if (o  >= 0) { uiPrefs.putUChar("orient",    (uint8_t)o);  currentOrientation = static_cast<Orientation>(o); }
      if (d  >= 0) { uiPrefs.putBool("darkMode",   d != 0);      darkMode           = (d != 0); }
      if (wm >= 0) { uiPrefs.putUChar("writeMode", (uint8_t)wm); writingMode        = static_cast<WritingMode>(wm); }
      if (fs >= 0) { uiPrefs.putUChar("fontSize",  (uint8_t)fs); fontSize           = static_cast<FontSize>(fs); }
      if (wc >= 0) { uiPrefs.putBool("showWC",     wc != 0);     showWordCount      = (wc != 0); }
      // Re-apply orientation in case it changed
      GfxRenderer::Orientation gfxOrient = GfxRenderer::Portrait;
      switch (currentOrientation) {
        case Orientation::PORTRAIT:      gfxOrient = GfxRenderer::Portrait; break;
        case Orientation::LANDSCAPE_CW:  gfxOrient = GfxRenderer::LandscapeClockwise; break;
        case Orientation::PORTRAIT_INV:  gfxOrient = GfxRenderer::PortraitInverted; break;
        case Orientation::LANDSCAPE_CCW: gfxOrient = GfxRenderer::LandscapeCounterClockwise; break;
      }
      renderer.setOrientation(gfxOrient);
      DBG_PRINTLN("UI prefs restored from SD backup");
    }
  }

  bleSetup();

  // Best-effort bounded weather refresh on CrossSlate start; failures preserve SD cache.
  dashboardRefreshWeather();

  // Enable automatic light sleep between loop iterations.
  // CONFIG_PM_ENABLE and CONFIG_FREERTOS_USE_TICKLESS_IDLE are compiled into
  // ESP-IDF via sdkconfig.defaults (framework = arduino, espidf). BLE modem
  // sleep keeps the radio alive across sleep/wake cycles.
  esp_pm_config_esp32c3_t pm_config = {
    .max_freq_mhz = 80,
    .min_freq_mhz = 10,
    .light_sleep_enable = true
  };
  esp_err_t pm_err = esp_pm_configure(&pm_config);
  DBG_PRINTF("PM configure: %s\n", esp_err_to_name(pm_err));

  // Initialize auto-reconnect to enabled by default
  autoReconnectEnabled = true;

  // Only expose the reciprocal boot action when the alternate image is valid.
  otaBootFindCrossInk();

  DBG_PRINTLN("CrossSlate ready.");

  // The display needs one FULL_REFRESH after power-on to initialize its analog
  // circuits before FAST_REFRESH will work.
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);

  screenDirty = true;
}

// Enter deep sleep - matches crosspoint pattern
void enterDeepSleep(SleepReason reason) {
  DBG_PRINTLN("Entering deep sleep...");
  
  // Remember the page we were on so wake returns here
  uiPrefs.putUChar("lastPage", static_cast<uint8_t>(currentState));

  // Persist the pomodoro so it survives deep sleep
  {
    uiPrefs.putBool("pomActive", pomodoro::state() == pomodoro::State::Running);
    uiPrefs.putULong("pomDeadline", static_cast<unsigned long>(pomodoro::deadlineEpoch()));
    uiPrefs.putULong("pomTotal", pomodoro::totalSeconds());
    uiPrefs.putUChar("pomDone", pomodoro::completedToday());
    const time_t nowSave = time(nullptr);
    uiPrefs.putULong("pomDay", (nowSave > 1700000000) ? static_cast<unsigned long>(nowSave / 86400u) : 0ul);
    uiPrefs.putBool("pomPaused", pomodoro::state() == pomodoro::State::Paused);
    uiPrefs.putULong("pomRemain", pomodoro::remainingSeconds());
  }

  // Render the sleep screen before entering deep sleep
  renderSleepScreen();

  // Save any unsaved work
  if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
    saveCurrentFile();
  }

  display.deepSleep();     // Power down display first
  gpio.startDeepSleep();   // Waits for power button release, then sleeps
  // Will not return - device is asleep
}

// Translate physical button presses to HID key codes
// NOTE: gpio.update() is called in loop() before this function
static void processPhysicalButtons() {
  static bool btnUpLast = false;
  static bool btnDownLast = false;
  static bool btnLeftLast = false;
  static bool btnRightLast = false;
  static bool btnConfirmLast = false;
  static bool btnBackLast = false;
  // Pomodoro reset: hold bottom-right 2s while on the Utilidades page
  static bool pomResetHeld = false;
  static unsigned long pomResetStart = 0;

  // Use isPressed() — persistent debounced state.  With one-shot scanning
  // (radio quiet during navigation), InputManager debounce works reliably.
  bool btnUp      = gpio.isPressed(HalGPIO::BTN_UP);
  bool btnDown    = gpio.isPressed(HalGPIO::BTN_DOWN);
  bool btnLeft    = gpio.isPressed(HalGPIO::BTN_LEFT);
  bool btnRight   = gpio.isPressed(HalGPIO::BTN_RIGHT);
  bool btnConfirm = gpio.isPressed(HalGPIO::BTN_CONFIRM);
  bool btnBack    = gpio.isPressed(HalGPIO::BTN_BACK);

  // Power button state machine for proper long/short press handling
  static bool powerHeld = false;
  static unsigned long powerPressStart = 0;
  static bool sleepTriggered = false;

  bool btnPower = gpio.isPressed(HalGPIO::BTN_POWER);

  if (btnPower && !powerHeld) {
    // Button just pressed
    powerHeld = true;
    sleepTriggered = false;
    powerPressStart = millis();
  }

  if (btnPower && powerHeld && !sleepTriggered) {
    if (millis() - powerPressStart > 3000) {
      sleepTriggered = true;
      enterDeepSleep(SleepReason::POWER_LONGPRESS);
      return; // Exit early to prevent further processing
    }
  }

  if (!btnPower && powerHeld) {
    // Button released
    unsigned long duration = millis() - powerPressStart;
    powerHeld = false;

    if (!sleepTriggered && duration > 50 && duration < 1000) {
      // Short press - go to main menu (except when already there)
      if (currentState != UIState::MAIN_MENU) {
        if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
          saveCurrentFile();
        }
        currentState = UIState::MAIN_MENU;
        screenDirty = true;
      }
    }
  }

  // Back button long-press for restart
  static bool backHeld = false;
  static unsigned long backPressStart = 0;
  static bool restartTriggered = false;

  if (btnBack && !backHeld) {
    backHeld = true;
    restartTriggered = false;
    backPressStart = millis();
  }

  if (btnBack && backHeld && !restartTriggered) {
    if (millis() - backPressStart > 5000) {
      restartTriggered = true;
      DBG_PRINTLN("BACK held for 5s — restarting device...");
      if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
        saveCurrentFile();
      }
      delay(100);
      ESP.restart();
    }
  }

  if (!btnBack && backHeld) {
    backHeld = false;
  }

  // Pomodoro reset: hold the bottom-right button 2s on the Utilidades page.
  // Long-press so a stray tap can't wipe a running session.
  if (currentState == UIState::READING_PAGE && btnRight && !pomResetHeld) {
    pomResetHeld = true;
    pomResetStart = millis();
  }
  if (pomResetHeld && millis() - pomResetStart > 2000) {
    pomResetHeld = false;
    pomodoro::reset();
    screenDirty = true;
  }
  if (!btnRight && pomResetHeld) {
    pomResetHeld = false;
  }

  // Map physical buttons to HID key codes based on current UI state
  switch (currentState) {
    case UIState::DASHBOARD:
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      // Physical Right = refresh weather (matches the on-screen "Pulse R" hint).
      if (btnRight && !btnRightLast) {
        enqueueKeyEvent(HID_KEY_R, 0, true);
        enqueueKeyEvent(HID_KEY_R, 0, false);
      }
      break;

    case UIState::READING_PAGE:
      // Side buttons: Up/Down adjust the pomodoro duration; Confirm toggles
      // start/pause; Back (short) returns to the menu. Power short-press also
      // exits via the global handler above.
      if (btnUp && !btnUpLast) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (btnDown && !btnDownLast) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::MAIN_MENU:
      if ((btnUp && !btnUpLast) || (btnRight && !btnRightLast)) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if ((btnDown && !btnDownLast) || (btnLeft && !btnLeftLast)) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      break;

    case UIState::FILE_BROWSER:
      if (((btnUp && !btnUpLast) || (btnRight && !btnRightLast)) && getFileCount() > 0) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (((btnDown && !btnDownLast) || (btnLeft && !btnLeftLast)) && getFileCount() > 0) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast && getFileCount() > 0) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::TEXT_EDITOR: {
      // Key repeat state for held navigation/backspace keys
      static uint8_t repeatKey = 0;
      static unsigned long repeatStart = 0;
      static unsigned long lastRepeat = 0;
      const unsigned long REPEAT_DELAY = 400;
      const unsigned long REPEAT_RATE  = 80;

      auto fireKey = [](uint8_t k) {
        enqueueKeyEvent(k, 0, true);
        enqueueKeyEvent(k, 0, false);
      };

      // Map currently held button to HID key (0 = none)
      uint8_t heldKey = 0;
      if      (btnUp)    heldKey = HID_KEY_UP;
      else if (btnDown)  heldKey = HID_KEY_DOWN;
      else if (btnLeft)  heldKey = HID_KEY_LEFT;
      else if (btnRight) heldKey = HID_KEY_RIGHT;

      if (heldKey != repeatKey) {
        // Key changed — fire immediately on press
        if (heldKey != 0) fireKey(heldKey);
        repeatKey   = heldKey;
        repeatStart = millis();
        lastRepeat  = millis();
      } else if (heldKey != 0) {
        unsigned long now = millis();
        if (now - repeatStart > REPEAT_DELAY && now - lastRepeat > REPEAT_RATE) {
          fireKey(heldKey);
          lastRepeat = now;
        }
      }

      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        if (editorHasUnsavedChanges()) saveCurrentFile();
        currentState = UIState::FILE_BROWSER;
        screenDirty = true;
      }
      break;
    }

    case UIState::RENAME_FILE:
    case UIState::NEW_FILE:
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::CITY_SELECTION:
      if (btnUp && !btnUpLast) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (btnDown && !btnDownLast) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::BLUETOOTH_SETTINGS:
      if (btnUp && !btnUpLast) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (btnDown && !btnDownLast) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnRight && !btnRightLast) {
        enqueueKeyEvent(HID_KEY_RIGHT, 0, true);  // Scan
        enqueueKeyEvent(HID_KEY_RIGHT, 0, false);
      }
      if (btnLeft && !btnLeftLast) {
        enqueueKeyEvent(HID_KEY_LEFT, 0, true);   // Disconnect
        enqueueKeyEvent(HID_KEY_LEFT, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::PAIRED_KEYBOARDS:
      if ((btnUp && !btnUpLast) || (btnRight && !btnRightLast)) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if ((btnDown && !btnDownLast) || (btnLeft && !btnLeftLast)) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::WIFI_SYNC:
      if ((btnUp && !btnUpLast) || (btnRight && !btnRightLast)) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if ((btnDown && !btnDownLast) || (btnLeft && !btnLeftLast)) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::SETTINGS:
      if ((btnUp && !btnUpLast) || (btnRight && !btnRightLast)) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if ((btnDown && !btnDownLast) || (btnLeft && !btnLeftLast)) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    default:
      break;
  }

  // Update last state
  btnUpLast = btnUp;
  btnDownLast = btnDown;
  btnLeftLast = btnLeft;
  btnRightLast = btnRight;
  btnConfirmLast = btnConfirm;
  btnBackLast = btnBack;
}

// Global variable for activity tracking
static unsigned long lastActivityTime = 0;
const unsigned long IDLE_TIMEOUT = 5UL * 60UL * 1000UL; // 5 minutes

void registerActivity() {
  lastActivityTime = millis();
}

// Function to render the sleep screen
bool renderSleepWallpaper(int sw, int sh) {
  // Reservoir sampling over the same folders CrossInk uses for its own sleep
  // images, so both firmwares share one wallpaper collection with no copying.
  static const char* const kSleepDirs[] = {"/sleep", "/.sleep"};
  char chosen[128];
  chosen[0] = '\0';

  for (const char* dirPath : kSleepDirs) {
    auto dir = SdMan.open(dirPath);
    if (!dir || !dir.isDirectory()) continue;

    char name[64];
    uint16_t candidates = 0;
    // Reservoir sampling: each valid file has 1/n chance of being the keeper.
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) { file.close(); continue; }
      file.getName(name, sizeof(name));
      const size_t len = strlen(name);
      const bool isBmp = len > 4 &&
          (strcasecmp(name + len - 4, ".bmp") == 0);
      if (!isBmp || name[0] == '.') { file.close(); continue; }
      file.close();
      candidates++;
      if (random(candidates) == 0) {
        snprintf(chosen, sizeof(chosen), "%s/%s", dirPath, name);
      }
    }
    dir.close();
    if (chosen[0]) break;
  }

  if (!chosen[0]) return false;

  auto file = SdMan.open(chosen, O_RDONLY);
  if (!file) return false;

  Bitmap bitmap(file, /*dithering=*/true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    file.close();
    return false;
  }
  // Fit the image inside the screen, preserving aspect ratio.
  int iw = bitmap.getWidth(), ih = bitmap.getHeight();
  if (iw <= 0 || ih <= 0) { file.close(); return false; }
  int drawW = iw, drawH = ih;
  if (iw > sw || ih > sh) {
    // Scale down proportionally using integer math on the larger ratio.
    const uint32_t ratioW = ((uint32_t)iw * 1000) / sw;
    const uint32_t ratioH = ((uint32_t)ih * 1000) / sh;
    if (ratioW >= ratioH) { drawW = sw; drawH = (int)(((uint32_t)ih * 1000) / ratioW); }
    else { drawH = sh; drawW = (int)(((uint32_t)iw * 1000) / ratioH); }
  }
  const int x = (sw - drawW) / 2, y = (sh - drawH) / 2;
  renderer.drawBitmap(bitmap, x, y, drawW, drawH);
  file.close();  // only one SD reader may hold a file open

  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  delay(500);
  return true;
}

// E-ink waveforms are asynchronous: cutting panel power mid-refresh leaves
// exactly the noisy half-scanned state. Wait for BUSY to drop before sleeping.
static void waitDisplaySettled() {
  const uint32_t start = millis();
  while (renderer.isRefreshing() && millis() - start < 5000u) {
    renderer.pollRefresh();
    delay(5);
  }
}

void renderSleepScreen() {
  // The page being left may have been refreshed many times with fast waveforms
  // (the pomodoro countdown), which accumulates ghosting. A plain full refresh
  // does not fully reset those particles. Force one clean white flash first:
  // once per sleep, so the battery cost is negligible.
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  {
    const uint32_t start = millis();
    while (renderer.isRefreshing() && millis() - start < 5000u) {
      renderer.pollRefresh();
      delay(5);
    }
  }
  renderer.clearScreen();

  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  // Running pomodoro: the sleep screen becomes the countdown. The deadline is
  // wall-clock based, so the time shown is exact when the device wakes.
  if (pomodoro::state() == pomodoro::State::Running) {
    const time_t now = time(nullptr);
    uint32_t rem = pomodoro::remainingSeconds();
    if (now > 1700000000 && pomodoro::deadlineEpoch() > 1700000000) {
      const time_t left = pomodoro::deadlineEpoch() - now;
      if (left > 0) rem = static_cast<uint32_t>(left);
    }
    // Double rectangular frame with the countdown scaled 4x (each FONT_LARGE
    // glyph cell drawn as 4x4 blocks for a chunky, readable-at-a-glance clock).
    char buf[24];
    snprintf(buf, sizeof(buf), "%02u:%02u", static_cast<unsigned>(rem / 60u),
             static_cast<unsigned>(rem % 60u));

    const int frameW = 420, frameH = 300;
    const int fx = (sw - frameW) / 2, fy = (sh - frameH) / 2 - 20;
    renderer.drawRect(fx, fy, frameW, frameH, true);
    renderer.drawRect(fx + 10, fy + 10, frameW - 20, frameH - 20, true);

    const char* label = "Pomodoro";
    const int lw = renderer.getTextAdvanceX(FONT_UI, label);
    renderer.drawText(FONT_UI, (sw - lw) / 2, fy + 26, label, true);

    // Seven-segment digits drawn with fillRect: huge, centered on the frame's
    // vertical axis, and crisp at 1-bit (font-independent sizing).
    // Layout: HH:MM. Digit box 56x104, stroke 12px.
    auto drawDigit = [](GfxRenderer& r, int x, int y, int d) {
      const int dw = 56, dh = 104, t = 12;
      const bool A = d != 1 && d != 4;
      const bool B = d != 5 && d != 6;
      const bool C = d != 2;
      const bool D = d != 1 && d != 4 && d != 7;
      const bool E = d == 0 || d == 2 || d == 6 || d == 8;
      const bool F = d != 1 && d != 2 && d != 3 && d != 7;
      const bool G = d >= 2 && d != 7;
      auto segH = [&](int x0, int y0) { r.fillRect(x0, y0, dw - 2 * t + 4, t, true); };
      auto segV = [&](int x0, int y0) { r.fillRect(x0, y0, t, dh / 2 - 8, true); };
      if (A) segH(x + t - 2, y);
      if (G) segH(x + t - 2, y + dh / 2 - t / 2);
      if (D) segH(x + t - 2, y + dh - t);
      if (F) segV(x, y + t);
      if (E) segV(x, y + dh / 2 + t / 2);
      if (B) segV(x + dw - t, y + t);
      if (C) segV(x + dw - t, y + dh / 2 + t / 2);
    };

    const int dd = 104, dw = 48, gap = 14, colonW = 30;
    const int totalW = dw * 4 + gap * 4 + colonW;
    const int clockX = (sw - totalW) / 2;  // centered on frame axis
    const int clockY = fy + 56;

    const int h1 = (rem / 600) % 10, h2 = (rem / 60) % 10;
    const int m1 = (rem / 10) % 10, m2 = rem % 10;
    int dx = clockX;
    drawDigit(renderer, dx, clockY, h1); dx += dw + gap;
    drawDigit(renderer, dx, clockY, h2); dx += dw + gap;
    renderer.fillRect(dx + colonW / 2 - 6, clockY + 26, 12, 12, true);
    renderer.fillRect(dx + colonW / 2 - 6, clockY + dd - 38, 12, 12, true);
    dx += colonW + gap;
    drawDigit(renderer, dx, clockY, m1); dx += dw + gap;
    drawDigit(renderer, dx, clockY, m2);

    const char* footer = "Hold Power to wake";
    const int fw = renderer.getTextAdvanceX(FONT_SMALL, footer);
    renderer.drawText(FONT_SMALL, (sw - fw) / 2, fy + frameH - 34, footer, true);
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    waitDisplaySettled();
    return;
  }

  // CrossInk-compatible wallpaper: pick a random .bmp from the shared sleep
  // folders and render it full-screen. Falls back to the text screen when no
  // valid image exists so the device never sleeps on a blank display.
  if (renderSleepWallpaper(sw, sh)) {
    waitDisplaySettled();
    return;  // wallpaper already flushed the buffer
  }

  // Title: "CrossSlate"
  const char* title = "CrossSlate";
  int titleWidth = renderer.getTextAdvanceX(FONT_BODY, title);
  int titleX = (sw - titleWidth) / 2;
  int titleY = sh * 0.35; // 35% down the screen (moved up)
  renderer.drawText(FONT_BODY, titleX, titleY, title, true, EpdFontFamily::BOLD);
  
  // Subtitle: "Asleep"
  const char* subtitle = "Asleep";
  int subTitleWidth = renderer.getTextAdvanceX(FONT_UI, subtitle);
  int subTitleX = (sw - subTitleWidth) / 2;
  int subTitleY = sh * 0.48; // 48% down the screen (moved up)
  renderer.drawText(FONT_UI, subTitleX, subTitleY, subtitle, true);
  
  // Footer: "Hold Power to wake"
  const char* footer = "Hold Power to wake";
  int footerWidth = renderer.getTextAdvanceX(FONT_SMALL, footer);
  int footerX = (sw - footerWidth) / 2;
  int footerY = sh * 0.75; // 75% down the screen (moved up from bottom)
  renderer.drawText(FONT_SMALL, footerX, footerY, footer);
  
  // Perform a full display refresh to ensure the sleep screen is visible
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  waitDisplaySettled();

  // Small delay to ensure the display update is complete
  delay(200);
}

void loop() {
  // --- GPIO first: always poll buttons before anything else ---
  gpio.update();
  pomodoro::tick();
  // Live countdown: redraw once per second while the timer runs.
  if (currentState == UIState::READING_PAGE && pomodoro::secondTicked()) {
    screenDirty = true;
  }
  // Deep clean every 5 min of continuous countdown: fast refreshes accumulate
  // ghosting; one full waveform per 300 clears it. Imperceptible cost.
  static uint32_t lastDeepCleanMs = 0;
  if (currentState == UIState::READING_PAGE && pomodoro::state() == pomodoro::State::Running) {
    if (lastDeepCleanMs == 0) lastDeepCleanMs = millis();
    if (millis() - lastDeepCleanMs > 300000ul) {
      lastDeepCleanMs = millis();
      renderer.clearScreen();
      renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    }
  } else {
    lastDeepCleanMs = 0;
  }

  // Control auto-reconnect based on UI state
  static UIState lastState = UIState::DASHBOARD;
  if (isWifiSyncActive()) {
    autoReconnectEnabled = false;
    if (isDeviceScanning()) stopDeviceScan();
  } else if (currentState == UIState::BLUETOOTH_SETTINGS) {
    autoReconnectEnabled = false;
    // On first entry to BT settings, do a one-shot scan
    if (lastState != UIState::BLUETOOTH_SETTINGS) {
      cancelPendingConnection();
      startDeviceScan();  // One-shot 5s scan, radio goes quiet after
    }
  } else {
    autoReconnectEnabled = true;
    if (lastState == UIState::BLUETOOTH_SETTINGS && isDeviceScanning()) {
      stopDeviceScan();
    }
  }
  lastState = currentState;

  // Process BLE (connection handling, scan completion detection)
  bleLoop();

  // Process WiFi sync HTTP clients when active
  if (isWifiSyncActive()) wifiSyncLoop();

  // CRITICAL: Process buttons BEFORE checking wasAnyPressed() to avoid consuming button states
  processPhysicalButtons();
  int inputEventsProcessed = processAllInput(); // Assuming this returns number of events processed

  // Register activity AFTER button processing (don't consume button states prematurely)
  static unsigned long lastInputTime = 0;
  bool hadActivity = gpio.wasAnyPressed() || inputEventsProcessed > 0;
  if (hadActivity) {
    registerActivity();
    lastInputTime = millis();
  }

  // Auto-save: hybrid idle + hard cap for crash protection.
  // - Saves after 10s of no keystrokes (catches natural pauses between sentences)
  // - Hard cap every 2min during continuous typing (never lose more than 2min of work)
  static unsigned long lastAutoSaveMs = 0;
  if (currentState == UIState::TEXT_EDITOR
      && editorHasUnsavedChanges()
      && editorGetCurrentFile()[0] != '\0') {
    unsigned long now = millis();
    bool idleTrigger = (now - lastInputTime) > AUTO_SAVE_IDLE_MS
                    && (now - lastAutoSaveMs) > AUTO_SAVE_IDLE_MS;
    bool capTrigger  = (now - lastAutoSaveMs) > AUTO_SAVE_MAX_MS;
    if (idleTrigger || capTrigger) {
      lastAutoSaveMs = now;
      saveCurrentFile(false);  // Skip refreshFileList — file list unchanged by content update
    }
  }

  // Periodically refresh sync screen to show status changes (every 2s)
  if (currentState == UIState::WIFI_SYNC) {
    static unsigned long lastSyncRefresh = 0;
    if (millis() - lastSyncRefresh > 2000) {
      screenDirty = true;
      lastSyncRefresh = millis();
    }
  }

  // Poll display refresh — non-blocking check of BUSY pin
  if (renderer.isRefreshing()) {
    renderer.pollRefresh();
  }

  // Don't start a new screen update while display is still refreshing
  if (screenDirty && !renderer.isRefreshing()) {
    updateScreen();
  }

  // Persist UI settings to NVS when they change (NVS write only on change, not every loop)
  static Orientation lastSavedOrientation = currentOrientation;
  static bool lastSavedDarkMode = darkMode;
  static WritingMode lastSavedWritingMode = writingMode;
  static FontSize lastSavedFontSize = fontSize;
  static bool lastSavedShowWordCount = showWordCount;
  if (currentOrientation != lastSavedOrientation || darkMode != lastSavedDarkMode
      || writingMode != lastSavedWritingMode || fontSize != lastSavedFontSize
      || showWordCount != lastSavedShowWordCount) {
    uiPrefs.putUChar("orient", static_cast<uint8_t>(currentOrientation));
    uiPrefs.putBool("darkMode", darkMode);
    uiPrefs.putUChar("writeMode", static_cast<uint8_t>(writingMode));
    uiPrefs.putUChar("fontSize", static_cast<uint8_t>(fontSize));
    uiPrefs.putBool("showWC", showWordCount);
    lastSavedOrientation = currentOrientation;
    lastSavedDarkMode = darkMode;
    lastSavedWritingMode = writingMode;
    lastSavedFontSize = fontSize;
    lastSavedShowWordCount = showWordCount;
    // Keep SD backup in sync so settings survive a firmware flash
    static char uiBuf[128];
    snprintf(uiBuf, sizeof(uiBuf),
             "{\"orient\":%d,\"dark\":%d,\"writeMode\":%d,\"fontSize\":%d,\"showWC\":%d}",
             (int)currentOrientation, darkMode ? 1 : 0,
             (int)writingMode, (int)fontSize, showWordCount ? 1 : 0);
    if (!SdMan.exists("/microslate")) SdMan.mkdir("/microslate");
    sdWriteFile("/microslate/ui_prefs.json", uiBuf);
  }

  // Check for idle timeout (skip while WiFi sync is active)
  if (!isWifiSyncActive() && millis() - lastActivityTime > IDLE_TIMEOUT) {
    enterDeepSleep(SleepReason::IDLE_TIMEOUT);
  }

  // Adaptive delay with recently-active window for button responsiveness.
  // BLE keystrokes wake from light sleep via modem interrupt (delay value irrelevant).
  // Physical buttons are polled, so the idle delay must be short enough to catch a
  // quick tap (~80-150ms). 50ms idle guarantees 1-2 samples per press.
  // Stay at fast polling for 2s after any activity for snappy consecutive presses.
  static constexpr unsigned long ACTIVE_WINDOW_MS = 2000;
  bool recentlyActive = (millis() - lastInputTime) < ACTIVE_WINDOW_MS;
  delay((hadActivity || screenDirty || recentlyActive) ? 10 : 50);
}
