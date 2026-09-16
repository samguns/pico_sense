/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* Shared I2S clocks: MAX98357A on GP12, INMP441 on GP13. */
int audio_output_init(void);
void audio_output_test_tone(void);
void audio_output_set_volume(unsigned int percent);
