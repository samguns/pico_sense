# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

set(DTC_OVERLAY_FILE app.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pico_sense)

option(PICO_SENSE_DIAGNOSTIC_NO_USB "Skip USB startup and blink after drawing the display" OFF)
if(PICO_SENSE_DIAGNOSTIC_NO_USB)
  target_compile_definitions(app PRIVATE PICO_SENSE_DIAGNOSTIC_NO_USB=1)
endif()

target_sources(app PRIVATE src/main.c src/usb.c src/uac2_headset.c)
target_sources_ifdef(CONFIG_PICO_SENSE_AUDIO app PRIVATE src/audio_output.c)
