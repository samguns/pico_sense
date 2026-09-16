/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>

/*
 * 32-bit I2S slots, MSB first, one FIFO word per channel.
 * Side-set bit 0 is BCLK, bit 1 is LRCLK. LRCLK changes on the
 * previous channel's LSB, one bit before the next channel's MSB.
 * Each stereo frame is 128 PIO cycles (64 BCLK). Start at instruction 7.
 */
static const uint16_t i2s_instructions[] = {
	0x6001, /* out pins, 1      side 0 */
	0x0840, /* jmp x--, 0       side 1 */
	0x7001, /* out pins, 1      side 2 */
	0xf83e, /* set x, 30        side 3 */
	0x7001, /* out pins, 1      side 2 */
	0x1844, /* jmp x--, 4       side 3 */
	0x6001, /* out pins, 1      side 0 */
	0xe83e, /* set x, 30        side 1 */
};
