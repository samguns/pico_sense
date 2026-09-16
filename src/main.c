/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "uac2_headset.h"
#include "usb.h"
#include "audio_output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pico/bootrom.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

/* The console can use CDC ACM or a hardware UART for startup diagnostics. */
BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_display), sitronix_st7796s),
	     "Display is not ST7796S");
BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_touch), goodix_gt911),
	     "Touch is not GT911");

#define DISPLAY_WIDTH  DT_PROP(DT_CHOSEN(zephyr_display), width)
#define DISPLAY_HEIGHT DT_PROP(DT_CHOSEN(zephyr_display), height)
#define LINE_PIXELS    DISPLAY_WIDTH
#define RGB565_BPP     2

/*
 * Shared LCD/GT911 RESET is owned by the MIPI DBI node. Hold INT low before
 * that reset so GT911 latches I2C address 0x5D.
 */
static int gt911_hold_int_low(void)
{
	const struct gpio_dt_spec irq = GPIO_DT_SPEC_GET(DT_NODELABEL(gt911), irq_gpios);

	if (!gpio_is_ready_dt(&irq)) {
		return -ENODEV;
	}

	return gpio_pin_configure_dt(&irq, GPIO_OUTPUT_INACTIVE);
}

SYS_INIT(gt911_hold_int_low, POST_KERNEL, 40);

static void fill_rgb565(uint8_t *buf, size_t pixels, uint16_t color)
{
	/* ST7796 4-wire SPI is 8-bit: send the high RGB565 byte first. */
	for (size_t i = 0; i < pixels; i++) {
		buf[i * 2] = color >> 8;
		buf[i * 2 + 1] = color & 0xFF;
	}
}

static int draw_color_bars(const struct device *display)
{
	uint8_t line[LINE_PIXELS * RGB565_BPP];
	const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
	const size_t band = DISPLAY_WIDTH / ARRAY_SIZE(colors);
	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(line),
		.width = DISPLAY_WIDTH,
		.height = 1,
		.pitch = DISPLAY_WIDTH,
	};

	for (size_t x = 0; x < DISPLAY_WIDTH; x++) {
		size_t idx = MIN(x / band, ARRAY_SIZE(colors) - 1);

		fill_rgb565(&line[x * RGB565_BPP], 1, colors[idx]);
	}

	for (size_t y = 0; y < DISPLAY_HEIGHT; y++) {
		int ret = display_write(display, 0, y, &desc, line);

		if (ret < 0) {
			printf("demo: display_write failed at row %u (%d)\n",
			       (unsigned int)y, ret);
			return ret;
		}
	}

	return 0;
}

static void touch_event(struct input_event *evt, void *user_data)
{
	static int32_t slot;
	static int32_t x;
	static int32_t y;
	static bool pressed;

	ARG_UNUSED(user_data);

	switch (evt->code) {
	case INPUT_ABS_MT_SLOT:
		slot = evt->value;
		break;
	case INPUT_ABS_X:
		x = evt->value;
		break;
	case INPUT_ABS_Y:
		y = evt->value;
		break;
	case INPUT_BTN_TOUCH:
		pressed = evt->value;
		break;
	default:
		break;
	}

	/* Driver sets sync on SLOT and again on BTN_TOUCH; print only the complete report. */
	if (evt->sync && evt->code == INPUT_BTN_TOUCH) {
		printf("demo: touch slot=%d %s x=%d y=%d\n", slot, pressed ? "down" : "up", x, y);
	}
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_CHOSEN(zephyr_touch)), touch_event, NULL);

static FUNC_NORETURN void enter_usb_bootloader(void)
{
	uint32_t led_mask = 0;

#if DT_NODE_EXISTS(DT_ALIAS(led0))
	led_mask = BIT(DT_GPIO_PIN(DT_ALIAS(led0), gpios));
#endif

	printf("demo: entering USB bootloader\n");
	k_msleep(50);
	reset_usb_boot(led_mask, 0);
	CODE_UNREACHABLE;
}

