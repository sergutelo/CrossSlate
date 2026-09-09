#include "reading_stats.h"

#include <cstdio>
#include <cstring>

#ifdef ARDUINO
#include <SDCardManager.h>
#endif

namespace {

uint32_t readLe32(const uint8_t* d, size_t off) {
  return static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
         (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
}

uint16_t readLe16(const uint8_t* d, size_t off) {
  return static_cast<uint16_t>(d[off]) | static_cast<uint16_t>(static_cast<uint16_t>(d[off + 1]) << 8);
}

// Days since 1970-01-01 for a proleptic Gregorian date. Standard civil-days
// algorithm (Howard Hinnant), valid for the years this device will ever see.
uint32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);                    // [0, 399]
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;          // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                   // [0, 146096]
  return static_cast<uint32_t>(era * 146097) + doe - 719468;
}

}  // namespace

uint32_t readingEpochDayFromYMD(int year, int month, int day) {
  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31) return 0;
  return daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
}

bool readingHeatmapCell(uint32_t day, uint32_t today, int weeks, int& column, int& row) {
  if (weeks <= 0 || day > today) return false;
  const uint32_t todayDow = (today + 3u) % 7u;  // 1970-01-01 = Thursday
  const uint32_t startMonday = today - todayDow - static_cast<uint32_t>(weeks - 1) * 7u;
  if (day < startMonday) return false;
  column = static_cast<int>((day - startMonday) / 7u);
  row = static_cast<int>((day + 3u) % 7u);
  return column >= 0 && column < weeks;
}

uint32_t readingPrepareForDisplay(ReadingSnapshot& snap, uint32_t liveTodayDay,
                                  ReadingDisplayCache& cache) {
  if (liveTodayDay != 0) cache.lastKnownDay = liveTodayDay;
  uint32_t today = cache.lastKnownDay;
  if (today == 0 && snap.valid && snap.anchorDay != 0) {
    today = snap.anchorDay + RS_HISTORY_DAYS - 1;
  }

  if (snap.valid) {
    const bool totalsAdvanced = snap.totalReadingSeconds > cache.lastTotalSeconds ||
                                snap.totalPagesTurned > cache.lastTotalPages;
    if (today != 0 && cache.initialized && totalsAdvanced) {
      // Normal path: CrossInk persisted new reading while CrossSlate was away.
      cache.inferredReadDay = today;
    } else if (today != 0 && !cache.initialized &&
               (snap.totalReadingSeconds != 0 || snap.totalPagesTurned != 0)) {
      // Bootstrap path: after an OTA there is no previous CrossSlate sample to
      // compare against. With a valid clock, prefer preserving a reading that
      // happened before this first visit over falsely showing Crossi asleep.
      // This overlay exists only for this one known day; later redraws use real
      // counter deltas and never rewrite CrossInk's source file.
      cache.inferredReadDay = today;
    }
    if (cache.inferredReadDay != 0) snap.setDayBit(cache.inferredReadDay);
    cache.lastTotalSeconds = snap.totalReadingSeconds;
    cache.lastTotalPages = snap.totalPagesTurned;
    cache.initialized = true;
  }
  return today;
}

ReadingSnapshot readingSnapshotParse(const uint8_t* data, size_t len) {
  ReadingSnapshot snap;
  if (len != RS_FILE_SIZE || data[0] != RS_VERSION) return snap;
  snap.totalSessions = readLe32(data, 1);
  snap.totalReadingSeconds = readLe32(data, 5);
  snap.totalPagesTurned = readLe32(data, 9);
  snap.completedBooks = readLe32(data, 13);
  snap.anchorDay = readLe32(data, 61);
  memcpy(snap.bits, data + 65, RS_HISTORY_BYTES);
  snap.longestReadingStreak = readLe16(data, 157);
  snap.valid = true;
  return snap;
}

#ifdef ARDUINO

ReadingSnapshot readingSnapshotLoad() {
  ReadingSnapshot snap;
  uint8_t data[RS_FILE_SIZE] = {0};

  auto file = SdMan.open("/.crosspoint/global_stats.bin", O_RDONLY);
  if (!file) return snap;
  const size_t n = file.read(data, sizeof(data));
  file.close();  // only one SD reader may hold a file open
  SdMan.sleep();

  return readingSnapshotParse(data, n);
}

#endif  // ARDUINO

uint16_t readingCurrentStreak(const ReadingSnapshot& snap, uint32_t todayEpochDay) {
  if (!snap.valid) return 0;
  // Walk back from today. An unset today does not break the streak (the day
  // is not over), matching CrossInk's semantics.
  uint16_t streak = 0;
  uint32_t day = todayEpochDay;
  if (!snap.dayBit(day)) {
    if (day == 0) return 0;
    --day;
  }
  while (snap.dayBit(day)) {
    ++streak;
    if (day == 0) break;
    --day;
  }
  return streak;
}

uint16_t readingLongestStreak(const ReadingSnapshot& snap) {
  if (!snap.valid) return 0;
  uint16_t best = 0;
  uint16_t run = 0;
  for (size_t i = 0; i < RS_HISTORY_DAYS; ++i) {
    const size_t byte = i / 8;
    const uint8_t mask = static_cast<uint8_t>(1u << (i % 8));
    if (snap.bits[byte] & mask) {
      if (++run > best) best = run;
    } else {
      run = 0;
    }
  }
  // The stored field wins if it is higher (CrossInk may know older history).
  if (snap.longestReadingStreak > best) best = snap.longestReadingStreak;
  return best;
}
