/* SPDX-License-Identifier: Apache-2.0 */
#include "battery_display.h"
#include <stdio.h>
#include <string.h>
#include <zephyr/drivers/display.h>
#include <zephyr/sys/util.h>

#define WIDTH 480
#define HEIGHT 320
#define BG 0x0841
#define WHITE 0xef7d
#define CYAN 0x07ff

/* Original 5x7 bitmap glyphs, one column per byte, top pixel in bit zero. */
static const char alphabet[] = "0123456789.- ABCDEGILMNORTVXY";
static const uint8_t glyphs[][5] = {
	{62,81,73,69,62}, {0,66,127,64,0}, {98,81,73,73,70},
	{34,65,73,73,54}, {24,20,18,127,16}, {39,69,69,69,57},
	{60,74,73,73,48}, {1,113,9,5,3}, {54,73,73,73,54},
	{6,73,73,41,30}, {0,96,96,0,0}, {8,8,8,8,8}, {0,0,0,0,0},
	{126,9,9,9,126}, {127,73,73,73,54}, {62,65,65,65,34},
	{127,65,65,34,28}, {127,73,73,73,65}, {62,65,73,73,58},
	{0,65,127,65,0}, {127,64,64,64,64}, {127,2,12,2,127},
	{127,4,8,16,127}, {62,65,65,65,62}, {127,9,25,41,70},
	{1,1,127,1,1}, {31,32,64,32,31}, {99,20,8,20,99},
	{3,4,120,4,3},
};
BUILD_ASSERT(ARRAY_SIZE(glyphs) == sizeof(alphabet) - 1);
static const struct device *lcd;
static uint8_t line[WIDTH * 2];

static void pixel(int x, uint16_t color)
{
	line[x * 2] = color >> 8;
	line[x * 2 + 1] = color & 255;
}

/* Render complete opaque rows to avoid clearing flashes and a full framebuffer. */
static void text_row(const char *text, int center, int top, int scale,
		     int y, uint16_t color)
{
	if (y < top || y >= top + 7 * scale) {
		return;
	}
	int left = center - ((int)strlen(text) * 6 - 1) * scale / 2;
	for (int i = 0; text[i]; i++) {
		const char *g = strchr(alphabet, text[i]);
		if (!g) {
			continue;
		}
		for (int col = 0; col < 5; col++) {
			if (!(glyphs[g - alphabet][col] & BIT((y - top) / scale))) {
				continue;
			}
			for (int dx = 0; dx < scale; dx++) {
				int x = left + (i * 6 + col) * scale + dx;
				if (x >= 0 && x < WIDTH) {
					pixel(x, color);
				}
			}
		}
	}
}

static void format_voltage(char *buf, size_t size, unsigned int value)
{
	snprintf(buf, size, "%02u.%02u V", value / 100, value % 100);
}

static int render(bool full, unsigned int value)
{
	char current[16] = "--.-- V";
	if (!full) {
		format_voltage(current, sizeof(current), value);
	}
	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(line), .width = WIDTH, .height = 1, .pitch = WIDTH,
	};
	for (int y = 0; y < HEIGHT; y++) {
		if (!full && !(y >= 86 && y < 156)) {
			continue;
		}
		for (int x = 0; x < WIDTH; x++) {
			pixel(x, (y == 222 && x >= 28 && x < 452) ? 0x4228 : BG);
		}
		text_row("BATTERY", 240, 25, 3, y, WHITE);
		text_row(current, 240, 86, 10, y, CYAN);
		text_row("DC VOLTAGE", 240, 183, 2, y, WHITE);
		text_row("MIN", 120, 238, 2, y, WHITE);
		text_row("MAX", 360, 238, 2, y, WHITE);
		text_row("0 V", 120, 264, 3, y, WHITE);
		text_row("28 V", 360, 264, 3, y, WHITE);
		int ret = display_write(lcd, 0, y, &desc, line);
		if (ret < 0) {
			return ret;
		}
	}
	return 0;
}

int battery_display_init(const struct device *display)
{
	struct display_capabilities caps;
	if (!device_is_ready(display)) {
		return -ENODEV;
	}
	display_get_capabilities(display, &caps);
	if (caps.x_resolution != WIDTH || caps.y_resolution != HEIGHT) {
		return -EINVAL;
	}
	lcd = display;
	int ret = render(true, 0);
	if (ret == 0) {
		ret = display_blanking_off(lcd);
	}
	if (ret < 0) {
		lcd = NULL;
	}
	return ret;
}

int battery_display_update(unsigned int centivolts)
{
	if (!lcd) {
		return -ENODEV;
	}
	return render(false, centivolts);
}
