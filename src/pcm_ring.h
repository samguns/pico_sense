/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define PCM_RING_FRAMES 384

/* Caller serializes access. Index and count are in frames, not samples. */
struct pcm_ring {
	int16_t samples[PCM_RING_FRAMES * 2];
	size_t write;
	size_t count;
	uint8_t channels;
};

static inline void pcm_ring_push(struct pcm_ring *ring, const int16_t *src, size_t frames)
{
	for (size_t f = 0; f < frames; f++) {
		for (size_t ch = 0; ch < ring->channels; ch++) {
			ring->samples[ring->write * ring->channels + ch] = *src++;
		}
		ring->write = (ring->write + 1) % PCM_RING_FRAMES;
		if (ring->count < PCM_RING_FRAMES) {
			ring->count++;
		}
	}
}

static inline size_t pcm_ring_pull(struct pcm_ring *ring, int16_t *dst, size_t frames)
{
	size_t copied = frames < ring->count ? frames : ring->count;
	size_t read = (ring->write + PCM_RING_FRAMES - ring->count) % PCM_RING_FRAMES;

	for (size_t f = 0; f < copied; f++) {
		for (size_t ch = 0; ch < ring->channels; ch++) {
			*dst++ = ring->samples[read * ring->channels + ch];
		}
		read = (read + 1) % PCM_RING_FRAMES;
	}
	ring->count -= copied;
	return copied;
}
