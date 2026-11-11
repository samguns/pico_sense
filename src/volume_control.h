/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define VOLUME_TOUCH_SLOTS 16
struct volume_contact { int x, y; bool down; };
struct volume_touch {
    struct volume_contact contacts[VOLUME_TOUCH_SLOTS];
    unsigned int session, count;
    bool multitouch;
};
struct volume_control {
    unsigned int session, percent;
    int slot, anchor_x, anchor_y, anchor_volume, last_y;
    bool tracking, adjusting, blocked, visible;
    int level_q8, alpha_q8;
    int64_t last_tick, last_adjustment;
};
struct volume_view { unsigned int percent, level_q8, alpha; };
void volume_touch_report(struct volume_touch *touch, unsigned int slot, int x, int y, bool down);
void volume_control_init(struct volume_control *control, unsigned int percent, int64_t now);
void volume_control_set(struct volume_control *control, unsigned int percent, int64_t now);
/* Returns true only when the actual speaker volume changes. Coordinates are landscape. */
bool volume_control_tick(struct volume_control *control, const struct volume_touch *touch,
                         unsigned int actual_percent, int64_t now, struct volume_view *view);
