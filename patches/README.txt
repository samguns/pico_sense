RP2350 USB Start-of-Frame interrupt fix

0001-udc-rpi-pico-ack-sof.patch applies to the Zephyr checkout, not this
application. UAC2 enables CONFIG_UDC_ENABLE_SOF. The RP2350 requires a read
of SOF_RD to acknowledge DEV_SOF; failing to do so can leave the CPU
repeatedly servicing the interrupt and prevent USB enumeration and drawing
from completing.

From the project root, with ZEPHYR_BASE pointing to your Zephyr checkout,
apply once before building with a checkout missing this fix:

git -C "$ZEPHYR_BASE" apply --check "$PWD/patches/0001-udc-rpi-pico-ack-sof.patch"
git -C "$ZEPHYR_BASE" apply "$PWD/patches/0001-udc-rpi-pico-ack-sof.patch"

To check whether this exact patch is already applied:

git -C "$ZEPHYR_BASE" apply --reverse --check "$PWD/patches/0001-udc-rpi-pico-ack-sof.patch"

If that check succeeds, skip applying it again. Keep the fix when updating
Zephyr until the driver acknowledges DEV_SOF upstream. If neither check
succeeds, inspect the driver before applying a patch to a different version.

Reference: the udc_rpi_pico.c driver in the 3.5inch-touchscreen workspace.
Only its SOF acknowledgement fix is carried here; unrelated driver differences
are omitted.
