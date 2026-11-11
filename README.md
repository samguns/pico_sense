# Pico Sense

A Zephyr-based firmware project for the Raspberry Pi Pico 2 (RP2350) that demonstrates a compact USB audio + display + touch device.

Linux-oriented host commands live in this file. For PowerShell, COM ports, and Windows USB notes, see [README-Windows.md](README-Windows.md).

## Features

- USB CDC ACM serial console
- USB Audio Class 2.0 (UAC2) headset device
- ST7796S LCD in 480x320 landscape (90-degree rotation)
- GT911 touch controller
- MAX98357A speaker output over PIO/DMA I²S (see [AUDIO.txt](AUDIO.txt))
- Embedded command console for boot and loopback control
- Compact state/voltage header, one network line, and English/Chinese dialogue
  below it. Recognized speech is white and spoken text green, without role labels.

The Xavier LCD sink cycles valid physical-interface IPv4 entries every three
seconds. It consumes `/speech/text` (`dm_mc02_interfaces/SpeechText`) from the
local speech SSE bridge. Partial recognition updates one entry; text wraps and
older entries scroll upward. The existing 15 MHz SPI DMA, framebuffer, double
staging buffers and transient volume overlay are retained.

Internal console command `talk <uint64-id> w <UTF-8 text>` displays recognized
speech; `g` instead of `w` displays spoken text. Reusing an ID replaces the
entry. The bounded history holds eight messages, each at most 767 UTF-8 bytes.
`OK talk <id>` is sent only after LCD transfers complete. Status timeout clears
the header/network fields without erasing dialogue. Font provenance and the
offline generator are documented in [fonts/README.md](fonts/README.md).

## Hardware target

This project is built for the Zephyr board target:

- `rpi_pico2/rp2350a/m33`

## Repository layout

```text
pico_sense/
├── app.overlay
├── CMakeLists.txt
├── prj.conf
├── west.yaml
├── README.md
├── README-Windows.md
└── src/
    ├── main.c
    ├── uac2_headset.c
    ├── uac2_headset.h
    ├── usb.c
    └── usb.h
```

## Prerequisites

Install Git, Python 3.12 or newer, CMake 3.28 or newer, Ninja, and the host
prerequisites for Zephyr. Use a Zephyr SDK compatible with the selected checkout
(the manifest-pinned checkout specifies SDK 1.0.1).

The project can live anywhere. Run the commands below from the project root.
Shell examples use Bash on Linux. For PowerShell, COM ports, and Windows USB
notes, see [README-Windows.md](README-Windows.md).

## Setup

