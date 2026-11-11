/* SPDX-License-Identifier: Apache-2.0 */
#include "volume_control.h"
#include <stdlib.h>
#include <string.h>

static int clamp(int value, int lo, int hi) { return value < lo ? lo : value > hi ? hi : value; }

void volume_touch_report(struct volume_touch *touch, unsigned int slot, int x, int y, bool down)
{
    if (slot >= VOLUME_TOUCH_SLOTS) return;
    if (down && !touch->contacts[slot].down) {
        if (!touch->count) { touch->session++; touch->multitouch = false; }
        touch->count++;
    } else if (!down && touch->contacts[slot].down) {
        touch->count--;
    }
    touch->contacts[slot] = (struct volume_contact){x, y, down};
    if (touch->count > 1) touch->multitouch = true;
}

void volume_control_init(struct volume_control *control, unsigned int percent, int64_t now)
{
    memset(control, 0, sizeof(*control));
    control->percent = clamp(percent, 0, 100);
    control->level_q8 = control->percent * 256;
    control->last_tick = now;
}

void volume_control_set(struct volume_control *control, unsigned int percent, int64_t now)
{
    control->percent = clamp(percent, 0, 100);
    control->last_adjustment = now;
    control->visible = true;
    /* A software change during a drag must not get overwritten by its old anchor. */
    control->tracking = false;
}

bool volume_control_tick(struct volume_control *control, const struct volume_touch *touch,
                         unsigned int actual_percent, int64_t now, struct volume_view *view)
{
    int elapsed = clamp(now - control->last_tick, 0, 100);
    control->last_tick = now;
    unsigned int before = clamp(actual_percent, 0, 100);
    control->percent = before;
    if (touch->session != control->session) {
        control->session = touch->session;
        control->tracking = control->adjusting = control->blocked = false;
    }
    if (!touch->count) {
        /* Start the idle timeout on release, even after a long stationary hold. */
        if (control->adjusting) control->last_adjustment = now;
        control->tracking = control->adjusting = control->blocked = false;
    } else if (touch->count != 1 || touch->multitouch) {
        control->tracking = control->adjusting = false;
        control->blocked = true;
    } else if (!control->blocked) {
        int slot = 0;
        while (slot < VOLUME_TOUCH_SLOTS && !touch->contacts[slot].down) slot++;
        if (slot < VOLUME_TOUCH_SLOTS) {
            int x = touch->contacts[slot].x, y = touch->contacts[slot].y;
            if (x < 240 || x >= 480 || y < 0 || y >= 320) {
                control->tracking = control->adjusting = false;
                control->blocked = true;
            } else if (!control->tracking) {
                control->tracking = true;
                control->slot = slot;
                control->anchor_x = x;
                control->anchor_y = control->last_y = y;
                control->anchor_volume = before;
            } else if (slot != control->slot) {
                control->blocked = true;
                control->tracking = control->adjusting = false;
            } else {
                int dy = control->anchor_y - y;
                if (!control->adjusting && abs(dy) >= 6 && abs(dy) >= abs(x - control->anchor_x))
                    control->adjusting = true;
                if (control->adjusting && y != control->last_y) {
                    int delta = dy * 100;
                    int percent = control->anchor_volume + (delta + (delta < 0 ? -120 : 120)) / 240;
                    control->percent = clamp(percent, 0, 100);
                    control->visible = true;
                    control->last_adjustment = now;
                    control->last_y = y;
                    /* Reverse immediately at the end stops, without a dead travel zone. */
                    if (percent <= 0 || percent >= 100) {
                        control->anchor_y = y;
                        control->anchor_volume = control->percent;
                    }
                }
            }
        }
    }
    /* A recognised drag remains active while the finger rests. Only movement
     * changes volume; holding keeps the HUD visible and preserves its anchor. */
    if (control->tracking && control->adjusting && !control->blocked)
        control->last_adjustment = now;
    int target = control->percent * 256;
    if (elapsed) {
        /* Time-based easing stays consistent when LCD transfer time varies. */
        int difference = target - control->level_q8;
        control->level_q8 += difference * elapsed / (60 + elapsed);
        if (abs(target - control->level_q8) < 32) control->level_q8 = target;
        int alpha_target = control->visible && now - control->last_adjustment < 900 ? 255 * 256 : 0;
        int step = elapsed * 255 * 256 / (alpha_target ? 120 : 260);
        control->alpha_q8 += clamp(alpha_target - control->alpha_q8, -step, step);
        if (!control->alpha_q8 && !alpha_target) control->visible = false;
    }
    *view = (struct volume_view){control->percent, control->level_q8, control->alpha_q8 / 256};
    return control->percent != before;
}
