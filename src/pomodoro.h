#pragma once

#include <cstdint>
#include <ctime>

// Simple countdown timer drawn on the reading page. Timekeeping uses
// millis() deltas so it survives rendering; no timers/ISRs.
namespace pomodoro {

enum class State : uint8_t { Idle, Running, Paused, Finished };

void reset();               // back to Idle, full duration
void toggle();              // Idle->Running, Running<->Paused
void addMinute();           // +1 minute (max 90), stops Finished
void subMinute();           // -1 minute (min 1)
void tick();                // call every loop(); advances countdown
bool secondTicked();        // true once per new second while Running (auto-clears)
State state();
uint32_t remainingSeconds();
uint32_t totalSeconds();
uint8_t completedToday();   // finished sessions since midnight
time_t deadlineEpoch();     // wall-clock finish line while Running
void setDeadlineEpoch(time_t t);
void restoreRunning(uint32_t totalSeconds, uint32_t remaining, uint8_t completedToday);
void restoreFinished(uint32_t totalSeconds, uint8_t completedToday);
void restoreIdle(uint32_t totalSeconds);
void setCompletedToday(uint8_t n);
void setDayStamp(uint32_t epochDay);

}  // namespace pomodoro
