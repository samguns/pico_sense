/* SPDX-License-Identifier: Apache-2.0 */
#include "audio_output.h"
#include "i2s_program.h"
#include "uac2_headset.h"

#include <hardware/dma.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#define BCLK DT_PROP(DT_PATH(zephyr_user), i2s_bclk_pin)
#define LRCLK DT_PROP(DT_PATH(zephyr_user), i2s_lrclk_pin)
#define DATA DT_PROP(DT_PATH(zephyr_user), i2s_data_pin)
#define BLOCK_FRAMES 96

BUILD_ASSERT(LRCLK == BCLK + 1, "PIO side-set requires adjacent BCLK/LRCLK pins");
BUILD_ASSERT(!IS_ENABLED(CONFIG_DMA), "Audio owns DMA IRQ0; do not enable the Zephyr DMA driver");

static const struct device *const pio_dev = DEVICE_DT_GET(DT_ALIAS(audio_pio));
static PIO audio_pio;
static size_t audio_sm;
static int channels[2];
static uint32_t buffers[2][BLOCK_FRAMES];
static atomic_t volume = ATOMIC_INIT(25);
static atomic_t tone_request;
static unsigned int tone_left;
static unsigned int tone_phase;

/* 1 kHz sine at 48 kHz, about -18 dBFS before the volume control. */
static const int16_t tone[48] = {
	0, 535, 1060, 1567, 2048, 2493, 2896, 3250, 3547, 3784, 3956, 4061,
	4096, 4061, 3956, 3784, 3547, 3250, 2896, 2493, 2048, 1567, 1060, 535,
	0, -535, -1060, -1567, -2048, -2493, -2896, -3250, -3547, -3784, -3956, -4061,
	-4096, -4061, -3956, -3784, -3547, -3250, -2896, -2493, -2048, -1567, -1060, -535,
};

static void fill_buffer(unsigned int index)
{
	int16_t pcm[BLOCK_FRAMES * AUDIO_CHANNELS];
	size_t got = audio_playback_pull(pcm, BLOCK_FRAMES);
	unsigned int gain = atomic_get(&volume);

	if (atomic_set(&tone_request, 0)) {
		tone_left = AUDIO_SAMPLE_RATE_HZ;
		tone_phase = 0;
	}

	for (size_t i = 0; i < BLOCK_FRAMES; i++) {
		int32_t sample = 0;

		if (tone_left > 0) {
			sample = tone[tone_phase];
			tone_phase = (tone_phase + 1) % ARRAY_SIZE(tone);
			tone_left--;
		} else if (i < got) {
			sample = ((int32_t)pcm[2 * i] + pcm[2 * i + 1]) / 2;
		}
		sample = sample * (int32_t)gain / 100;
		/* Duplicate the mono mix so either amplifier channel selection works. */
		uint32_t word = (uint16_t)sample;

		buffers[index][i] = (word << 16) | word;
	}
}

static void audio_dma_isr(const void *arg)
{
	ARG_UNUSED(arg);
	for (unsigned int i = 0; i < 2; i++) {
		unsigned int channel = channels[i];

		if (!dma_channel_get_irq0_status(channel)) {
			continue;
		}
		dma_channel_acknowledge_irq0(channel);
		/* The other DMA channel is already running via hardware chaining. */
		fill_buffer(i);
		dma_channel_set_read_addr(channel, buffers[i], false);
		dma_channel_set_trans_count(channel, dma_encode_transfer_count(BLOCK_FRAMES), false);
	}
}