For a new workspace, place this repository in its own parent directory, then run:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install west
west init -l . --mf west.yaml
west update
export ZEPHYR_BASE="$(west list zephyr -f '{abspath}')"
west packages pip --install
west sdk install --gnu-toolchains arm-zephyr-eabi
```

`west init` creates `.west` in the parent directory. The manifest downloads its
pinned dependencies from upstream repositories; no existing local checkout or
particular repository directory name is required.

For an existing workspace, skip `west init` / `west update` / `west sdk install`.
Activate that workspace's Python environment and set `ZEPHYR_BASE` and
`ZEPHYR_SDK_INSTALL_DIR` from it. From inside that workspace,
`west list zephyr -f '{abspath}'` prints the checkout location. Keep those
values in the session or in your user environment; do not commit host-specific
absolute paths. If this directory is itself a west workspace, `[zephyr] base`
stays the manifest-relative `deps/zephyr`.

Before the first build, apply the USB audio driver fixes following
[patches/README.txt](patches/README.txt). These prevent a USB interrupt stall and excessive queueing logs when
UAC2 is enabled. Skip applying the patch if it is already present.

## Build

```bash
west build -p always -b rpi_pico2/rp2350a/m33 .
```

This produces the firmware image in the build directory, typically as:

```text
build/zephyr/zephyr.uf2
```

## Flash

1. Put the Pico into BOOTSEL mode.
2. Run:

```bash
west flash --runner uf2
```

If `west flash` is not available in your environment, copy the generated `.uf2` file to the USB mass-storage device presented by the Pico while it is in BOOTSEL mode.

## Serial console

After flashing, set `SERIAL_PORT` to the CDC device assigned by your host, then
open it at 115200 baud. On Linux, list candidates with `ls /dev/ttyACM*`:

```bash
read -r -p "CDC serial device: " SERIAL_PORT
minicom -D "$SERIAL_PORT" -b 115200
```

You can also use `screen`, `picocom`, or a similar serial terminal if preferred.

## Runtime commands

The app listens for these commands via the console:

- `voltage 12.64` – displays a voltage (0..99.99 V, up to two decimal places)
- `boot` – enters the USB bootloader
- `loop` – USB playback copied to USB record (ignore the INMP441)
- `noloop` – USB record from the INMP441 (default)
- `tone` – plays a one-second speaker test tone
- `vol 0..100` – sets speaker volume (default 25%)

## Battery voltage demo

Build and flash the firmware using the steps above. The LCD immediately shows
`STATE UNAVAILABLE` at the top left and `--.-- V` centered beneath it.
The Xavier supplies confirmed robot state, measured battery voltage and assigned
IPv4 addresses from `/robot/status`. `ARMED` uses larger amber accent text.
The USB host supplies illustrative readings; this demo does not measure a battery.

Close other serial terminals, then run:

```bash
python3 -m pip install pyserial
python3 scripts/voltage_demo.py --port /dev/ttyACM0
python3 scripts/voltage_demo.py --port /dev/ttyACM0 --voltage 12.64
```

On Windows, use `python` and a port such as `--port COM5`.
Without `--voltage`, the script sends ten samples, ending at 12.64 V.
The display expires readings after eight seconds without host updates.
The default sample interval is 0.5 seconds (`--interval` changes it).

Protocol: send ASCII `voltage 12.64\n` over USB CDC at 115200 baud. CRLF also
works. The board replies `OK voltage 12.64` after the LCD write completes,
or `ERR ...` for invalid input/display failure. Existing console echo and
log messages can appear between responses; the script waits for the matching ACK.
Accepted inputs are unsigned decimal volts, 0..99.99, with at most two decimal
places. Oversized lines are discarded through the next newline.
Only the main voltage region is redrawn on updates. Touch diagnostics still report
the panel's native portrait coordinates; the battery display has no touch controls.
`voltage unavailable` clears the reading; `row 0 SLEEP` sets robot state and
`row 1 WLAN1 192.168.31.130/24` sets a network row. Rows 1–5 carry network
entries; the host omits IPv6 and interfaces without a valid IPv4 address.
The board replies `OK row <index>` after writing the row.

LCD SPI runs at 15 MHz. A 300 KiB wire-order RGB565 framebuffer tracks final
pixels; each 16-line rendering batch sends only its changed bounding rectangle.
Two aligned DMA strip buffers use 30 KiB total. Narrow regions use taller
batches within the same buffer capacity; the volume HUD needs two transfers
rather than seventeen. A dedicated worker starts the
transfer before the main thread continues rendering the next strip, allowing
rendering/copying to overlap SPI DMA without modifying the Zephyr display driver.
Buffers remain owned by the worker until completion. Console acknowledgements
wait for all queued transfers; failures invalidate the affected range for retry.
Unchanged text/voltage refreshes the watchdog without rendering. LCD DMA stays
on channels 14/15 and IRQ1, separately from audio's DMA channels and IRQ0.

## Touch volume

Slide one finger vertically on the right half of the landscape panel to adjust
speaker volume (up increases, down decreases). The gesture must start on the
right half; taps, primarily horizontal movement, and multitouch are ignored.
A 6-pixel threshold rejects touch jitter. A 240-pixel drag spans 0?100% volume,
and reversing at either limit responds immediately.

A rounded vertical HUD near the right edge shows the percentage and an eased
fill level. It fades in over 120 ms, remains visible for 900 ms after the last
adjustment, then fades out over 260 ms. The HUD is composited over current status
content, which is restored as it disappears. Touch reports are coalesced without
blocking the input thread; all drawing is serialized with console status updates.
Audio gain ramps over samples to avoid abrupt gain changes. Volume affects the
speaker only, not microphone capture or the Xavier's host mixer. The initial
speaker volume remains 25% after reboot.

Console `vol` reports current volume; `vol 0..100` sets it and shows the same HUD.
Native controller/display checks run with a Linux C compiler:

```bash
python3 tests/test_volume_native.py
python3 tests/test_display_native.py
```

## What the app does

At runtime, the firmware:

- displays robot state, battery voltage and IPv4 network entries in landscape
- checks the GT911 touch sensor
- exposes a USB composite device with CDC + UAC2
- supports console-driven behavior for testing and recovery

## Useful commands

```bash
west build -p always -b rpi_pico2/rp2350a/m33 .
west flash --runner uf2
```

After moving the source, SDK, or Zephyr checkout, create a fresh build with
`west build -p always -b rpi_pico2/rp2350a/m33 .`. Generated build directories
contain absolute paths and are ignored by Git; do not copy them between machines.

For configuration and board tuning:

```bash
west build -t menuconfig
```

Host audio tests:

```bash
python3 tests/test_audio.py
```

Optional UART diagnostics (GP0 TX, GP1 RX, 115200 baud):

```bash
west build -d build-debug -b rpi_pico2/rp2350a/m33 . -- \
  -DEXTRA_CONF_FILE=debug.conf -DEXTRA_DTC_OVERLAY_FILE=debug-uart.overlay
