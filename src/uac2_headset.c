/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB Audio 2 playback to I2S, with optional mono USB recording loopback.
 */

#include "uac2_headset.h"
#include "pcm_ring.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/class/usbd_uac2.h>

LOG_MODULE_REGISTER(demo_uac2, LOG_LEVEL_INF);

#define HEADPHONES_OUT_TERMINAL_ID UAC2_ENTITY_ID(DT_NODELABEL(out_terminal))
#define MICROPHONE_IN_TERMINAL_ID  UAC2_ENTITY_ID(DT_NODELABEL(in_terminal))

#define SAMPLES_PER_SOF   48
#define PLAY_BYTES_SLOT   (AUDIO_CHANNELS * AUDIO_SAMPLE_BYTES)
#define REC_BYTES_SLOT    (AUDIO_RECORD_CH * AUDIO_SAMPLE_BYTES)
#define REC_NOMINAL       (SAMPLES_PER_SOF * REC_BYTES_SLOT)
#define MAX_PLAY_BYTES    ((SAMPLES_PER_SOF + 1) * PLAY_BYTES_SLOT)
#define MAX_REC_BYTES     ((SAMPLES_PER_SOF + 1) * REC_BYTES_SLOT)
#define PLAY_SLAB_BLOCK   ROUND_UP(MAX_PLAY_BYTES, UDC_BUF_GRANULARITY)
#define REC_SLAB_BLOCK    ROUND_UP(MAX_REC_BYTES, UDC_BUF_GRANULARITY)
#define USB_BLOCKS        8
#define PLAY_TARGET       (PCM_RING_FRAMES / 2)

K_MEM_SLAB_DEFINE_STATIC(play_slab, PLAY_SLAB_BLOCK, USB_BLOCKS, UDC_BUF_ALIGN);
K_MEM_SLAB_DEFINE_STATIC(rec_slab, REC_SLAB_BLOCK, USB_BLOCKS, UDC_BUF_ALIGN);

struct uac2_ctx {
	bool playback_enabled;
	bool record_enabled;
	bool loopback;
	struct pcm_ring play;
	struct pcm_ring rec;
	bool play_primed;
	int32_t fill_average_q8;
};

static struct k_spinlock audio_lock;

static struct uac2_ctx uac2_ctx = {
	.loopback = true,
	.play = { .channels = AUDIO_CHANNELS },
	.rec = { .channels = AUDIO_RECORD_CH },
};

static void ring_push(struct pcm_ring *ring, const int16_t *src, size_t frames)
{
	k_spinlock_key_t key = k_spin_lock(&audio_lock);

	pcm_ring_push(ring, src, frames);
	k_spin_unlock(&audio_lock, key);
}

static size_t ring_pull(struct pcm_ring *ring, int16_t *dst, size_t frames)
{
	k_spinlock_key_t key = k_spin_lock(&audio_lock);
	size_t copied = pcm_ring_pull(ring, dst, frames);

	k_spin_unlock(&audio_lock, key);
	return copied;
}

static void uac2_terminal_update_cb(const struct device *dev, uint8_t terminal, bool enabled,
				    bool microframes, void *user_data)
{
	struct uac2_ctx *ctx = user_data;

	ARG_UNUSED(dev);
	ARG_UNUSED(microframes);

	if (terminal == HEADPHONES_OUT_TERMINAL_ID) {
		k_spinlock_key_t key = k_spin_lock(&audio_lock);

		ctx->playback_enabled = enabled;
		ctx->play.count = 0;
		ctx->play.write = 0;
		ctx->play_primed = false;
		ctx->fill_average_q8 = PLAY_TARGET * 256;
		k_spin_unlock(&audio_lock, key);
		LOG_INF("USB playback %s", enabled ? "on" : "off");
	} else if (terminal == MICROPHONE_IN_TERMINAL_ID) {
		ctx->record_enabled = enabled;
		LOG_INF("USB record %s", enabled ? "on" : "off");
	}
}

static void *uac2_get_recv_buf(const struct device *dev, uint8_t terminal, uint16_t size,
			       void *user_data)
{
	struct uac2_ctx *ctx = user_data;
	void *buf = NULL;

	ARG_UNUSED(dev);

	if (terminal != HEADPHONES_OUT_TERMINAL_ID || !ctx->playback_enabled ||
	    size > MAX_PLAY_BYTES) {
		return NULL;
	}

	if (k_mem_slab_alloc(&play_slab, &buf, K_NO_WAIT) != 0) {
		return NULL;
	}

	return buf;
}

static void uac2_data_recv_cb(const struct device *dev, uint8_t terminal, void *buf, uint16_t size,
			      void *user_data)
{
	struct uac2_ctx *ctx = user_data;
	size_t frames = size / PLAY_BYTES_SLOT;

	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);

	if (buf == NULL) {
		return;
	}

	if (frames > 0 && frames <= MAX_PLAY_BYTES / PLAY_BYTES_SLOT && ctx->playback_enabled) {
		const int16_t *stereo = buf;

		ring_push(&ctx->play, stereo, frames);
		if (audio_loopback_enabled()) {
			int16_t mono[SAMPLES_PER_SOF + 1];
			size_t n = MIN(frames, ARRAY_SIZE(mono));

			for (size_t i = 0; i < n; i++) {
				mono[i] = (int16_t)(((int32_t)stereo[2 * i] +
						     stereo[2 * i + 1]) / 2);
			}
			ring_push(&ctx->rec, mono, n);
		}
	}

	k_mem_slab_free(&play_slab, buf);
}

