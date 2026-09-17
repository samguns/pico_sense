/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BATTERY_DISPLAY_H
#define BATTERY_DISPLAY_H
#include <zephyr/device.h>
int battery_display_init(const struct device *display);
int battery_display_update(unsigned int centivolts);
int battery_voltage_parse(const char *text, unsigned int *centivolts);
#endif
