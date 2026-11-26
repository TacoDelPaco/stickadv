# AGENTS.md — StickADV Project Guide

Instructions and reference notes for AI coding agents working on **StickADV**.

---

## Mandatory Post-Change Workflow

Whenever you make changes to this repository, **automatically perform all of the following steps before finishing** (unless explicitly instructed otherwise):

1. **Run host-native unit tests** (must compile cleanly with `-Wall -Wextra` and pass):
   ```bash
   g++ -std=c++17 -O2 -Wall -Wextra -Iinclude \
       test/test_core.cpp src/hid_parser.cpp src/imu_controller.cpp \
       src/sim_engine.cpp src/renderer.cpp src/betaflight_osd.cpp \
       -lm -o /tmp/test_core && /tmp/test_core
   ```
2. **Rebuild the ESP32-S3 firmware** using PlatformIO (install with `pip install platformio` if `pio` is not yet installed):
   ```bash
   pio run -e cardputer-adv
   ```
3. **Regenerate the prebuilt firmware binaries in `dist/`** whenever any firmware code changes, so M5Launcher and M5Burner binaries always match `main`:
   ```bash
   cp .pio/build/cardputer-adv/firmware.bin dist/stickadv.bin
   python3 ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 merge_bin \
       --flash_mode qio --flash_freq 80m --flash_size 8MB \
       -o dist/stickadv-merged.bin \
       0x0 .pio/build/cardputer-adv/bootloader.bin \
       0x8000 .pio/build/cardputer-adv/partitions.bin \
       0xe000 ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
       0x10000 .pio/build/cardputer-adv/firmware.bin
   ```
4. **Commit and push** all changes (including updated `dist/` binaries) to the remote repository:
   ```bash
   git status
   git add -A
   git commit -m "<clear, concise summary of changes>"
   git push
   ```
   Do not leave untracked build artifacts or temporary files in the repo (`.pio/` is gitignored; clean up temporary files in `/tmp`).

---

## Project Overview & Target Hardware

- **App**: **StickADV** — 60 FPS full-color 3D FPV racing simulator ported from EdgeTX `Game-simulator.lua` with a Betaflight Analog OSD and tri-mode controller input.
- **Hardware**: **M5Stack Cardputer-Adv**
  - MCU: Stamp-S3A (`ESP32-S3FN8`, 240 MHz, 8 MB Flash, **no PSRAM**)
  - Display: 240×135 ST7789V2 LCD (DMA SPI via `M5Canvas` 16-bit RGB565 sprite — 64.8 KB in internal SRAM)
  - IMU: BMI270 6-axis accel + gyro
  - Keyboard: TCA8418 I²C keyboard (arrow keys work both with `Fn` and directly as `; . , /`, plus `WASD`)
  - Audio: ES8311 speaker codec (default gentle volume `20%`)
- **Hardware Quirks & Constraints**:
  - **No PSRAM**: Keep static RAM allocations lean and avoid dynamic heap churn inside `loop()`.
  - **USB-OTG Host**: `ARDUINO_USB_CDC_ON_BOOT=0` in `platformio.ini` frees the ESP32-S3 internal USB-OTG PHY for ESP-IDF `usb_host_install()` (EdgeTX USB HID radios). Do not re-enable USB CDC on boot.
  - **BMI270 Axis Signs**: On the Cardputer-Adv, both X (roll) and Y (pitch) BMI270 axes are negated relative to the screen/pilot frame. `ImuController::computeTiltAxes()` negates both `rawRollDeg - zeroRollDeg` and `rawPitchDeg - zeroPitchDeg` before applying `settings.imuInvertY`.
  - **RGB565 Endianness**: `M5Canvas` stores big-endian RGB565 (`FB_SWAP_BYTES=1` under `ARDUINO`, `0` on native host builds). Always use `rgb565()`, `unpack_rgb565_raw()`, `pack_rgb565_raw()`, `blend565()`, etc. from `include/config.h`.

---

## Codebase Map

