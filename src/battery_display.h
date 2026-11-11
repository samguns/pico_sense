/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BATTERY_DISPLAY_H
#define BATTERY_DISPLAY_H
#include <zephyr/device.h>
#include <stdbool.h>
#include <stdint.h>
int battery_display_init(const struct device *display);
int battery_display_update(unsigned int centivolts);
int battery_display_unavailable(void);
int battery_display_row(unsigned int row, const char *text);
int battery_display_text(uint64_t id, bool spoken, const char *text);
void battery_display_tick(void);
int battery_display_listening(uint64_t id, unsigned int phase);
bool battery_display_listening_hit(int x, int y, uint64_t *id);
int battery_display_volume(unsigned int percent, unsigned int level_q8, unsigned int alpha);
int battery_voltage_parse(const char *text, unsigned int *centivolts);
#endif
