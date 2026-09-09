#pragma once

#include <cstddef>
#include <cstdint>

// Minimal parser for CrossInk's /.crosspoint/global_stats.bin (version 3,
// 159 bytes, little-endian). Reimplemented here on purpose: CrossSlate and
// CrossInk are separate repos, so only the on-disk format is shared.
//
// Layout (offsets from serializeStats in CrossInk's GlobalReadingStats.cpp):
//   [0]      version (must be 3)
//   [1..4]   totalSessions      u32 LE
//   [5..8]   totalReadingSeconds u32 LE
//   [9..12]  totalPagesTurned   u32 LE
//   [13..16] completedBooks     u32 LE
//   [17..32] timeOfDaySeconds   4 x u32 LE
//   [33..60] dayOfWeekSeconds   7 x u32 LE
//   [61..64] readingHistoryAnchorDay u32 LE (day index of bits[0])
//   [65..156] readingHistoryBits 92 bytes, 730 days, 1 bit per day
//            (bit i of the array = anchorDay + i epoch-day)
//   [157..158] longestReadingStreak u16 LE

constexpr size_t RS_HISTORY_DAYS = 730;
constexpr size_t RS_HISTORY_BYTES = (RS_HISTORY_DAYS + 7) / 8;  // 92
constexpr size_t RS_FILE_SIZE = 159;
constexpr uint8_t RS_VERSION = 3;

struct ReadingSnapshot {
  bool valid = false;
  uint32_t totalSessions = 0;
  uint32_t totalReadingSeconds = 0;
  uint32_t totalPagesTurned = 0;
  uint32_t completedBooks = 0;
  uint32_t anchorDay = 0;  // epoch-day index of bits[0]
  uint16_t longestReadingStreak = 0;
  uint8_t bits[RS_HISTORY_BYTES] = {0};

  // True when the given day index (absolute epoch-day) was a reading day.
  bool dayBit(uint32_t dayIndex) const {
    if (dayIndex < anchorDay) return false;
    const uint32_t idx = dayIndex - anchorDay;
    if (idx >= RS_HISTORY_DAYS) return false;
    return (bits[idx / 8] & static_cast<uint8_t>(1u << (idx % 8))) != 0;
  }

  // Mark a day in a display-only snapshot. If CrossInk's history window is
  // stale because it was offline, advance this in-memory window just enough to
  // include the requested newer day. The source stats file is never modified.
  bool setDayBit(uint32_t dayIndex) {
    if (anchorDay == 0 || dayIndex < anchorDay) return false;
    const uint32_t lastDay = anchorDay + static_cast<uint32_t>(RS_HISTORY_DAYS - 1);
    if (dayIndex > lastDay) {
      const uint32_t shift = dayIndex - lastDay;
      uint8_t oldBits[RS_HISTORY_BYTES];
      for (size_t i = 0; i < RS_HISTORY_BYTES; ++i) oldBits[i] = bits[i];
      for (size_t i = 0; i < RS_HISTORY_BYTES; ++i) bits[i] = 0;
      if (shift < RS_HISTORY_DAYS) {
        for (size_t oldIndex = shift; oldIndex < RS_HISTORY_DAYS; ++oldIndex) {
          if (oldBits[oldIndex / 8] & static_cast<uint8_t>(1u << (oldIndex % 8))) {
            const size_t newIndex = oldIndex - shift;
            bits[newIndex / 8] |= static_cast<uint8_t>(1u << (newIndex % 8));
          }
        }
      }
      anchorDay += shift;
    }
    const uint32_t idx = dayIndex - anchorDay;
    if (idx >= RS_HISTORY_DAYS) return false;
    bits[idx / 8] |= static_cast<uint8_t>(1u << (idx % 8));
    return true;
  }
};

struct ReadingDisplayCache {
  bool initialized = false;
  uint32_t lastKnownDay = 0;
  uint32_t lastTotalSeconds = 0;
  uint32_t lastTotalPages = 0;
  uint32_t inferredReadDay = 0;
};

// Pure parse from an in-memory buffer (host-testable).
ReadingSnapshot readingSnapshotParse(const uint8_t* data, size_t len);

// Reads and parses /.crosspoint/global_stats.bin via SdMan. Never fails
// visibly: invalid/missing file yields a snapshot with valid=false.
ReadingSnapshot readingSnapshotLoad();

// Current streak as of todayEpochDay (inclusive). Counts consecutive set bits
// walking back from today; today itself may be unset without breaking the
// streak (the day is not over yet).
uint16_t readingCurrentStreak(const ReadingSnapshot& snap, uint32_t todayEpochDay);

// Longest run of set bits anywhere in the history window.
uint16_t readingLongestStreak(const ReadingSnapshot& snap);

// Epoch-day index for a UTC date (days since 1970-01-01). Proleptic
// Gregorian, no external libs.
uint32_t readingEpochDayFromYMD(int year, int month, int day);

// Maps an absolute day into a Monday-first heatmap ending with the current
// week. Today always belongs to the final column.
bool readingHeatmapCell(uint32_t day, uint32_t today, int weeks, int& column, int& row);

// Resolves today from a live day or the last cached connected day. When the
// CrossInk totals advance while offline, overlays that cached day as read and
// remembers it across redraws. The source stats file is never modified.
uint32_t readingPrepareForDisplay(ReadingSnapshot& snap, uint32_t liveTodayDay,
                                  ReadingDisplayCache& cache);
