/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "uac2_headset.h"
#include "usb.h"
#include "audio_output.h"
#include "battery_display.h"
#include "volume_control.h"
#include "wifi_ui.h"
#ifdef CONFIG_PICO_SENSE_LVGL_WIFI
#include "dashboard_lvgl.h"
#include "ui_cards.h"
#endif

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

static struct k_spinlock touch_lock;
static struct volume_touch touch_state;
static unsigned int press_sequence;
static int64_t last_touch_report;
void pico_touch_diagnostics(uint32_t *,uint32_t *,uint32_t *,int *);
static int press_x, press_y;
static unsigned press_slot;
/* Controller and renderer are owned only by the console thread. */
static struct volume_control speaker_control;

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

	/* Driver sets sync on SLOT and again on BTN_TOUCH. Coalesce complete reports
	 * without drawing or blocking the input thread on SPI/console output. */
	if (evt->sync && evt->code == INPUT_BTN_TOUCH) {
		k_spinlock_key_t key = k_spin_lock(&touch_lock);
        last_touch_report=k_uptime_get();
		/* Inverted edge coordinates can equal the configured screen extent. */
		if (pressed && !touch_state.count) {
			press_sequence++;
            press_slot=slot;
			press_x = CLAMP(x, 0, 479);
			press_y = CLAMP(y, 0, 319);
		}
		volume_touch_report(&touch_state, slot, CLAMP(x, 0, 479), CLAMP(y, 0, 319), pressed);
		k_spin_unlock(&touch_lock, key);
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
	static char line[1024];
	static bool overflow;
	static size_t len;
	unsigned char c;

	while (uart_poll_in(console, &c) == 0) {
		if (c == '\r' || c == '\n') {
			if (overflow) {
				printf("ERR command too long\n");
				len = 0;
				overflow = false;
				continue;
			}
			if (len == 0) {
				continue;
			}
			line[len] = '\0';
			printf("\n");
			if (strcmp(line, "boot") == 0) {
				enter_usb_bootloader();
			} else if (strcmp(line, "touchstatus") == 0) {
                struct volume_touch touch;
                k_spinlock_key_t key=k_spin_lock(&touch_lock);
                touch=touch_state;int64_t age=k_uptime_get()-last_touch_report;
                k_spin_unlock(&touch_lock,key);
                uint32_t polls,errors,frames;int error;
                pico_touch_diagnostics(&polls,&errors,&frames,&error);
                printf("OK touch polls=%u frames=%u errors=%u last_error=%d age_ms=%lld count=%u session=%u multitouch=%u blocked=%u tracking=%u adjusting=%u alpha=%u\n",
                    polls,frames,errors,error,(long long)age,touch.count,touch.session,
                    touch.multitouch,speaker_control.blocked,speaker_control.tracking,
                    speaker_control.adjusting,speaker_control.alpha_q8/256);
                len=0;continue;
            }
#ifdef CONFIG_PICO_SENSE_LVGL_WIFI
            else if (strncmp(line, "robot ", 6) == 0) {
                int ret=dashboard_robot_reply(line+6);
                printf(ret<0 ? "ERR robot\n" : "OK robot\n");
                len=0;continue;
            }
#endif
            else if (strncmp(line, "wifi ", 5) == 0) {
                int ret = wifi_ui_command(line + 5);
                printf(ret < 0 ? "ERR wifi\n" : "OK wifi\n");
                len = 0; continue;
            } else if (strncmp(line, "listen ", 7) == 0) {
                unsigned long long id;
                unsigned int phase;
                char extra;
                int ret = -EINVAL;
                if (sscanf(line + 7, "%llu %u %c", &id, &phase, &extra) == 2)
                    ret = battery_display_listening(id, phase);
                if (ret < 0) printf("ERR listen %d\n", ret);
                else printf("OK listen %llu %u\n", id, phase);
                len = 0;
                continue;
            } else if (strncmp(line, "talk ", 5) == 0) {
                char *end;
                unsigned long long id = strtoull(line + 5, &end, 10);
                int ret = -EINVAL;
                if (end > line + 5 && end[0] == ' ' &&
                    (end[1] == 'w' || end[1] == 'g') && end[2] == ' ')
                    ret = battery_display_text(id, end[1] == 'g', end + 3);
                if (ret < 0) printf("ERR talk %d\n", ret);
                else printf("OK talk %llu\n", id);
                len = 0;
                continue;
            } else if (strncmp(line, "row ", 4) == 0) {
                int ret = -EINVAL;
                if (line[4] >= '0' && line[4] <= '5' && line[5] == ' ')
                    ret = battery_display_row(line[4] - '0', line + 6);
                if (ret < 0) printf("ERR row %d\n", ret);
                else printf("OK row %c\n", line[4]);
                len = 0;
                continue;
            } else if (strcmp(line, "voltage unavailable") == 0) {
                int ret = battery_display_unavailable();
                if (ret < 0) printf("ERR display %d\n", ret);
                else printf("OK voltage unavailable\n");
                len = 0;
                continue;
			} else if (strncmp(line, "voltage ", 8) == 0) {
				unsigned int value;
				if (battery_voltage_parse(line + 8, &value) < 0) {
					printf("ERR voltage: use voltage 0..99.99\n");
				} else {
					int ret = battery_display_update(value);
					if (ret < 0) {
						printf("ERR display %d\n", ret);
					} else {
						printf("OK voltage %u.%02u\n", value / 100, value % 100);
					}
				}
				len = 0;
				continue;
			} else if (IS_ENABLED(CONFIG_PICO_SENSE_AUDIO) && strcmp(line, "tone") == 0) {
				audio_output_test_tone();
				printf("demo: 1 kHz test tone for one second\n");
				len = 0;
				continue;
			} else if (IS_ENABLED(CONFIG_PICO_SENSE_AUDIO) && strcmp(line, "vol") == 0) {
				printf("OK vol %u\n", audio_output_get_volume());
				len = 0;
				continue;
			} else if (IS_ENABLED(CONFIG_PICO_SENSE_AUDIO) &&
				   strncmp(line, "vol ", 4) == 0) {
				char *end;
				long volume = strtol(line + 4, &end, 10);

				if (end != line + 4 && *end == '\0' && volume >= 0 && volume <= 100) {
					audio_output_set_volume(volume);
					volume_control_set(&speaker_control, volume, k_uptime_get());
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
			printf("demo: unknown command '%s' (boot, loop, noloop, tone, vol 0..100, voltage 0..99.99)\n", line);
			len = 0;
			continue;
		}

		if (overflow) {
			continue;
		}

		if (c == '\b' || c == 0x7f) {
			if (len > 0) {
				len--;
				printf("\b \b");
			}
			continue;
		}

		/* Preserve UTF-8 continuation and leading bytes. Filtering to ASCII
		 * silently emptied Chinese talk commands before font lookup. DEL and
		 * terminal controls are handled above or ignored below. */
		if (c >= 32 && c != 127) {
			if (len + 1 >= sizeof(line)) {
				overflow = true;
				continue;
			}
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
	volume_control_init(&speaker_control,
		IS_ENABLED(CONFIG_PICO_SENSE_AUDIO) ? audio_output_get_volume() : 0, k_uptime_get());

    unsigned int consumed_press = 0;
	while (1) {
		console_monitor_poll(console);
        if (IS_ENABLED(CONFIG_PICO_SENSE_AUDIO)) {
            struct volume_touch snapshot;
            k_spinlock_key_t key = k_spin_lock(&touch_lock);
            snapshot = touch_state;
            unsigned int sequence = press_sequence;
            int x = press_x, y = press_y;
            unsigned first_slot=press_slot;
            k_spin_unlock(&touch_lock, key);
            /* Preserve a short tap that ended between console-thread ticks. */
            bool wifi_consumed = false;
            if (sequence != consumed_press && !snapshot.count && !snapshot.multitouch) {
                struct volume_touch press = snapshot;
                press.count = 1;
                if(first_slot<VOLUME_TOUCH_SLOTS)press.contacts[first_slot] = (struct volume_contact){x, y, true};
#ifdef CONFIG_PICO_SENSE_LVGL_WIFI
                wifi_consumed = ui_cards_touch(&press);
#endif
#ifndef CONFIG_PICO_SENSE_LVGL_WIFI
                if(!wifi_consumed)wifi_consumed = wifi_ui_touch(&press);
#endif
            }
#ifdef CONFIG_PICO_SENSE_LVGL_WIFI
            wifi_consumed=ui_cards_touch(&snapshot) || wifi_consumed;
#else
            wifi_consumed = wifi_ui_touch(&snapshot) || wifi_consumed;
            if (wifi_consumed) { speaker_control.session=snapshot.session; speaker_control.blocked=true; }
            if (sequence != consumed_press) {
                consumed_press = sequence;
                uint64_t id;
                if (!wifi_consumed && battery_display_listening_hit(x, y, &id)) {
                    speaker_control.session = snapshot.session;
                    speaker_control.blocked = true;
                    /* One event per new finger-down. A later tap can retry if
                     * its predecessor failed to reach the speech service. */
                    printf("\nEVENT quit %llu\n", (unsigned long long)id);
                }
            }
#endif
            consumed_press=sequence;
            struct volume_view view;
            struct volume_touch volume_touch=snapshot;
            /* Deferred taps and page gestures never enter volume recognition.
             * A zero-contact sample releases its latch before a vertical drag. */
            if(wifi_consumed){volume_touch.count=0;for(unsigned i=0;i<VOLUME_TOUCH_SLOTS;i++)volume_touch.contacts[i].down=false;}
            if (volume_control_tick(&speaker_control, &volume_touch, audio_output_get_volume(),
                                    k_uptime_get(), &view))
                audio_output_set_volume(view.percent);
            battery_display_volume(view.percent, view.level_q8, view.alpha);
        }
        battery_display_tick();
		k_msleep(20);
	}
}

#ifdef CONFIG_PICO_SENSE_LVGL_WIFI
K_THREAD_STACK_DEFINE(console_monitor_stack, 6144);
#else
K_THREAD_STACK_DEFINE(console_monitor_stack, 3072);
#endif
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
	printf("demo: commands: boot, loop, noloop, tone, vol 0..100, voltage 0..99.99\n");

	int display_ret = battery_display_init(display);
	if (display_ret < 0) {
		printf("demo: battery display init failed (%d)\n", display_ret);
	} else {
		printf("demo: battery display 480x320 ready\n");
	}

	k_thread_create(&console_monitor_tid, console_monitor_stack,
			K_THREAD_STACK_SIZEOF(console_monitor_stack), console_monitor_thread,
			(void *)console, NULL, NULL, 7, 0, K_NO_WAIT);
	k_thread_name_set(&console_monitor_tid, "com_mon");

	if (!device_is_ready(touch)) {
		printf("demo: touch NOT ready (I2C/GT911 init failed)\n");
	} else {
		printf("demo: touch ready; slide one finger up/down on the right half for volume\n");
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
