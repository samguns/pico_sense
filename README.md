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
BATTERY, `--.-- V`, DC VOLTAGE, and fixed MIN 0 V / MAX 28 V labels. There is no timestamp.
The USB host supplies illustrative readings; this demo does not measure a battery.

Close other serial terminals, then run:

```bash
python3 -m pip install pyserial
python3 scripts/voltage_demo.py --port /dev/ttyACM0
python3 scripts/voltage_demo.py --port /dev/ttyACM0 --voltage 12.64
```

On Windows, use `python` and a port such as `--port COM5`.
Without `--voltage`, the script sends ten samples, leaving 12.64 V on screen
with MIN 0 V and MAX 28 V. These fixed range labels appear at startup and do
not change with incoming samples.
The default sample interval is 0.5 seconds (`--interval` changes it).

Protocol: send ASCII `voltage 12.64\n` over USB CDC at 115200 baud. CRLF also
works. The board replies `OK voltage 12.64` after the LCD write completes,
or `ERR ...` for invalid input/display failure. Existing console echo and
log messages can appear between responses; the script waits for the matching ACK.
Accepted inputs are unsigned decimal volts, 0..99.99, with at most two decimal
places. Oversized lines are discarded through the next newline.
Only the main voltage region is redrawn on updates. Touch diagnostics still report
the panel's native portrait coordinates; the battery display has no touch controls.

## What the app does

At runtime, the firmware:

- displays battery voltage plus fixed MIN 0 V / MAX 28 V in landscape
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
