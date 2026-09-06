#include "pomodoro.h"

#include <Arduino.h>

namespace pomodoro {
namespace {

constexpr uint32_t kDefaultSeconds = 25u * 60u;
constexpr uint32_t kMaxSeconds = 90u * 60u;
constexpr uint32_t kMinSeconds = 60u;

State g_state = State::Idle;
uint32_t g_totalSeconds = kDefaultSeconds;
uint32_t g_remainingSeconds = kDefaultSeconds;
uint32_t g_lastTickMs = 0;
uint8_t g_completedToday = 0;
bool g_secondTicked = false;
uint32_t g_completedDayStamp = 0;  // days() marker to reset the counter
time_t g_deadlineEpoch = 0;        // wall-clock finish line (survives sleep)

}  // namespace

void reset() {
  g_state = State::Idle;
  g_totalSeconds = kDefaultSeconds;
  g_remainingSeconds = kDefaultSeconds;
  g_lastTickMs = 0;
}

void toggle() {
  switch (g_state) {
    case State::Idle:
    case State::Paused:
      g_state = State::Running;
      g_lastTickMs = millis();
      g_deadlineEpoch = time(nullptr) + static_cast<time_t>(g_remainingSeconds);
      break;
    case State::Running:
      g_state = State::Paused;
      break;
    case State::Finished:
      reset();
      break;
  }
}

void addMinute() {
  if (g_totalSeconds + 60u > kMaxSeconds) return;
  g_totalSeconds += 60u;
  if (g_state == State::Finished) {
    g_state = State::Idle;
    g_remainingSeconds = g_totalSeconds;
  } else if (g_remainingSeconds + 60u <= g_totalSeconds) {
    g_remainingSeconds += 60u;
  }
}

void subMinute() {
  if (g_totalSeconds <= kMinSeconds) return;
  g_totalSeconds -= 60u;
  if (g_remainingSeconds > 60u) {
    g_remainingSeconds -= 60u;
  } else {
    g_remainingSeconds = g_totalSeconds;
    g_state = State::Idle;
  }
}

void tick() {
  if (g_state != State::Running) return;
  // Prefer wall-clock: tolerates millis() rollover, page redraws that stall
  // the loop, and any future deep-sleep gaps.
  const time_t now = time(nullptr);
  if (now > 1700000000 && g_deadlineEpoch > 1700000000) {
    if (now < g_deadlineEpoch) {
      const uint32_t rem = static_cast<uint32_t>(g_deadlineEpoch - now);
      if (rem < g_remainingSeconds) {
        g_remainingSeconds = rem;
        g_secondTicked = true;
      }
      return;
    }
    g_remainingSeconds = 0;
  }

  const uint32_t millisNow = millis();
  uint32_t elapsed;
  if (millisNow >= g_lastTickMs) {
    elapsed = millisNow - g_lastTickMs;
  } else {
    elapsed = (0xFFFFFFFFul - g_lastTickMs) + millisNow;
  }
  g_lastTickMs = millisNow;
  const uint32_t step = elapsed / 1000u;
  if (g_remainingSeconds > step) {
    g_remainingSeconds -= step;
    g_secondTicked = true;
    return;
  }
  // Session finished
  g_remainingSeconds = 0;
  g_state = State::Finished;
  ++g_completedToday;
  g_secondTicked = true;
}

bool secondTicked() {
  const bool v = g_secondTicked;
  g_secondTicked = false;
  return v;
}

time_t deadlineEpoch() { return g_deadlineEpoch; }
void setDeadlineEpoch(time_t t) { g_deadlineEpoch = t; }

void restoreRunning(uint32_t totalSeconds, uint32_t remaining, uint8_t completedToday) {
  g_totalSeconds = totalSeconds;
  g_remainingSeconds = remaining;
  g_completedToday = completedToday;
  g_state = State::Running;
  g_lastTickMs = millis();
  g_deadlineEpoch = time(nullptr) + static_cast<time_t>(remaining);
}

void restoreFinished(uint32_t totalSeconds, uint8_t completedToday) {
  g_totalSeconds = totalSeconds;
  g_remainingSeconds = 0;
  g_completedToday = completedToday;
  g_state = State::Finished;
}

void restoreIdle(uint32_t totalSeconds) {
  g_totalSeconds = totalSeconds;
  g_remainingSeconds = totalSeconds;
  g_state = State::Idle;
}

void setCompletedToday(uint8_t n) { g_completedToday = n; }
void setDayStamp(uint32_t epochDay) { g_completedDayStamp = epochDay; }

State state() { return g_state; }
uint32_t remainingSeconds() { return g_remainingSeconds; }
uint32_t totalSeconds() { return g_totalSeconds; }
uint8_t completedToday() { return g_completedToday; }

}  // namespace pomodoro
