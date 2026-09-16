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
#include <zephyr/sys/util.h>

#define BCLK    DT_PROP(DT_PATH(zephyr_user), i2s_bclk_pin)
#define LRCLK   DT_PROP(DT_PATH(zephyr_user), i2s_lrclk_pin)
#define DATA    DT_PROP(DT_PATH(zephyr_user), i2s_data_pin)
#define DATA_IN DT_PROP(DT_PATH(zephyr_user), i2s_data_in_pin)

#define BLOCK_FRAMES     96
#define TX_WORDS         (BLOCK_FRAMES * 2)
#define I2S_TX_ORIGIN    0
#define I2S_RX_ORIGIN    ARRAY_SIZE(i2s_instructions)
#define I2S_RX_BITLOOP   5
#define I2S_RX_JMP       8
#define I2S_TX_ENTRY     7

#define PIO_WAIT_GPIO(polarity, gpio) \
	((uint16_t)(0x2000u | ((uint16_t)(polarity) << 7) | (gpio)))
#define PIO_JMP_X_DEC(addr) ((uint16_t)(0x0040u | ((addr) & 0x1fu)))
#define PIO_SET_X(imm)      ((uint16_t)(0xe020u | ((imm) & 0x1fu)))
#define PIO_IN_PINS(n)      ((uint16_t)(0x4000u | ((n) & 0x1fu)))

BUILD_ASSERT(LRCLK == BCLK + 1, "PIO side-set requires adjacent BCLK/LRCLK pins");
BUILD_ASSERT(DATA_IN != DATA, "I2S DIN and SD must be different pins");
BUILD_ASSERT(!IS_ENABLED(CONFIG_DMA), "Audio owns DMA IRQ0; do not enable the Zephyr DMA driver");

static const struct device *const pio_dev = DEVICE_DT_GET(DT_ALIAS(audio_pio));
static PIO audio_pio;
static size_t tx_sm;
static size_t rx_sm;
static bool rx_enabled;
static int tx_channels[2] = { -1, -1 };
static int rx_channels[2] = { -1, -1 };
static uint32_t tx_buffers[2][TX_WORDS];
static uint32_t rx_buffers[2][BLOCK_FRAMES];
static atomic_t volume = ATOMIC_INIT(25);
static atomic_t tone_request;
static unsigned int tone_left;
static unsigned int tone_phase;

/*
 * Wait for WS falling, skip the I2S delay bit, capture the 32-bit left slot.
 * JMP target is absolute (TX program occupies the first 8 instruction slots).
 */
static const uint16_t i2s_rx_instructions[] = {
	PIO_WAIT_GPIO(1, LRCLK),
	PIO_WAIT_GPIO(0, LRCLK),
	PIO_WAIT_GPIO(1, BCLK),
	PIO_WAIT_GPIO(0, BCLK),
	PIO_SET_X(31),
	PIO_WAIT_GPIO(1, BCLK),
	PIO_IN_PINS(1),
	PIO_WAIT_GPIO(0, BCLK),
	PIO_JMP_X_DEC(I2S_RX_ORIGIN + I2S_RX_BITLOOP),
};

static const int16_t tone[48] = {
	0, 535, 1060, 1567, 2048, 2493, 2896, 3250, 3547, 3784, 3956, 4061,
	4096, 4061, 3956, 3784, 3547, 3250, 2896, 2493, 2048, 1567, 1060, 535,
	0, -535, -1060, -1567, -2048, -2493, -2896, -3250, -3547, -3784, -3956, -4061,
	-4096, -4061, -3956, -3784, -3547, -3250, -2896, -2493, -2048, -1567, -1060, -535,
};

BUILD_ASSERT(ARRAY_SIZE(i2s_rx_instructions) > I2S_RX_JMP, "RX JMP slot missing");

static void unclaim_dma(int channels[2])
{
	for (size_t i = 0; i < 2; i++) {
		if (channels[i] >= 0) {
			dma_channel_unclaim(channels[i]);
			channels[i] = -1;
		}
	}
}

