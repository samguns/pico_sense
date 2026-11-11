# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

set(DTC_OVERLAY_FILE app.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pico_sense)

option(PICO_SENSE_DIAGNOSTIC_NO_USB "Skip USB startup and blink after drawing the display" OFF)
if(PICO_SENSE_DIAGNOSTIC_NO_USB)
  target_compile_definitions(app PRIVATE PICO_SENSE_DIAGNOSTIC_NO_USB=1)
endif()

target_sources(app PRIVATE src/main.c src/display_font.c src/battery_voltage.c src/volume_control.c src/wifi_ui.c src/usb.c src/uac2_headset.c)
target_sources_ifdef(CONFIG_PICO_SENSE_AUDIO app PRIVATE src/audio_output.c)

target_sources_ifdef(CONFIG_PICO_SENSE_LVGL_WIFI app PRIVATE src/wifi_ui_lvgl.c)

if(CONFIG_PICO_SENSE_LVGL_WIFI)
  target_sources(app PRIVATE src/battery_display_lvgl.c src/dashboard_lvgl.c src/ui_cards.c)
else()
  target_sources(app PRIVATE src/battery_display.c)
endif()

# Board-local GT911 driver: atomic read/ack and a dedicated polling workqueue.
# Keep the upstream Kconfig/devicetree binding, replacing only its source.
get_target_property(pico_input_sources drivers__input SOURCES)
list(FILTER pico_input_sources EXCLUDE REGEX "(^|/)input_gt911\\.c$")
set_property(TARGET drivers__input PROPERTY SOURCES "${pico_input_sources}")
target_sources(app PRIVATE drivers/input_gt911.c)