```

For the display isolation image with USB/audio startup disabled and an LED
heartbeat after drawing:

```bash
west build -d build-no-usb -b rpi_pico2/rp2350a/m33 . -- \
  -DEXTRA_CONF_FILE=debug.conf -DEXTRA_DTC_OVERLAY_FILE=debug-uart.overlay \
  -DPICO_SENSE_DIAGNOSTIC_NO_USB=ON
```

## Troubleshooting

- If `west` cannot find Zephyr, verify `ZEPHYR_BASE` is exported correctly.
- If the board does not flash, confirm the Pico is in BOOTSEL mode.
- If no serial device appears, check `dmesg` or run `ls /dev/ttyACM*` after plugging in the board.
- If the touch or display does not initialize, verify the hardware wiring and overlay settings in `app.overlay`.

## Notes

This project uses a custom Zephyr overlay to configure the display, touch panel, and USB audio device on the RP2350. The application logic is centered in [src/main.c](src/main.c) and the USB audio behavior is implemented in [src/uac2_headset.c](src/uac2_headset.c).

## Touch Wi-Fi settings

Tap WIFI on the network line to open Xavier Wi-Fi settings. The Station/AP slider, visible-network picker and masked password keyboard communicate with Xavier through the existing USB CDC display bridge. Saved networks reuse host credentials. New personal secured networks open the keyboard; open networks do not need one. Host installation and validation are documented in `mecanum_mc02/xavier_host/docs/pico-wifi-control-20261008.md`. Wi-Fi settings consume touch input while open, leaving the volume and listening controls available again after CLOSE.

## LVGL display trial

The Wi-Fi panel, dashboard, speech text, listening popup, and volume overlay
use LVGL in this opt-in configuration:

```bash
west update lvgl
west build -d build-lvgl -b rpi_pico2/rp2350a/m33 . -- -DEXTRA_CONF_FILE=lvgl.conf
```

In PowerShell, quote the final CMake argument: `'-DEXTRA_CONF_FILE=lvgl.conf'`.
A normal build without `lvgl.conf` selects the original Wi-Fi screen.

Tap the network line to open settings. Use the Station/AP slider, manually
refresh the network list, select a network, and use the masked LVGL keyboard
for a new password. The existing Xavier Wi-Fi protocol is unchanged; there is
no automatic or periodic scan. Close returns to the dashboard.

The display uses two shared 480-by-16 RGB565 buffers (30 KiB total) with the
existing SPI DMA worker. LVGL releases a buffer only after its SPI transfer
finishes. The 300 KiB legacy framebuffer is removed; LVGL restores either
screen from its retained UI objects. The 64 KiB LVGL heap covers both screens.
The complete trial uses approximately 145 KiB of the 520 KiB SRAM region.
All LVGL UI/input calls run on the console thread; the worker calls only
`lv_display_flush_ready`. Audio channels/interrupt ownership and 15 MHz SPI
remain unchanged. Network labels reuse the existing Unifont glyph source.

Host interaction tests with the pinned LVGL library:

```bash
cmake -S tests/lvgl_native -B build-lvgl-native -G Ninja
cmake --build build-lvgl-native --target wifi_lvgl_test
ctest --test-dir build-lvgl-native --output-on-failure
```

The native test also covers eight maximum-length messages plus rollover,
32-pixel mixed CJK/English text, listening tap/expiry, volume overlays, and
screen restoration with all 32 network entries allocated. Spoken text stays
green. Status/text acknowledgements drain queued transfers; failed writes
invalidate the active screen for retry. The default build retains the original
renderer for rollback.

### Touch polling reliability

The application includes `drivers/input_gt911.c`, derived from the pinned
Zephyr GT911 driver. CMake replaces the upstream source while retaining its
Kconfig and devicetree bindings. Coordinates are copied before acknowledging
0x814E; failed coordinate/ACK reads are retried without changing contact state.
Touch polling uses a dedicated 2 KiB workqueue stack so USB/audio system work
cannot hold up touch polls. No reset of the shared LCD/touch reset pin is used.
`touchstatus` on CDC reports poll/frame/error counters, report age, contact and
gesture state for diagnosing a recurrence. Native fault-injection test:
`CC=gcc python tests/test_gt911_native.py` (PowerShell: `$env:CC='gcc'`).

LVGL UTF-8 text encoding is explicitly enabled in `lvgl.conf`; the minimal
Zephyr LVGL configuration otherwise selects ASCII, which splits Chinese
UTF-8 bytes into unrelated Latin glyphs. Native LVGL defaults differ.