static int claim_dma_pair(int channels[2])
{
	channels[0] = dma_claim_unused_channel(false);
	channels[1] = dma_claim_unused_channel(false);
	if (channels[0] < 0 || channels[1] < 0) {
		unclaim_dma(channels);
		return -EBUSY;
	}
	return 0;
}

static void configure_pingpong(int channels[2], void *fifo, void *buf0, void *buf1,
			       size_t words, uint8_t sm, bool to_pio)
{
	void *bufs[2] = { buf0, buf1 };

	for (unsigned int i = 0; i < 2; i++) {
		dma_channel_config cfg = dma_channel_get_default_config(channels[i]);

		channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
		channel_config_set_read_increment(&cfg, to_pio);
		channel_config_set_write_increment(&cfg, !to_pio);
		channel_config_set_dreq(&cfg, pio_get_dreq(audio_pio, sm, to_pio));
		channel_config_set_chain_to(&cfg, channels[1 - i]);
		dma_channel_configure(channels[i], &cfg, to_pio ? fifo : bufs[i],
				      to_pio ? bufs[i] : fifo, dma_encode_transfer_count(words),
				      false);
		dma_channel_acknowledge_irq0(channels[i]);
		dma_channel_set_irq0_enabled(channels[i], true);
	}
}

static void fill_tx_buffer(unsigned int index)
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
		uint32_t slot = ((uint16_t)sample) << 16;

		tx_buffers[index][2 * i] = slot;
		tx_buffers[index][2 * i + 1] = slot;
	}
}

static void harvest_rx_buffer(unsigned int index)
{
	int16_t mono[BLOCK_FRAMES];

	if (audio_loopback_enabled()) {
		return;
	}

	for (size_t i = 0; i < BLOCK_FRAMES; i++) {
		mono[i] = (int16_t)(rx_buffers[index][i] >> 16);
	}
	audio_record_push(mono, BLOCK_FRAMES);
}

static bool dma_take_irq0(int channel)
{
	if (channel < 0 || !dma_channel_get_irq0_status(channel)) {
		return false;
	}
	dma_channel_acknowledge_irq0(channel);
	return true;
}

static void audio_dma_isr(const void *arg)
{
	ARG_UNUSED(arg);

	for (unsigned int i = 0; i < 2; i++) {
		if (!dma_take_irq0(tx_channels[i])) {
			continue;
		}
		fill_tx_buffer(i);
		dma_channel_set_read_addr(tx_channels[i], tx_buffers[i], false);
		dma_channel_set_trans_count(tx_channels[i], dma_encode_transfer_count(TX_WORDS),
					    false);
	}

	if (!rx_enabled) {
		return;
	}

	for (unsigned int i = 0; i < 2; i++) {
		if (!dma_take_irq0(rx_channels[i])) {
			continue;
		}
		harvest_rx_buffer(i);
		dma_channel_set_write_addr(rx_channels[i], rx_buffers[i], false);
		dma_channel_set_trans_count(rx_channels[i],
					    dma_encode_transfer_count(BLOCK_FRAMES), false);
	}
}

static int audio_rx_init(void)
{
	const struct pio_program program = {
		.instructions = i2s_rx_instructions,
		.length = ARRAY_SIZE(i2s_rx_instructions),
		.origin = I2S_RX_ORIGIN,
	};
	int ret;

	if (!pio_can_add_program(audio_pio, &program)) {
		return -ENOMEM;
	}
	ret = pio_rpi_pico_allocate_sm(pio_dev, &rx_sm);
	if (ret < 0) {
		return ret;
	}
	ret = claim_dma_pair(rx_channels);
	if (ret < 0) {
		pio_sm_unclaim(audio_pio, rx_sm);
		return ret;
	}

	unsigned int offset = pio_add_program(audio_pio, &program);
	pio_sm_config cfg = pio_get_default_sm_config();

	sm_config_set_wrap(&cfg, offset, offset + I2S_RX_JMP);
	sm_config_set_in_pins(&cfg, DATA_IN);
	sm_config_set_in_shift(&cfg, false, true, 32);
	sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_RX);
	pio_sm_init(audio_pio, rx_sm, offset, &cfg);
	pio_sm_set_consecutive_pindirs(audio_pio, rx_sm, DATA_IN, 1, false);
	pio_gpio_init(audio_pio, DATA_IN);
	configure_pingpong(rx_channels, (void *)&audio_pio->rxf[rx_sm], rx_buffers[0],
			   rx_buffers[1], BLOCK_FRAMES, rx_sm, false);
	dma_start_channel_mask(BIT(rx_channels[0]));
	rx_enabled = true;
	return 0;
}