static void console_monitor_poll(const struct device *console)
{
	static char line[16];
	static size_t len;
	unsigned char c;

	while (uart_poll_in(console, &c) == 0) {
		if (c == '\r' || c == '\n') {
			if (len == 0) {
				continue;
			}
			line[len] = '\0';
			printf("\n");
			if (strcmp(line, "boot") == 0) {
				enter_usb_bootloader();
			} else if (IS_ENABLED(CONFIG_PICO_SENSE_AUDIO) && strcmp(line, "tone") == 0) {
				audio_output_test_tone();
				printf("demo: 1 kHz test tone for one second\n");
				len = 0;
				continue;
			} else if (IS_ENABLED(CONFIG_PICO_SENSE_AUDIO) &&
				   strncmp(line, "vol ", 4) == 0) {
				char *end;
				long volume = strtol(line + 4, &end, 10);

				if (end != line + 4 && *end == '\0' && volume >= 0 && volume <= 100) {
					audio_output_set_volume(volume);
					printf("demo: speaker volume %ld%%\n", volume);
				} else {
					printf("demo: use vol 0..100\n");
				}
				len = 0;
				continue;
			} else if (strcmp(line, "loop") == 0) {
				audio_set_loopback(true);
				printf("demo: USB audio loopback on (ignore INMP441)\n");
				len = 0;
				continue;
			} else if (strcmp(line, "noloop") == 0) {
				audio_set_loopback(false);
				printf("demo: USB record from INMP441\n");
				len = 0;
				continue;
			}
			printf("demo: unknown command '%s' (boot, loop, noloop, tone, vol 0..100)\n", line);
			len = 0;
			continue;
		}

		if (c == '\b' || c == 0x7f) {
			if (len > 0) {
				len--;
				printf("\b \b");
			}
			continue;
		}

		if (c >= 32 && c < 127 && (len + 1) < sizeof(line)) {
			line[len++] = (char)c;
			uart_poll_out(console, c);
		}
	}
}

static void console_monitor_thread(void *p1, void *p2, void *p3)
{
	const struct device *console = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		console_monitor_poll(console);
		k_msleep(20);
	}
}

K_THREAD_STACK_DEFINE(console_monitor_stack, 1024);
static struct k_thread console_monitor_tid;

#ifndef PICO_SENSE_DIAGNOSTIC_NO_USB
static int demo_usb_boot(void)
{
	int err = demo_uac2_init();

	if (err) {
		printk("demo: UAC2 initialization failed (%d)\n", err);
		return err;
	}

	err = demo_usb_init();
	if (err) {
		printk("demo: USB initialization failed (%d)\n", err);
	}
	return err;
}

SYS_INIT(demo_usb_boot, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif

int main(void)
{
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	const struct device *touch = DEVICE_DT_GET(DT_CHOSEN(zephyr_touch));
	const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

#ifndef PICO_SENSE_DIAGNOSTIC_NO_USB
	if (IS_ENABLED(CONFIG_PICO_SENSE_AUDIO)) {
		int ret = audio_output_init();

		if (ret < 0) {
			printf("demo: I2S init failed (%d)\n", ret);
		} else {
			printf("demo: I2S speaker GP12 / mic GP13, volume 25%%\n");
		}
	}
#endif

	printf("demo: Hello RP2350\n");
#ifdef PICO_SENSE_DIAGNOSTIC_NO_USB
	printf("demo: display isolation test, USB startup disabled\n");
#else
	printf("demo: USB CDC + UAC2 (48 kHz 16-bit stereo playback, mono record)\n");
#endif
	printf("demo: commands: boot, loop, noloop, tone, vol 0..100\n");

	k_thread_create(&console_monitor_tid, console_monitor_stack,
			K_THREAD_STACK_SIZEOF(console_monitor_stack), console_monitor_thread,
			(void *)console, NULL, NULL, 7, 0, K_NO_WAIT);
	k_thread_name_set(&console_monitor_tid, "com_mon");

	if (!device_is_ready(display)) {
		printf("demo: display not ready\n");
		return 0;
	}

	(void)display_blanking_off(display);

	if (draw_color_bars(display) < 0) {
		printf("demo: failed to draw color bars\n");
		return 0;
	}

	printf("demo: color bars 320x480 (R/G/B/W)\n");

	if (!device_is_ready(touch)) {
		printf("demo: touch NOT ready (I2C/GT911 init failed)\n");
	} else {
		printf("demo: touch ready, tap with up to 5 fingers\n");
	}

#ifdef PICO_SENSE_DIAGNOSTIC_NO_USB
	const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

	if (!gpio_is_ready_dt(&led) || gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) < 0) {
		printk("demo: diagnostic LED unavailable\n");
		return 0;
	}
#endif

	while (1) {
#ifdef PICO_SENSE_DIAGNOSTIC_NO_USB
		gpio_pin_toggle_dt(&led);
		k_sleep(K_MSEC(500));
#else
		k_sleep(K_SECONDS(2));
#endif
		if (!device_is_ready(touch)) {
			printf("demo: touch still not ready\n");
		}
	}

	return 0;
}
