/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usb.h"

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(demo_usb, CONFIG_USBD_LOG_LEVEL);

#define DEMO_USB_VID 0x2fe3
#define DEMO_USB_PID 0x0106

USBD_DEVICE_DEFINE(demo_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), DEMO_USB_VID,
		   DEMO_USB_PID);

USBD_DESC_LANG_DEFINE(demo_lang);
USBD_DESC_MANUFACTURER_DEFINE(demo_mfr, "Zephyr Project");
USBD_DESC_PRODUCT_DEFINE(demo_product, "RP2350 UAC+CDC");
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(demo_sn)));

USBD_DESC_CONFIG_DEFINE(demo_fs_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(demo_fs_config, 0, 125, &demo_fs_cfg_desc);

#if USBD_SUPPORTS_HIGH_SPEED
USBD_DESC_CONFIG_DEFINE(demo_hs_cfg_desc, "HS Configuration");
USBD_CONFIGURATION_DEFINE(demo_hs_config, 0, 125, &demo_hs_cfg_desc);
#endif

static void demo_fix_code_triple(struct usbd_context *uds_ctx, const enum usbd_speed speed)
{
	/* CDC ACM and UAC2 both use IADs. */
	usbd_device_set_code_triple(uds_ctx, speed, USB_BCC_MISCELLANEOUS, 0x02, 0x01);
}

static int demo_register_config(struct usbd_context *uds_ctx, const enum usbd_speed speed)
{
	int err;

#if USBD_SUPPORTS_HIGH_SPEED
	if (speed == USBD_SPEED_HS) {
		err = usbd_add_configuration(uds_ctx, USBD_SPEED_HS, &demo_hs_config);
	} else {
		err = usbd_add_configuration(uds_ctx, USBD_SPEED_FS, &demo_fs_config);
	}
#else
	ARG_UNUSED(speed);
	err = usbd_add_configuration(uds_ctx, USBD_SPEED_FS, &demo_fs_config);
#endif
	if (err) {
		LOG_ERR("Failed to add USB configuration (%d)", err);
		return err;
	}

	err = usbd_register_all_classes(uds_ctx, speed, 1, NULL);
	if (err) {
		LOG_ERR("Failed to register USB classes (%d)", err);
		return err;
	}

	demo_fix_code_triple(uds_ctx, speed);
	return 0;
}

static void demo_usbd_msg(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	ARG_UNUSED(ctx);

	if (msg->type == USBD_MSG_CONFIGURATION) {
		LOG_INF("USB configured");
	}
}

int demo_usb_init(void)
{
	int err;

	err = usbd_add_descriptor(&demo_usbd, &demo_lang);
	if (err) {
		return err;
	}

	err = usbd_add_descriptor(&demo_usbd, &demo_mfr);
	if (err) {
		return err;
	}

	err = usbd_add_descriptor(&demo_usbd, &demo_product);
	if (err) {
		return err;
	}

#if defined(CONFIG_HWINFO)
	err = usbd_add_descriptor(&demo_usbd, &demo_sn);
	if (err) {
		return err;
	}
#endif

#if USBD_SUPPORTS_HIGH_SPEED
	if (usbd_caps_speed(&demo_usbd) == USBD_SPEED_HS) {
		err = demo_register_config(&demo_usbd, USBD_SPEED_HS);
		if (err) {
			return err;
		}
	}
#endif

	err = demo_register_config(&demo_usbd, USBD_SPEED_FS);
	if (err) {
		return err;
	}

	err = usbd_msg_register_cb(&demo_usbd, demo_usbd_msg);
	if (err) {
		return err;
	}

	err = usbd_init(&demo_usbd);
	if (err) {
		LOG_ERR("usbd_init failed (%d)", err);
		return err;
	}

	err = usbd_enable(&demo_usbd);
	if (err) {
		LOG_ERR("usbd_enable failed (%d)", err);
		return err;
	}

	return 0;
}

