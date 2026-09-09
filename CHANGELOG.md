# Changelog

All notable changes to CrossSlate are documented here.

## [v39] - 2026-09-08

### Added

- Utilities page reading heatmap with a compact monochrome legend for unread days, read days, and the first day of each month.
- A deterministic marker for today: the last cell in the rightmost heatmap column is an outlined square with a thick X.

### Changed

- Moved the 14-week heatmap left to reserve a dedicated legend area on the right.
- Reworked the legend for the Xteink X4 e-ink display: `Sin leer` and `Leído` remain readable, while `Día1` identifies the unchanged month-start dot.
- Today no longer shares a visual state with reading activity. It is deliberately unlisted because its fixed position identifies it.
- The heatmap uses Monday-aligned weeks and Europe/Madrid civil dates.
- Saved Wi-Fi credentials are attempted independently, so an unavailable first SSID does not consume the connection budget for the next network.

### Fixed

- Corrected C++ UTF-8 escape handling that corrupted Spanish accented characters and inverted punctuation in Utilities.
- Preserved first-of-month dots while preventing them from being confused with the current-day marker.
- Retained offline reading activity by inferring the latest known civil day from CrossInk cumulative reading counters.

### Companion firmware

- The matching reader binary is **CrossInk v14**: `CrossInk-X4-v14-en.bin`.

## Installation

Install the appropriate `.bin` from the GitHub release using the Xteink X4 SD-card firmware update flow. CrossSlate and CrossInk occupy alternate OTA slots; install each image in its intended slot.
