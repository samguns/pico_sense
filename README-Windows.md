# Pico Sense (Windows)

A Zephyr-based firmware project for the Raspberry Pi Pico 2 (RP2350) that demonstrates a compact USB audio + display + touch device.

This file is the Windows counterpart to [README.md](README.md). Features, hardware, layout, runtime commands, and firmware behavior are the same; only host commands and USB/serial handling differ.

## Features

- USB CDC ACM serial console
- USB Audio Class 2.0 (UAC2) headset device
- ST7796S 320x480 LCD display
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

Install Git for Windows, Python 3.12 or newer, CMake 3.28 or newer, Ninja, and
the host prerequisites for Zephyr. Use a Zephyr SDK compatible with the selected
checkout (the manifest-pinned checkout specifies SDK 1.0.1). Add CMake and Ninja
to `PATH`.

PowerShell examples below assume you run them from the project root. If you
prefer Git Bash, follow [README.md](README.md) instead.

The first time you activate a venv in PowerShell you may need:

```powershell
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
```

## Setup

For a new workspace, place this repository in its own parent directory, then run:

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install west
west init -l . --mf west.yaml
west update
$env:ZEPHYR_BASE = (west list zephyr -f '{abspath}')
west packages pip --install
west sdk install --gnu-toolchains arm-zephyr-eabi
```

`west init` creates `.west` in the parent directory. The manifest downloads its
pinned dependencies from upstream repositories; no existing local checkout or
particular repository directory name is required.

For an existing workspace, skip `west init` / `west update` / `west sdk install`.
Activate that workspace's Python environment and set:

```powershell
$env:ZEPHYR_BASE = (west list zephyr -f '{abspath}')
$env:ZEPHYR_SDK_INSTALL_DIR = "C:\path\to\zephyr-sdk-1.0.1"
```

Run `west list` from the existing workspace so the first line resolves to that
checkout. Replace the SDK path with wherever you installed it. Keep these in
the session or in your user environment. Do not put host-specific absolute
paths in the repository (`.west\config`, `.vscode\settings.json`, or the
READMEs).

If this directory is itself a west workspace, `[zephyr] base` stays the
manifest-relative `deps/zephyr` created by `west update`.

Before the first build, apply the USB audio driver fixes following
[patches/README.txt](patches/README.txt). These prevent a USB interrupt stall and excessive queueing logs when
UAC2 is enabled. Skip applying the patch if it is already present.

```powershell
git -C $env:ZEPHYR_BASE apply --check "$PWD\patches\0001-udc-rpi-pico-audio-fixes.patch"
git -C $env:ZEPHYR_BASE apply "$PWD\patches\0001-udc-rpi-pico-audio-fixes.patch"
```

To check whether this exact patch is already applied:

```powershell
git -C $env:ZEPHYR_BASE apply --reverse --check "$PWD\patches\0001-udc-rpi-pico-audio-fixes.patch"
```

If that check succeeds, skip applying it again.

## Build

```powershell
west build -p always -b rpi_pico2/rp2350a/m33 .
```

This produces the firmware image in the build directory, typically as:

```text
build\zephyr\zephyr.uf2
```

## Flash

1. Put the Pico into BOOTSEL mode (hold BOOTSEL, plug in USB, then release).
2. Windows mounts a drive named `RPI-RP2`.
3. Run:

```powershell
west flash --runner uf2
```

If `west flash` is not available in your environment, copy the generated `.uf2`
file to that drive:

```powershell
Copy-Item build\zephyr\zephyr.uf2 <RPI-RP2 drive letter>:\
```

The drive disappears when the board reboots into the application.

## Serial console

After flashing, the composite device enumerates as **RP2350 UAC+CDC**
(`VID_2FE3&PID_0106`). The CDC function appears as a COM port.

In Device Manager, open **Ports (COM & LPT)** and note the `COMx` number for
**USB Serial Device**. Open it at 115200 baud with PuTTY, Tera Term, or:

```powershell
python -m pip install pyserial
python -m serial.tools.list_ports
python -m serial.tools.miniterm COMx 115200
```

Replace `COMx` with the port Device Manager assigned.

## Runtime commands

The app listens for these commands via the console:

- `boot` – enters the USB bootloader
- `loop` – USB playback copied to USB record (ignore the INMP441)
- `noloop` – USB record from the INMP441 (default)
- `tone` – plays a one-second speaker test tone
- `vol 0..100` – sets speaker volume (default 25%)

## What the app does

At runtime, the firmware:

- initializes the display and prints color bars
- checks the GT911 touch sensor
- exposes a USB composite device with CDC + UAC2
- supports console-driven behavior for testing and recovery

On Windows, Sound settings should list **RP2350 UAC+CDC** as a headset
(playback and recording). Select it as the output device to play 48 kHz 16-bit
stereo PCM. See [AUDIO.txt](AUDIO.txt) for speaker wiring.

## Useful commands

```powershell
west build -p always -b rpi_pico2/rp2350a/m33 .
west flash --runner uf2
```

After moving the source, SDK, or Zephyr checkout, create a fresh build with
`west build -p always -b rpi_pico2/rp2350a/m33 .`. Generated build directories
contain absolute paths and are ignored by Git; do not copy them between machines.

For configuration and board tuning:

```powershell
west build -t menuconfig
```

`menuconfig` needs a terminal that supports curses. Windows Terminal or a
recent PowerShell window usually works; Command Prompt often does not.

Host audio tests:

```powershell
python tests/test_audio.py
```

`tests/test_audio.py` compiles a small C shared library with `cc` and a `.so`
suffix. That path is Linux-oriented. On native Windows either skip it, run it
from WSL, or use Git Bash with a Unix toolchain. The firmware build does not
depend on this test.

Optional UART diagnostics (GP0 TX, GP1 RX, 115200 baud) using a USB–UART
adapter:

```powershell
west build -d build-debug -b rpi_pico2/rp2350a/m33 . -- `
  -DEXTRA_CONF_FILE=debug.conf -DEXTRA_DTC_OVERLAY_FILE=debug-uart.overlay
```