static int audio_tx_init(uint32_t clock_hz)
{
	const struct pio_program program = {
		.instructions = i2s_instructions,
		.length = ARRAY_SIZE(i2s_instructions),
		.origin = I2S_TX_ORIGIN,
	};
	int ret;

	if (!pio_can_add_program(audio_pio, &program)) {
		return -ENOMEM;
	}
	ret = pio_rpi_pico_allocate_sm(pio_dev, &tx_sm);
	if (ret < 0) {
		return ret;
	}
	ret = claim_dma_pair(tx_channels);
	if (ret < 0) {
		pio_sm_unclaim(audio_pio, tx_sm);
		return ret;
	}

	unsigned int offset = pio_add_program(audio_pio, &program);
	pio_sm_config cfg = pio_get_default_sm_config();
	uint32_t divider = ((uint64_t)clock_hz * 256 + AUDIO_SAMPLE_RATE_HZ * 64) /
			   (AUDIO_SAMPLE_RATE_HZ * 128);

	sm_config_set_wrap(&cfg, offset, offset + ARRAY_SIZE(i2s_instructions) - 1);
	sm_config_set_sideset(&cfg, 2, false, false);
	sm_config_set_sideset_pins(&cfg, BCLK);
	sm_config_set_out_pins(&cfg, DATA, 1);
	sm_config_set_out_shift(&cfg, false, true, 32);
	sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_TX);
	sm_config_set_clkdiv_int_frac(&cfg, divider >> 8, divider & 0xff);
	pio_sm_init(audio_pio, tx_sm, offset + I2S_TX_ENTRY, &cfg);
	pio_sm_set_pins_with_mask(audio_pio, tx_sm, 0, BIT(BCLK) | BIT(LRCLK) | BIT(DATA));
	pio_sm_set_consecutive_pindirs(audio_pio, tx_sm, BCLK, 2, true);
	pio_sm_set_consecutive_pindirs(audio_pio, tx_sm, DATA, 1, true);
	pio_gpio_init(audio_pio, BCLK);
	pio_gpio_init(audio_pio, LRCLK);
	pio_gpio_init(audio_pio, DATA);
	configure_pingpong(tx_channels, (void *)&audio_pio->txf[tx_sm], tx_buffers[0],
			   tx_buffers[1], TX_WORDS, tx_sm, true);
	return 0;
}

int audio_output_init(void)
{
	const struct reset_dt_spec dma_reset = RESET_DT_SPEC_GET(DT_NODELABEL(dma));
	const struct device *clock = DEVICE_DT_GET(DT_CLOCKS_CTLR(DT_ALIAS(audio_pio)));
	clock_control_subsys_t clock_id =
		(clock_control_subsys_t)DT_CLOCKS_CELL(DT_ALIAS(audio_pio), clk_id);
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
	ret = audio_tx_init(clock_hz);
	if (ret < 0) {
		return ret;
	}
	ret = audio_rx_init();
	if (ret < 0) {
		printk("demo: INMP441 I2S RX unavailable (%d)\n", ret);
	}

	IRQ_CONNECT(DT_IRQ_BY_IDX(DT_NODELABEL(dma), 0, irq), 2, audio_dma_isr, NULL, 0);
	irq_enable(DT_IRQ_BY_IDX(DT_NODELABEL(dma), 0, irq));
	dma_start_channel_mask(BIT(tx_channels[0]));
	if (rx_enabled) {
		pio_set_sm_mask_enabled(audio_pio, BIT(tx_sm) | BIT(rx_sm), true);
	} else {
		pio_sm_set_enabled(audio_pio, tx_sm, true);
	}
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