| File(s) | Purpose |
|---|---|
| `include/config.h` | Screen/FPS constants (`240×135`, `60 FPS`), RGB565 helpers, `SimSettings`, `PilotInput`, enums |
| `src/main.cpp` | `setup()`/`loop()`, 60 FPS pacing, keyboard sampling, discrete UI navigation, `cycleSettingItem()` |
| `src/imu_controller.cpp`, `include/imu_controller.h` | BMI270 complementary filter, `computeTiltAxes()`, and smoothed lower-sensitivity keyboard flight controls |
| `src/input_manager.cpp`, `include/input_manager.h` | Tri-mode input arbiter (`USB_HID` > `BLE_HID` > `IMU_KB`) |
| `src/usb_edgetx_host.cpp`, `src/ble_hid_host.cpp`, `src/hid_parser.cpp` | Core 0 USB-OTG EdgeTX HID host, low-latency NimBLE ExpressLRS Joystick host (`7.5 ms`), and dynamic HID report descriptor parser |
| `src/sim_engine.cpp`, `include/sim_engine.h` | Core 3D FPV physics, gate/flag spawning & collision, scoring, simulated 4S LiPo/RSSI telemetry, audio queue |
| `src/renderer.cpp`, `include/renderer.h` | Direct RGB565 3D software rasterizer (tilting horizon, ground grid, gates/flags, CRT scanlines, RF static) |
| `src/betaflight_osd.cpp`, `include/betaflight_osd.h` | MAX7456-style 5×7 outlined font/icons, Flight OSD, Title overlay, Settings menu, Input Monitor, Post-Flight summary |
| `src/audio_fx.cpp`, `src/storage.cpp` | Non-blocking ES8311 tones and NVS persistence (`Preferences` namespace `"stickadv"`) |
| `test/test_core.cpp` | Host-native unit & visual render verification tests |
| `dist/stickadv.bin`, `dist/stickadv-merged.bin` | Prebuilt ESP32-S3 binaries for M5Launcher (`0x10000`) and M5Burner (`0x0000`) |

---

## Key Conventions & Pitfalls

1. **Settings Menu Sync**:
   - The item count (`SETTINGS_COUNT`) and `switch (idx)` order in `cycleSettingItem()` (`src/main.cpp`) **must** stay strictly synchronized with `labels[]` and `vals[]` in `BetaflightOsd::drawSettingsMenu()` (`src/betaflight_osd.cpp`).
   - Support both Left (`-1`) and Right/Enter (`+1`) navigation in the Settings menu.
   - Any new setting persisted in `SimSettings` must also be loaded/saved in `src/storage.cpp` with safe defaults and bounds checks.

2. **UI & Menu Design (240×135 Tiny Display)**:
   - Keep menus clean, airy, and plain-English — avoid walls of text or cryptic acronyms.
   - The built-in bitmap font (`kFontTable` in `src/betaflight_osd.cpp`) is 5×7 with 6 px advance (`~38` chars full width, `~35` chars inside bordered panels). Lowercase letters auto-convert to uppercase; only characters present in `kFontTable` (`A-Z`, `0-9`, `: . - + / % ! ? [ ] ( ) < > = _ * #`) render (anything else becomes a space).
   - When modifying menus or HUD layout, visually verify by rendering native PPM/PNG frames on host to confirm spacing, alignment, and zero clipping.

3. **Native Host Testability**:
   - Keep core logic (`hid_parser`, `imu_controller`, `sim_engine`, `renderer`, `betaflight_osd`) free of hard Arduino dependencies (guard hardware-only code with `#ifdef ARDUINO`) so `[env:native]` and `test/test_core.cpp` always build and run fast on Linux/macOS.
   - Keep `[env:native]` in `platformio.ini` and the test command in `README.md` in sync if new portable `.cpp` files are added to tests.

4. **Control Tuning**:
   - Digital keyboard controls (`src/imu_controller.cpp`) use capped peak throw (`KB_ROLL_MAX`, `KB_VERT_MAX`) with smooth ramp-up and crisp release decay (`stepKeyAxis`) because raw ±0.90 digital steps move across an entire gate width in ~0.1 s and are unplayable.
   - Do not change analog USB/BLE stick scaling unless explicitly requested — digital keyboard smoothing only applies in `ImuController`.
