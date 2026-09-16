/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define AUDIO_SAMPLE_RATE_HZ 48000
#define AUDIO_CHANNELS       2
#define AUDIO_RECORD_CH      1
#define AUDIO_SAMPLE_BYTES   2

int demo_uac2_init(void);

/* Pull stereo frames for the speaker; returns fewer frames on underrun. */
size_t audio_playback_pull(int16_t *dst, size_t frames);

/* Push microphone samples into the USB record path. */
size_t audio_record_push(const int16_t *src, size_t frames);

/* When true, USB playback is downmixed into USB record (no microphone ADC). */
void audio_set_loopback(bool enable);
bool audio_loopback_enabled(void);