For the display isolation image with USB/audio startup disabled and an LED
heartbeat after drawing:

```powershell
west build -d build-no-usb -b rpi_pico2/rp2350a/m33 . -- `
  -DEXTRA_CONF_FILE=debug.conf -DEXTRA_DTC_OVERLAY_FILE=debug-uart.overlay `
  -DPICO_SENSE_DIAGNOSTIC_NO_USB=ON
```

## Troubleshooting

- If `west` cannot find Zephyr, verify `$env:ZEPHYR_BASE` in this session
  (`echo $env:ZEPHYR_BASE`) and that `.west\config` `[zephyr] base` is the
  relative manifest path `deps/zephyr`. `west list zephyr -f '{abspath}'`
  prints the checkout west will use.
- If `west` reports `unknown command "build"`, the workspace is not loading
  Zephyr's extensions. `.west\config` must use `file = west.yaml` (not the old
  `demo\west.yml`). Then run `west list`; `build` should appear under
  `west help`.
- If activating `.venv` fails, set the execution policy shown under Prerequisites.
- If the board does not flash, confirm the Pico is in BOOTSEL mode and that
  `RPI-RP2` appears in Explorer.
- If no COM port appears, open Device Manager and look under **Ports (COM & LPT)**
  and **Sound, video and game controllers**. Also check **Universal Serial Bus
  controllers** for **Unknown USB Device**.
- `USB\VID_0000&PID_0002` with status `0xC0000719` is a failed enumeration ghost,
  not a real VID/PID. The previous device on that hub port (often a Logitech
  receiver `VID_046D`) is unrelated leftover. Uninstall the unknown device, then
  confirm the USB SOF patch is applied and rebuild.
- If the color bars never appear after a UAC2 build, the firmware is likely
  stuck in the USB ISR because `SOF_RD` was not acknowledged. Apply
  [patches/0001-udc-rpi-pico-audio-fixes.patch](patches/0001-udc-rpi-pico-audio-fixes.patch).
- Windows `usbaudio2.sys` needs Full-Speed asynchronous UAC2 with explicit
  feedback. This project already sets `CONFIG_USBD_UAC2_FS_WINDOWS_WORKAROUND`.
  Do not switch the overlay to implicit feedback or SOF-synchronous ISO.
- After a PID change, uninstall leftover **Unknown USB Device** / `VID_0000`
  entries so Windows does not reuse a failed instance.
- If the touch or display does not initialize, verify the hardware wiring and
  overlay settings in `app.overlay`.

## Notes

This project uses a custom Zephyr overlay to configure the display, touch panel, and USB audio device on the RP2350. The application logic is centered in [src/main.c](src/main.c) and the USB audio behavior is implemented in [src/uac2_headset.c](src/uac2_headset.c).
