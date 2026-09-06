#include "../src/reading_stats.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

uint32_t readLe32(const uint8_t* d, size_t off) {
  return static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
         (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
}

void writeLe32(uint8_t* d, size_t off, uint32_t v) {
  d[off] = v & 0xFF;
  d[off + 1] = (v >> 8) & 0xFF;
  d[off + 2] = (v >> 16) & 0xFF;
  d[off + 3] = (v >> 24) & 0xFF;
}

// Builds a synthetic version-3 stats file. days: absolute epoch-days marked
// as read.
size_t buildStatsFile(uint8_t* out, uint32_t anchorDay, const uint32_t* days, size_t dayCount,
                      uint32_t totalSeconds, uint32_t totalPages, uint16_t longestStreak) {
  std::memset(out, 0, RS_FILE_SIZE);
  out[0] = RS_VERSION;
  writeLe32(out, 1, 42);              // totalSessions
  writeLe32(out, 5, totalSeconds);
  writeLe32(out, 9, totalPages);
  writeLe32(out, 13, 3);              // completedBooks
  // timeOfDay / dayOfWeek stay zero
  writeLe32(out, 61, anchorDay);
  for (size_t i = 0; i < dayCount; ++i) {
    const uint32_t idx = days[i] - anchorDay;
    assert(idx < RS_HISTORY_DAYS);
    out[65 + idx / 8] |= static_cast<uint8_t>(1u << (idx % 8));
  }
  out[157] = longestStreak & 0xFF;
  out[158] = (longestStreak >> 8) & 0xFF;
  return RS_FILE_SIZE;
}

// Mirrors the parser's field reads so tests assert against real offsets.
}  // namespace

int main() {
  // Epoch-day sanity: 2026-09-05 must be a stable value across runs.
  const uint32_t today = readingEpochDayFromYMD(2026, 9, 5);
  assert(readingEpochDayFromYMD(1970, 1, 1) == 0);
  assert(readingEpochDayFromYMD(1970, 1, 2) == 1);
  assert(readingEpochDayFromYMD(2026, 9, 4) == today - 1);
  assert(readingEpochDayFromYMD(2000, 3, 1) == 11017);  // known civil-days value

  // Build: 5 consecutive reading days ending today, anchor = today - 729
  uint8_t file[RS_FILE_SIZE];
  const uint32_t days[5] = {today - 4, today - 3, today - 2, today - 1, today};
  const size_t len = buildStatsFile(file, today - 729, days, 5, 3600 * 90, 4212, 34);
  assert(len == RS_FILE_SIZE);

  ReadingSnapshot snap = readingSnapshotParse(file, len);
  assert(snap.valid);
  assert(snap.totalReadingSeconds == 3600 * 90);
  assert(snap.totalPagesTurned == 4212);
  assert(snap.completedBooks == 3);

  // Day bit: all five present, tomorrow absent, day before window absent
  assert(snap.dayBit(today));
  assert(snap.dayBit(today - 4));
  assert(!snap.dayBit(today - 5));
  assert(!snap.dayBit(today - 6));
  assert(!snap.dayBit(today + 1));
  // Out-of-window absolute days are safely false
  assert(!snap.dayBit(today + 400));
  assert(!snap.dayBit(today - 800));

  // Streaks: 5 consecutive ending today
  assert(readingCurrentStreak(snap, today) == 5);
  // Today unset but yesterday set: streak still counts from yesterday
  uint8_t file2[RS_FILE_SIZE];
  const uint32_t days2[5] = {today - 5, today - 4, today - 3, today - 2, today - 1};
  buildStatsFile(file2, today - 729, days2, 5, 100, 10, 0);
  ReadingSnapshot snap2 = readingSnapshotParse(file2, len);
  assert(readingCurrentStreak(snap2, today) == 5);
  // Gap breaks the streak
  uint8_t file3[RS_FILE_SIZE];
  const uint32_t days3[2] = {today - 4, today - 2};
  buildStatsFile(file3, today - 729, days3, 2, 100, 10, 0);
  ReadingSnapshot snap3 = readingSnapshotParse(file3, len);
  assert(readingCurrentStreak(snap3, today) == 0);  // gap between the two days
  // Longest run from bits (5-day block beats stored 34? no: stored wins if higher)
  assert(readingLongestStreak(snap) == 34);
  assert(readingLongestStreak(snap3) == 1);

  // Corrupt: wrong version
  file[0] = 2;
  ReadingSnapshot bad = readingSnapshotParse(file, len);
  assert(!bad.valid);
  file[0] = RS_VERSION;
  // Corrupt: truncated
  ReadingSnapshot bad2 = readingSnapshotParse(file, len - 1);
  assert(!bad2.valid);

  printf("test_reading_stats: all assertions passed\n");
  return 0;
}
