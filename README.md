# CrossSlate — a reading dashboard for the XTEink X4

**CrossSlate** is a second firmware (dual boot) for the **XTEink X4** e-reader that turns it into an e-ink dashboard: weather, tasks, pinned notes, hand-drawn doodles sent from your browser, a reading heatmap, a pomodoro timer, and a pet named **Crossi** that grows with your reading habits.

It lives in the second OTA slot of the X4 alongside **CrossInk** (the reader firmware), and both share the same SD card.

> ⚠️ **Use at your own risk.** This is a personal hobby firmware, flashed at your own discretion. It is **tested only on the XTEink X4**; Community reports suggest the X3 hardware is firmware-compatible, but rendering on its screen (528×792 vs 480×800) is unverified. Use on X3 at your own risk.

---

## Latest release — v39

The current Xteink X4 release is **CrossSlate v39**. It includes the complete reading-statistics and Utilities page work:

- A 14-week heatmap anchored to Monday, with a stable Europe/Madrid day calculation and offline reading inference from CrossInk cumulative counters.
- A compact, legible monochrome legend: empty cell = unread, inset fill = read, and center dot = first day of a month (`Día1`).
- Today is always the final cell in the rightmost column, rendered as an outlined square with a thick X. It has no legend entry and takes precedence if today is also the first day of a month.
- Correct UTF-8 Spanish UI rendering for accents and punctuation on the Utilities page.
- Imported Wi-Fi credentials now try each saved SSID with an independent connection deadline.

The matching reader-side companion binary is **CrossInk v14** (`CrossInk-X4-v14-en.bin`). It exposes the CrossSlate entry point and the shared SD-backed CrossSlate web page.

## Credits

This project **would not exist without**:

