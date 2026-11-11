/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <lvgl.h>
#include "volume_control.h"
enum ui_page { UI_ROBOT, UI_HOME, UI_WIFI };
void ui_cards_init(lv_obj_t *canvas,lv_obj_t *robot,lv_obj_t *home,lv_obj_t *wifi);
bool ui_cards_touch(const struct volume_touch *touch);
void ui_cards_tick(void);
void ui_cards_go(enum ui_page page);
enum ui_page ui_cards_page(void);
bool ui_cards_moving(void);
