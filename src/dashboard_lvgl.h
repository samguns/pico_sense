/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
void dashboard_init(void);
int dashboard_flush(void);
#include "volume_control.h"
bool dashboard_touch(const struct volume_touch *touch);
int dashboard_robot_reply(const char *reply);
void dashboard_set_control_page(bool active);
void dashboard_cancel_touch(void);