static void uac2_buf_release_cb(const struct device *dev, uint8_t terminal, void *buf,
				void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (terminal == MICROPHONE_IN_TERMINAL_ID && buf != NULL) {
		k_mem_slab_free(&rec_slab, buf);
	}
}

static void uac2_sof(const struct device *dev, void *user_data)
{
	struct uac2_ctx *ctx = user_data;
	int16_t *mic_buf = NULL;
	size_t got;

	if (!ctx->record_enabled) {
		return;
	}

	if (k_mem_slab_alloc(&rec_slab, (void **)&mic_buf, K_NO_WAIT) != 0) {
		return;
	}

	got = ring_pull(&ctx->rec, mic_buf, SAMPLES_PER_SOF);
	if (got < SAMPLES_PER_SOF) {
		memset(&mic_buf[got * AUDIO_RECORD_CH], 0,
		       (SAMPLES_PER_SOF - got) * REC_BYTES_SLOT);
	}

	if (usbd_uac2_send(dev, MICROPHONE_IN_TERMINAL_ID, mic_buf, REC_NOMINAL) < 0) {
		k_mem_slab_free(&rec_slab, mic_buf);
	}
}

static uint32_t uac2_feedback_cb(const struct device *dev, uint8_t terminal, void *user_data)
{
	struct uac2_ctx *ctx = user_data;
	k_spinlock_key_t key = k_spin_lock(&audio_lock);
	int32_t correction = 0;

	if (ctx->playback_enabled) {
		/* Smooth the 2 ms DMA block cadence; steer the host toward half-full. */
		ctx->fill_average_q8 +=
			((int32_t)ctx->play.count * 256 - ctx->fill_average_q8) / 16;
		correction = CLAMP((PLAY_TARGET * 256 - ctx->fill_average_q8) / 16,
				   -4096, 4096);
	}
	k_spin_unlock(&audio_lock, key);
	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);

	/* Full-Speed explicit feedback is Q10.14. Nominal 48 kHz = 48 samples/SOF. */
	return ((AUDIO_SAMPLE_RATE_HZ / 1000) << 14) + correction;
}

static uint32_t uac2_get_sample_rate(const struct device *dev, uint8_t clock_id, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(clock_id);
	ARG_UNUSED(user_data);

	return AUDIO_SAMPLE_RATE_HZ;
}

static int uac2_set_sample_rate(const struct device *dev, uint8_t clock_id, uint32_t rate,
				void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(clock_id);
	ARG_UNUSED(user_data);

	return rate == AUDIO_SAMPLE_RATE_HZ ? 0 : -EINVAL;
}

static struct uac2_ops usb_audio_ops = {
	.sof_cb = uac2_sof,
	.terminal_update_cb = uac2_terminal_update_cb,
	.get_recv_buf = uac2_get_recv_buf,
	.data_recv_cb = uac2_data_recv_cb,
	.buf_release_cb = uac2_buf_release_cb,
	.feedback_cb = uac2_feedback_cb,
	.get_sample_rate = uac2_get_sample_rate,
	.set_sample_rate = uac2_set_sample_rate,
};

int demo_uac2_init(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(uac2_headset));

	if (!device_is_ready(dev)) {
		LOG_ERR("UAC2 device not ready");
		return -ENODEV;
	}

	usbd_uac2_set_ops(dev, &usb_audio_ops, &uac2_ctx);
	return 0;
}

size_t audio_playback_pull(int16_t *dst, size_t frames)
{
	k_spinlock_key_t key = k_spin_lock(&audio_lock);
	size_t copied = 0;

	if (uac2_ctx.playback_enabled) {
		if (uac2_ctx.play.count >= PLAY_TARGET) {
			uac2_ctx.play_primed = true;
		}
		if (uac2_ctx.play_primed) {
			copied = pcm_ring_pull(&uac2_ctx.play, dst, frames);
			if (copied < frames) {
				uac2_ctx.play_primed = false;
			}
		}
	}
	k_spin_unlock(&audio_lock, key);
	return copied;
}

size_t audio_record_push(const int16_t *src, size_t frames)
{
	ring_push(&uac2_ctx.rec, src, frames);
	return frames;
}

void audio_set_loopback(bool enable)
{
	k_spinlock_key_t key = k_spin_lock(&audio_lock);

	uac2_ctx.loopback = enable;
	uac2_ctx.rec.count = 0;
	uac2_ctx.rec.write = 0;
	k_spin_unlock(&audio_lock, key);
}

bool audio_loopback_enabled(void)
{
	k_spinlock_key_t key = k_spin_lock(&audio_lock);
	bool enabled = uac2_ctx.loopback;

	k_spin_unlock(&audio_lock, key);
	return enabled;
}