int audio_output_init(void)
{
	const struct reset_dt_spec dma_reset = RESET_DT_SPEC_GET(DT_NODELABEL(dma));
	const struct device *clock = DEVICE_DT_GET(DT_CLOCKS_CTLR(DT_ALIAS(audio_pio)));
	clock_control_subsys_t clock_id =
		(clock_control_subsys_t)DT_CLOCKS_CELL(DT_ALIAS(audio_pio), clk_id);
	const struct pio_program program = {
		.instructions = i2s_instructions,
		.length = ARRAY_SIZE(i2s_instructions),
		.origin = -1,
	};
	uint32_t clock_hz;
	int ret;

	if (!device_is_ready(pio_dev) || !device_is_ready(clock)) {
		return -ENODEV;
	}
	ret = clock_control_get_rate(clock, clock_id, &clock_hz);
	if (ret < 0) {
		return ret;
	}
	ret = reset_line_toggle_dt(&dma_reset);
	if (ret < 0) {
		return ret;
	}
	audio_pio = pio_rpi_pico_get_pio(pio_dev);
	if (!pio_can_add_program(audio_pio, &program)) {
		return -ENOMEM;
	}
	ret = pio_rpi_pico_allocate_sm(pio_dev, &audio_sm);
	if (ret < 0) {
		return ret;
	}
	channels[0] = dma_claim_unused_channel(false);
	channels[1] = dma_claim_unused_channel(false);
	if (channels[0] < 0 || channels[1] < 0) {
		for (unsigned int i = 0; i < 2; i++) {
			if (channels[i] >= 0) {
				dma_channel_unclaim(channels[i]);
			}
		}
		pio_sm_unclaim(audio_pio, audio_sm);
		return -EBUSY;
	}

	unsigned int offset = pio_add_program(audio_pio, &program);
	pio_sm_config cfg = pio_get_default_sm_config();
	uint32_t divider = ((uint64_t)clock_hz * 256 + AUDIO_SAMPLE_RATE_HZ * 32) /
			   (AUDIO_SAMPLE_RATE_HZ * 64);

	sm_config_set_wrap(&cfg, offset, offset + ARRAY_SIZE(i2s_instructions) - 1);
	sm_config_set_sideset(&cfg, 2, false, false);
	sm_config_set_sideset_pins(&cfg, BCLK);
	sm_config_set_out_pins(&cfg, DATA, 1);
	sm_config_set_out_shift(&cfg, false, true, 32);
	sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_TX);
	sm_config_set_clkdiv_int_frac(&cfg, divider >> 8, divider & 0xff);
	pio_sm_init(audio_pio, audio_sm, offset + 7, &cfg);
	pio_sm_set_pins_with_mask(audio_pio, audio_sm, 0, BIT(BCLK) | BIT(LRCLK) | BIT(DATA));
	pio_sm_set_consecutive_pindirs(audio_pio, audio_sm, BCLK, 2, true);
	pio_sm_set_consecutive_pindirs(audio_pio, audio_sm, DATA, 1, true);
	pio_gpio_init(audio_pio, BCLK);
	pio_gpio_init(audio_pio, LRCLK);
	pio_gpio_init(audio_pio, DATA);

	for (unsigned int i = 0; i < 2; i++) {
		dma_channel_config dma_cfg = dma_channel_get_default_config(channels[i]);

		channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_32);
		channel_config_set_read_increment(&dma_cfg, true);
		channel_config_set_write_increment(&dma_cfg, false);
		channel_config_set_dreq(&dma_cfg, pio_get_dreq(audio_pio, audio_sm, true));
		channel_config_set_chain_to(&dma_cfg, channels[1 - i]);
		dma_channel_configure(channels[i], &dma_cfg, &audio_pio->txf[audio_sm],
				      buffers[i], dma_encode_transfer_count(BLOCK_FRAMES), false);
		dma_channel_acknowledge_irq0(channels[i]);
		dma_channel_set_irq0_enabled(channels[i], true);
	}
	IRQ_CONNECT(DT_IRQ_BY_IDX(DT_NODELABEL(dma), 0, irq), 2, audio_dma_isr, NULL, 0);
	irq_enable(DT_IRQ_BY_IDX(DT_NODELABEL(dma), 0, irq));
	dma_start_channel_mask(BIT(channels[0]));
	pio_sm_set_enabled(audio_pio, audio_sm, true);
	return 0;
}

void audio_output_test_tone(void)
{
	atomic_set(&tone_request, 1);
}

void audio_output_set_volume(unsigned int percent)
{
	atomic_set(&volume, MIN(percent, 100));
}