- **CrossInk** — [github.com/uxjulia/crossink](https://github.com/uxjulia/crossink) and the CrossPoint Reader ecosystem ([crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader)), the source of the SDK, the reader, and the web server. CrossSlate reuses its web server, its SD updater, and its reading-stats file format.
- **MicroSlate** — [github.com/Josh-writes/microslate-firmware](https://github.com/Josh-writes/microslate-firmware), the source of the BLE keyboard bridge, note writing, and the dual-boot idea.
- The XTEink X4/X3 community (see [Ecosystem](#ecosystem)) for inspiration and reference implementations.

CrossSlate is a fork derived from **multiple X4 projects**. All credit for the foundations belongs to the original authors; the added bugs are ours. 😄

---

## Features

### Main page (Dashboard)
- **Open-Meteo weather** with selectable city (11 cities), manual refresh via the right button
- **Upcoming tasks** (up to 5 × 80 chars)
- **Pinned note** (35 chars)
- **Doodle**: draw with your mouse or finger in the browser, with 8 hand-drawn style frames

### "Utilities" page
- **GitHub-style reading heatmap** (14 weeks × 7 days) fed by CrossInk's real reading stats (`/.crosspoint/global_stats.bin`)
- Current streak and record
- **Integrated pomodoro** (1–90 min) that survives deep sleep, with a giant 7-segment countdown on the sleep screen
- **Crossi** 🧡, the pet: happy if you read today, asleep if you missed 2 days, with random accessories (bow, top hat, Santa hat in December)

### Web server (from CrossInk)
- **CrossSlate tab** to edit tasks and the pinned note, and draw doodles from your Mac or phone browser

### Also
- Sleep screen with random BMP wallpapers from `/sleep` (shared with CrossInk) or the pomodoro countdown
- Wi-Fi credential import/merge from CrossInk
- BLE keyboard for writing notes (inherited from MicroSlate)
- SD updater: each firmware flashes the opposite slot from a `.bin` on the card

---

## Installation

### Option A — dual image from scratch (recommended for a fresh start)

Flashes **both firmwares at once** over a blank X4 or any previous firmware.

1. Copy `firmware/crossink-crossslate-dual-16MB-v33.bin` somewhere accessible
2. Flash over USB with esptool **at offset 0x0**:

```bash
esptool.py --chip esp32c3 --port /dev/ttyUSB0 --baud 460800 \
  write_flash 0x0 crossink-crossslate-dual-16MB-v33.bin
```

> ⚠️ This image targets X4 units with 16 MB flash. Verify your hardware first.

3. On first boot you will have CrossInk in slot 1 and CrossSlate in slot 2.

### Option B — update a single slot (daily method)

**The golden rule of dual boot:**

> The SD updater **always writes the opposite slot** from the running firmware, then **reboots into that other slot**.

- To update **CrossInk**: copy its `.bin` to the SD, boot into **CrossSlate** → Settings → System → SD Update
- To update **CrossSlate**: copy its `.bin` to the SD, boot into **CrossInk** → Settings → System → SD Update

You can never end up half-flashed: the running slot never overwrites itself.

### Files in this release

| File | What it is |
|---|---|
| `CrossInk-X4-v14-en.bin` | Current CrossInk companion binary with the CrossSlate web-tab integration |
| `CrossSlate-X4-v39-today-x.bin` | Current CrossSlate: offline-aware heatmap, Wi-Fi import fixes, Pomodoro, and Crossi |
| `crossink-crossslate-dual-16MB-v33.bin` | Earlier complete 16 MB dual-image baseline for from-scratch flashing |

---

## Usage

### Dashboard
- **Right button**: refresh the weather
- **Enter**: main menu
- **Esc (Back)**: reload data from the SD

### Utilities
- **Enter**: start/pause the pomodoro
- **Side buttons up/down**: ±1 minute of duration
- **Hold right button 2 s**: reset the pomodoro
- **Esc / Back**: back to the menu

### Web server (from CrossInk)
1. Enable the X4 access point or Wi-Fi
2. Open `http://<x4-ip>/crossslate`
3. Edit tasks and the pinned note, draw a doodle, pick a frame, press **Send to X4**
4. In CrossSlate press Confirm to reload the SD and see the changes

---

## How it works inside

```
Mac/phone (browser)
   │  http://x4/crossslate → tasks, note, canvas
   ▼
CrossInk (slot 1, web server)
   │  writes /crossslate/*.txt, *.bmp to the SD (atomic: .tmp → .bak → rename)
   ▼
Shared SD ◄──── also reads: cached weather, sleep wallpapers, global_stats.bin
   │
CrossSlate (slot 2, dashboard)
      reads the SD on boot / reload and paints it in ink
```

- **The SD card is the shared memory.** Neither firmware writes into the other's slot.
- **The heatmap captures nothing**: it reads the stats file CrossInk already maintains (730 days of history, 1 bit per day).
- **Crossi feeds on those same bits**: if you read, Crossi smiles.
- Doodles are rasterized to a **1-bit monochrome BMP in the browser**; the X4 only receives ready-to-paint bytes.

---

## Ecosystem

This firmware exists thanks to the XTEink X4/X3 community. Projects that inspired features or answered questions: [Habitink](https://github.com/mohitagw15856/Habitink) (streak grid), [InkStorm-Solo](https://github.com/SkyWalker541/InkStorm-Solo) (extended Open-Meteo), [CrossLuaReader](https://github.com/dcherrera/CrossLuaReader) (pomodoro), [crosspoint-reader-lockscreens](https://github.com/t0nyz0/crosspoint-reader-lockscreens) (sleep dashboards), and CrossPoint's [ClippingStore](https://github.com/crosspoint-reader/crosspoint-reader) (atomic write pattern). Full 40+ project report in the repo.

## Licenses and notes

- Respect the licenses of the base projects (CrossInk/CrossPoint and MicroSlate).
- **No** AGPL code was included (e.g. `crosspoint-chinesetype`).
- AliExpress X4 units may have USB flashing locked: always use the SD updater.
- The hardware clock (RTC) is on the X3, not the X4: time comes from NTP over Wi-Fi.

## Screenshots

See the [`screenshots/`](screenshots/) folder — dashboard, heatmap, pomodoro, Crossi, and the original concept sketches.
