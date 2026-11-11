/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "volume_control.h"
bool wifi_ui_active(void);
bool wifi_ui_touch(const struct volume_touch *touch);
bool wifi_ui_dirty(void);
int wifi_ui_command(const char *command);
void wifi_ui_render(int y, void (*pixel)(int, uint16_t));

/* UI-independent state/actions keep the Xavier console protocol unchanged. */
struct wifi_network { char ssid[65]; bool saved; unsigned security, signal; };
struct wifi_view {
    bool active, busy, ap, keyboard;
    unsigned count, selected;
    const struct wifi_network *nets;
    const char *message, *ap_ssid, *ap_password, *ap_address;
};
enum wifi_action { WIFI_CLOSE, WIFI_SCAN, WIFI_STATION, WIFI_AP, WIFI_SELECT,
                   WIFI_PASSWORD, WIFI_CANCEL_PASSWORD, WIFI_OPEN };
struct wifi_view wifi_ui_view(void);
void wifi_ui_action(enum wifi_action action, unsigned index);
void wifi_ui_join(const char *password);
#ifdef CONFIG_PICO_SENSE_LVGL_WIFI
#include <lvgl.h>
int wifi_ui_lvgl_init(void *buffer1, void *buffer2, unsigned size, lv_display_flush_cb_t flush);
void wifi_ui_lvgl_tick(bool changed);
void wifi_ui_lvgl_touch(const struct volume_touch *touch);
void wifi_ui_lvgl_cancel_touch(void);
lv_obj_t *wifi_ui_lvgl_card(void);
#endif
