RP2350 USB audio driver fixes
============================

0001-udc-rpi-pico-audio-fixes.patch applies to the Zephyr checkout, not this
application. It combines two fixes in drivers/usb/udc/udc_rpi_pico.c:

1. Acknowledge DEV_SOF by reading SOF_RD. Without this, UAC2 can leave the
   CPU repeatedly servicing the interrupt, blocking enumeration and drawing.
2. Log a busy endpoint during a new-transfer event at debug level. UAC2
   normally queues buffers while a transfer is active; completion starts the
   next buffer. The completion-path busy error and other USB errors remain.
   CONFIG_UDC_DRIVER_LOG_LEVEL_WRN suppresses the normal queueing message.
   "messages dropped" counts lost log messages, not lost audio packets.

From the project root, with ZEPHYR_BASE pointing to the checkout used by your
build, apply once to a checkout missing both fixes:

git -C "$ZEPHYR_BASE" apply --check "$PWD/patches/0001-udc-rpi-pico-audio-fixes.patch"
git -C "$ZEPHYR_BASE" apply "$PWD/patches/0001-udc-rpi-pico-audio-fixes.patch"

To check whether both fixes are already applied:

git -C "$ZEPHYR_BASE" apply --reverse --check "$PWD/patches/0001-udc-rpi-pico-audio-fixes.patch"

In PowerShell, use $env:ZEPHYR_BASE instead of $ZEPHYR_BASE.
If the reverse check succeeds, skip applying the patch. This also covers
checkouts where both former individual patches were applied; no migration
or reapplication is needed.

If only one former fix is present, neither whole-patch check will succeed.
Inspect the driver and apply only the missing hunk; do not force the whole
patch or discard other local driver changes. The same applies when updating
to a Zephyr version that already includes one of the fixes upstream.

Rebuild and flash after changing driver code. Combining patch files alone
does not change firmware already built with both fixes.
