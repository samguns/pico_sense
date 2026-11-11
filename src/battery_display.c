/* SPDX-License-Identifier: Apache-2.0 */
#include "battery_display.h"
#include "display_font.h"
#include "wifi_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/drivers/display.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>

#define WIDTH 480
#define HEIGHT 320
#define BG 0x0841
#define WHITE 0xef7d
#define CYAN 0x07ff
#define GREEN 0x07e0
#define TEXT_TOP 70
#define TEXT_BOTTOM 316
#define MESSAGE_BYTES 768
#define VISIBLE_LINES 8
#define ARMED_ACCENT 0xfd20
#define STRIP_LINES 16
#define LISTEN_LEFT 144
#define LISTEN_RIGHT 336
#define LISTEN_BOTTOM 42

/* Original 5x7 bitmap glyphs, one column per byte, top pixel in bit zero. */
static const char alphabet[] = "0123456789.- ABCDEGILMNORTVXYFHJKPQSUWZ:/%_";
static const uint8_t glyphs[][5] = {
	{62,81,73,69,62}, {0,66,127,64,0}, {98,81,73,73,70},
	{34,65,73,73,54}, {24,20,18,127,16}, {39,69,69,69,57},
	{60,74,73,73,48}, {1,113,9,5,3}, {54,73,73,73,54},
	{6,73,73,41,30}, {0,96,96,0,0}, {8,8,8,8,8}, {0,0,0,0,0},
	{126,9,9,9,126}, {127,73,73,73,54}, {62,65,65,65,34},
	{127,65,65,34,28}, {127,73,73,73,65}, {62,65,73,73,58},
	{0,65,127,65,0}, {127,64,64,64,64}, {127,2,12,2,127},
	{127,4,8,16,127}, {62,65,65,65,62}, {127,9,25,41,70},
	{1,1,127,1,1}, {31,32,64,32,31}, {99,20,8,20,99},
	{3,4,120,4,3},
    {127,9,9,9,1}, {127,8,8,8,127},
    {32,64,65,63,1}, {127,8,20,34,65}, {127,9,9,9,6},
    {62,65,81,33,94}, {38,73,73,73,50}, {63,64,64,64,63},
    {63,64,56,64,63}, {97,81,73,69,67}, {0,54,54,0,0},
    {64,32,16,8,4}, {99,19,8,100,99}, {64,64,64,64,64},
};
BUILD_ASSERT(ARRAY_SIZE(glyphs) == sizeof(alphabet) - 1);
static const struct device *lcd;
/* Framebuffer stores wire-order RGB565. Only the producer accesses it.
 * Worker owns a strip from enqueue until completion; buffers never alias it. */
static uint8_t framebuffer[HEIGHT][WIDTH * 2] __aligned(4);
static bool valid_line[HEIGHT];
struct transfer {
    uint8_t pixels[STRIP_LINES * WIDTH * 2] __aligned(4);
    struct display_buffer_descriptor desc;
    uint16_t x, y;
    struct k_sem done;
    int result;
    bool pending;
#ifdef CONFIG_PICO_SENSE_LVGL_WIFI
    lv_display_t *lv_display;
#endif
};
static struct transfer transfers[2];
K_MSGQ_DEFINE(flush_queue, sizeof(struct transfer *), 2, sizeof(void *));
K_THREAD_STACK_DEFINE(flush_stack, 1536);
static struct k_thread flush_thread;
static bool worker_started;

static void flush_worker(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    for (;;) {
        struct transfer *job;
        k_msgq_get(&flush_queue, &job, K_FOREVER);
        job->result = display_write(lcd, job->x, job->y, &job->desc, job->pixels);
#ifdef CONFIG_PICO_SENSE_LVGL_WIFI
        if(job->lv_display) {
            lv_display_flush_ready(job->lv_display);
            job->lv_display=NULL;
        }
#endif
        k_sem_give(&job->done);
    }
}

static int finish_transfer(struct transfer *job)
{
    if (!job->pending) return 0;
    k_sem_take(&job->done, K_FOREVER);
    job->pending = false;
    return job->result;
}
#ifdef CONFIG_PICO_SENSE_LVGL_WIFI
/* LVGL partial buffers alias the existing DMA strips. A buffer is released to
 * LVGL only after display_write finishes. No extra channel or draw buffer. */
static void lvgl_flush(lv_display_t *display,const lv_area_t *area,uint8_t *pixels)
{
    struct transfer *job=NULL;
    for(unsigned i=0;i<ARRAY_SIZE(transfers);i++)
        if(pixels==transfers[i].pixels)job=&transfers[i];
    int width=area->x2-area->x1+1,height=area->y2-area->y1+1;
    if(!job || width<1 || height<1 || area->x1<0 || area->y1<0 ||
       area->x2>=WIDTH || area->y2>=HEIGHT || width*height*2>(int)sizeof(job->pixels)) {
        printf("LCD LVGL invalid flush area\n");lv_display_flush_ready(display);return;
    }
    finish_transfer(job);
    lv_draw_sw_rgb565_swap(pixels,width*height);
    for(int y=0;y<height;y++)
        memcpy(framebuffer[area->y1+y]+area->x1*2,pixels+y*width*2,width*2);
    job->x=area->x1;job->y=area->y1;
    job->desc=(struct display_buffer_descriptor){.buf_size=width*height*2,
        .width=width,.height=height,.pitch=width};
    job->lv_display=display;job->pending=true;
    k_msgq_put(&flush_queue,&job,K_FOREVER);
}
#endif
static uint8_t *line;
static char rows[6][79];
static unsigned int current_voltage;
static bool voltage_available;
static bool voltage_drawn;
static bool row_drawn[6];
static int64_t last_status;
static unsigned int volume_percent = 25, volume_level_q8 = 25 * 256;
static unsigned int volume_alpha;
static bool volume_drawn = true;
static unsigned int draw_alpha = 255;
static uint64_t listening_id;
static unsigned int listening_phase, listening_alpha, listening_wave;
static int64_t listening_updated, listening_tick;
static int draw_left, draw_right = WIDTH;
static struct {
    uint64_t id;
    bool spoken;
    char text[MESSAGE_BYTES];
} messages[8];
static unsigned int message_count;
static struct text_line {
    const uint8_t *bits[28];
    uint8_t widths[28], count;
    uint16_t color;
    bool new_turn;
    int top;
} conversation[VISIBLE_LINES];
static unsigned int conversation_count;
static bool conversation_drawn;

static uint32_t utf8_next(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    uint32_t code = *p++;
    unsigned int extra = 0;
    if (code >= 0xc2 && code <= 0xdf) { code &= 31; extra = 1; }
    else if (code >= 0xe0 && code <= 0xef) { code &= 15; extra = 2; }
    else if (code >= 0xf0 && code <= 0xf4) { code &= 7; extra = 3; }
    else if (code >= 0x80) { *text = (const char *)p; return 0x25a1; }
    for (unsigned int i = 0; i < extra; ++i) {
        if ((*p & 0xc0) != 0x80) { *text = (const char *)p; return 0x25a1; }
        code = (code << 6) | (*p++ & 63);
    }
    *text = (const char *)p;
    if ((extra == 1 && code < 0x80) || (extra == 2 && code < 0x800) ||
        (extra == 3 && code < 0x10000) || code > 0x10ffff ||
        (code >= 0xd800 && code <= 0xdfff)) return 0x25a1;
    return code;
}

static void append_line(struct text_line *current)
{
    if (!current->count) return;
    if (conversation_count == VISIBLE_LINES) {
        memmove(conversation, conversation + 1, sizeof(conversation[0]) * (VISIBLE_LINES - 1));
        conversation_count--;
    }
    conversation[conversation_count++] = *current;
    current->count = 0;
    current->new_turn = false;
}

static void layout_conversation(void)
{
    conversation_count = 0;
    for (unsigned int i = 0; i < message_count; ++i) {
        struct text_line current = {.color = messages[i].spoken ? GREEN : WHITE, .new_turn = true};
        const char *p = messages[i].text;
        unsigned int used = 0;
        bool word_start = true;
        while (*p) {
            const char *begin = p;
            uint32_t code = utf8_next(&p);
            if (code == '\n') { append_line(&current); used = 0; word_start = true; continue; }
            if (code == '\t' || code == '\r') code = ' ';
            if (code == ' ' && !used) continue;
            if (word_start && code > ' ' && code < 128) {
                unsigned int word_width = 0;
                for (const unsigned char *q = (const unsigned char *)begin; *q > ' ' && *q < 128; ++q)
                    word_width += 16;
                if (used && word_width <= WIDTH - 24 && used + word_width > WIDTH - 24) {
                    append_line(&current); used = 0;
                }
            }
            unsigned int width;
            const uint8_t *bits = display_font_glyph(code, &width);
            if (used + width * 2 > WIDTH - 24 || current.count == ARRAY_SIZE(current.bits)) {
                append_line(&current); used = 0;
                if (code == ' ') { word_start = true; continue; }
            }
            current.bits[current.count] = bits;
            current.widths[current.count++] = width;
            used += width * 2;
            word_start = code == ' ';
        }
        append_line(&current);
    }
    unsigned int first = 0;
    for (;;) {
        int height = 0;
        for (unsigned int i = first; i < conversation_count; ++i)
            height += 34 + (i > first && conversation[i].new_turn ? 4 : 0);
        if (height <= TEXT_BOTTOM - TEXT_TOP || first == conversation_count) break;
        first++;
    }
    if (first) {
        conversation_count -= first;
        memmove(conversation, conversation + first, conversation_count * sizeof(conversation[0]));
    }
    int top = TEXT_TOP;
    for (unsigned int i = 0; i < conversation_count; ++i) {
        if (i && conversation[i].new_turn) top += 4;
        conversation[i].top = top;
        top += 34;
    }
}

static uint16_t blend(uint16_t front, uint16_t back, unsigned int alpha)
{
    unsigned int inverse = 255 - alpha;
    unsigned int r = (((front >> 11) & 31) * alpha + ((back >> 11) & 31) * inverse + 127) / 255;
    unsigned int g = (((front >> 5) & 63) * alpha + ((back >> 5) & 63) * inverse + 127) / 255;
    unsigned int b = ((front & 31) * alpha + (back & 31) * inverse + 127) / 255;
    return (r << 11) | (g << 5) | b;
}

static void pixel(int x, uint16_t color)
{
	if (x < draw_left || x >= draw_right) return;
	if (draw_alpha != 255) color = blend(color, (line[x * 2] << 8) | line[x * 2 + 1], draw_alpha);
	line[x * 2] = color >> 8;
	line[x * 2 + 1] = color & 255;
}

/* Render into a scanline, compare final pixels, then update the framebuffer. */
static void text_row(const char *text, int left, int top, int scale,
		     int y, uint16_t color)
{
	if (y < top || y >= top + 7 * scale) {
		return;
	}
	for (int i = 0; text[i]; i++) {
		const char *g = strchr(alphabet, text[i]);
		if (!g) {
			continue;
		}
		for (int col = 0; col < 5; col++) {
			if (!(glyphs[g - alphabet][col] & BIT((y - top) / scale))) {
				continue;
			}
			for (int dx = 0; dx < scale; dx++) {
				int x = left + (i * 6 + col) * scale + dx;
				if (x >= 0 && x < WIDTH) {
					pixel(x, color);
				}
			}
		}
	}
}

static void format_voltage(char *buf, size_t size, unsigned int value)
{
	snprintf(buf, size, "%02u.%02u V", value / 100, value % 100);
}

static void conversation_row(int y)
{
    for (unsigned int i = 0; i < conversation_count; ++i) {
        const struct text_line *row = &conversation[i];
        if (y < row->top || y >= row->top + 32) continue;
        unsigned int source_y = (y - row->top) / 2;
        int left = 12;
        for (unsigned int j = 0; j < row->count; ++j) {
            unsigned int bits = (row->bits[j][source_y * 2] << 8) | row->bits[j][source_y * 2 + 1];
            for (unsigned int x = 0; x < row->widths[j]; ++x)
                if (bits & (0x8000U >> x)) {
                    pixel(left + x * 2, row->color);
                    pixel(left + x * 2 + 1, row->color);
                }
            left += row->widths[j] * 2;
        }
    }
}

static void network_row(int y)
{
    if (y < 44 || y >= 60) return;
    const char *p = rows[1];
    int left = 12;
    text_row("WIFI", 426, 45, 2, y, CYAN);
    while (*p && left < WIDTH - 60) {
        unsigned int width;
        const uint8_t *bits = display_font_glyph(utf8_next(&p), &width);
        unsigned int pixels = (bits[(y - 44) * 2] << 8) | bits[(y - 44) * 2 + 1];
        for (unsigned int x = 0; x < width && left + x < WIDTH - 60; ++x)
            if (pixels & (0x8000U >> x)) pixel(left + x, WHITE);
        left += width;
    }
}

static bool rounded(int x, int y, int left, int top, int width, int height, int radius)
{
    if (x < left || x >= left + width || y < top || y >= top + height) return false;
    int cx = CLAMP(x, left + radius, left + width - radius - 1);
    int cy = CLAMP(y, top + radius, top + height - radius - 1);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius;
}

/* Transient iPhone-style HUD, composited over freshly rendered status pixels.
 * It never modifies the underlying status model or its freshness watchdog. */
static void volume_overlay(int y)
{
    if (!volume_alpha || y < 34 || y >= 302) return;
    draw_alpha = volume_alpha;
    int filled_top = 264 - (198 * volume_level_q8 + 12800) / 25600;
    for (int x = 414; x < 470; x++) {
        if (rounded(x, y, 414, 34, 56, 268, 24)) pixel(x, 0x18e3);
        if (rounded(x, y, 424, 66, 36, 198, 18))
            pixel(x, y >= filled_top ? 0xffff : 0x630c);
        if ((x >= 431 && x <= 436 && y >= 275 && y <= 283) ||
            (x >= 437 && x <= 443 && abs(y - 279) <= x - 435)) pixel(x, 0xffff);
        if (volume_percent) {
            int dx = x - 442, dy = y - 279;
            int distance = dx * dx + dy * dy;
            if (x >= 447 && x <= 452 && distance >= 64 && distance <= 100)
                pixel(x, 0xffff);
        } else if (x >= 447 && x <= 453 &&
                   (y - 276 == x - 447 || y - 276 == 453 - x)) pixel(x, 0xffff);
    }
    char label[8];
    snprintf(label, sizeof(label), "%u%%", volume_percent);
    text_row(label, 442 - ((int)strlen(label) * 6 - 1) / 2, 47, 1, y, 0xffff);
    draw_alpha = 255;
}

/* Top-center popup uses the existing framebuffer and DMA staging buffers. */
static void listening_overlay(int y)
{
    if (!listening_alpha || y >= LISTEN_BOTTOM) return;
    int top = 3 - (255 - (int)listening_alpha) * 12 / 255;
    uint16_t color = listening_phase == 1 ? CYAN :
                     listening_phase == 3 ? GREEN : 0xb39f;
    draw_alpha = listening_alpha;
    for (int x = LISTEN_LEFT; x < LISTEN_RIGHT; ++x)
        if (rounded(x, y, LISTEN_LEFT, top, LISTEN_RIGHT - LISTEN_LEFT, 38, 17)) pixel(x, 0x2106);
    static const char *const labels[] = {"", "LISTENING", "THINKING", "SPEAKING", "PAUSED", "CLOSING"};
    const char *label = labels[MIN(listening_phase, 5)];
    text_row(label, 240 - ((int)strlen(label) * 6 - 1), top + 3, 2, y, color);
    for (int i = 0; i < 11; ++i) {
        int pulse = (listening_wave + i * 3) % 24;
        int half = listening_phase == 4 ? 3 : 3 + abs(12 - pulse) / 2;
        for (int x = 187 + i * 10; x < 193 + i * 10; ++x)
            if (y >= top + 28 - half && y <= top + 28 + half) pixel(x, color);
    }
    draw_alpha = 255;
}

static int render_region(int first, int end, int left, int right)
{
#ifdef CONFIG_PICO_SENSE_LVGL_WIFI
    if(wifi_ui_active())return 0; /* The panel owns LCD writes until it closes. */
#endif
	/* Narrow regions can fit many more lines into the same staging buffer. */
    int batch_lines = sizeof(transfers[0].pixels) / ((right - left) * 2);
    draw_left = left;
    draw_right = right;
	char current[16] = "--.-- V";
	if (voltage_available) {
		format_voltage(current, sizeof(current), current_voltage);
	}
    uint8_t scanline[WIDTH * 2] __aligned(4);
    unsigned int next = 0;
    int result = 0;
    for (int start = first; start < end; start += batch_lines) {
        int count = MIN(batch_lines, end - start);
        int left_dirty = WIDTH, right_dirty = -1;
        int top_dirty = start + count, bottom_dirty = start - 1;
        for (int y = start; y < start + count; y++) {
            line = scanline;
            for (int x = left; x < right; x++) pixel(x, BG);
            bool armed = strcmp(rows[0], "ARMED") == 0;
            text_row(rows[0], 12, 12, armed ? 3 : 2, y, armed ? ARMED_ACCENT : WHITE);
            text_row(current, WIDTH - 12 - ((int)strlen(current) * 6 - 1) * 3,
                     12, 3, y, CYAN);
            network_row(y);
            conversation_row(y);
            volume_overlay(y);
            listening_overlay(y);
            wifi_ui_render(y, pixel);
            for (int x = left; x < right; x++) {
                if (!valid_line[y] || memcmp(scanline + x * 2,
                                            framebuffer[y] + x * 2, 2)) {
                    left_dirty = MIN(left_dirty, x);
                    right_dirty = MAX(right_dirty, x);
                    top_dirty = MIN(top_dirty, y);
                    bottom_dirty = MAX(bottom_dirty, y);
                }
            }
            memcpy(framebuffer[y] + left * 2, scanline + left * 2, (right - left) * 2);
        }
        if (right_dirty < left_dirty) continue;
        struct transfer *job = &transfers[next];
        int ret = finish_transfer(job);
        if (ret < 0) { result = ret; break; }
        int width = right_dirty - left_dirty + 1;
        int height = bottom_dirty - top_dirty + 1;
        for (int y = 0; y < height; y++) {
            memcpy(job->pixels + y * width * 2,
                   framebuffer[top_dirty + y] + left_dirty * 2, width * 2);
        }
        job->x = left_dirty;
        job->y = top_dirty;
        job->desc = (struct display_buffer_descriptor) {
            .buf_size = width * height * 2, .width = width,
            .height = height, .pitch = width,
        };
        job->pending = true;
        k_msgq_put(&flush_queue, &job, K_FOREVER);
        next ^= 1;
    }
    /* ACK remains completion-based. Drain even after an error so no queued
     * buffer outlives this call; failed ranges must resend on the next update. */
    for (unsigned int i = 0; i < ARRAY_SIZE(transfers); i++) {
        int ret = finish_transfer(&transfers[i]);
        if (ret < 0) result = ret;
    }
    /* Cropped success must not mark untransmitted pixels valid after a failure. */
    if (result < 0 || (left == 0 && right == WIDTH))
        memset(valid_line + first, result == 0, end - first);
    draw_left = 0;
    draw_right = WIDTH;
    return result;
}

static int render_range(int first, int end)
{
    return render_region(first, end, 0, WIDTH);
}

int battery_display_volume(unsigned int percent, unsigned int level_q8, unsigned int alpha)
{
    if (!lcd) return -ENODEV;
    percent = MIN(percent, 100);
    level_q8 = MIN(level_q8, 100 * 256);
    alpha = MIN(alpha, 255);
    /* Quantize to the visible fill height, avoiding subpixel-only redraws. */
    if (volume_drawn && alpha == volume_alpha &&
        (!alpha || (percent == volume_percent &&
         (198 * level_q8 + 12800) / 25600 == (198 * volume_level_q8 + 12800) / 25600))) return 0;
    volume_percent = percent;
    volume_level_q8 = level_q8;
    volume_alpha = alpha;
    int ret = render_region(34, 302, 414, 470);
    volume_drawn = ret == 0;
    return ret;
}

int battery_display_init(const struct device *display)
{
	struct display_capabilities caps;
	if (!device_is_ready(display)) {
		return -ENODEV;
	}
	display_get_capabilities(display, &caps);
	if (caps.x_resolution != WIDTH || caps.y_resolution != HEIGHT) {
		return -EINVAL;
	}
	lcd = display;
    if (!worker_started) {
        for (unsigned int i = 0; i < ARRAY_SIZE(transfers); i++)
            k_sem_init(&transfers[i].done, 0, 1);
        /* Start DMA before returning to the producer, then sleep in the
         * synchronous driver while the producer renders the next strip. */
        k_thread_create(&flush_thread, flush_stack, K_THREAD_STACK_SIZEOF(flush_stack),
                        flush_worker, NULL, NULL, NULL, -1, 0, K_NO_WAIT);
        worker_started = true;
    }
    memset(valid_line, 0, sizeof(valid_line));
    message_count = conversation_count = 0;
    conversation_drawn = false;
    strcpy(rows[0], "STATE UNAVAILABLE");
    last_status = k_uptime_get();
	int ret = render_range(0, HEIGHT);
	if (ret == 0) {
        voltage_drawn = true;
        memset(row_drawn, true, sizeof(row_drawn));
		ret = display_blanking_off(lcd);
	}
	if (ret < 0) {
		lcd = NULL;
	}
#ifdef CONFIG_PICO_SENSE_LVGL_WIFI
    if(ret==0)ret=wifi_ui_lvgl_init(transfers[0].pixels,transfers[1].pixels,
                                  sizeof(transfers[0].pixels),lvgl_flush);
#endif
	return ret;
}

int battery_display_update(unsigned int centivolts)
{
	if (!lcd) {
		return -ENODEV;
	}
    last_status = k_uptime_get();
    if (voltage_drawn && voltage_available && current_voltage == centivolts) return 0;
    current_voltage = centivolts;
    voltage_available = true;
    int ret = render_range(0, 36);
    voltage_drawn = ret == 0;
    return ret;
}

int battery_display_unavailable(void)
{
    if (!lcd) return -ENODEV;
    last_status = k_uptime_get();
    if (voltage_drawn && !voltage_available) return 0;
    voltage_available = false;
    int ret = render_range(0, 36);
    voltage_drawn = ret == 0;
    return ret;
}

int battery_display_row(unsigned int row, const char *text)
{
    if (!lcd) return -ENODEV;
    if (row >= ARRAY_SIZE(rows) || strlen(text) >= sizeof(rows[0])) return -EINVAL;
    last_status = k_uptime_get();
    if (row >= 2) return 0; /* Accept old hosts without occupying conversation space. */
    if (row_drawn[row] && strcmp(rows[row], text) == 0) return 0;
    strcpy(rows[row], text);
    int ret = row == 0 ? render_range(0, 36)
        : render_range(40, 64);
    row_drawn[row] = ret == 0;
    return ret;
}

int battery_display_text(uint64_t id, bool spoken, const char *text)
{
    if (!lcd) return -ENODEV;
    if (!text || strlen(text) >= MESSAGE_BYTES) return -EINVAL;
    unsigned int index;
    for (index = 0; index < message_count; ++index)
        if (messages[index].id == id) break;
    if (index < message_count && conversation_drawn && messages[index].spoken == spoken &&
        strcmp(messages[index].text, text) == 0) return 0;
    if (index == ARRAY_SIZE(messages)) {
        memmove(messages, messages + 1, (ARRAY_SIZE(messages) - 1) * sizeof(messages[0]));
        message_count--; index--;
    }
    if (index == message_count) message_count++;
    messages[index].id = id;
    messages[index].spoken = spoken;
    strcpy(messages[index].text, text);
    layout_conversation();
    int ret = render_range(TEXT_TOP, TEXT_BOTTOM);
    conversation_drawn = ret == 0;
    return ret;
}

int battery_display_listening(uint64_t id, unsigned int phase)
{
    if (!lcd) return -ENODEV;
    if (phase > 5 || (!id && phase)) return -EINVAL;
    listening_id = id;
    listening_phase = phase;
    listening_updated = k_uptime_get();
    return 0;
}

bool battery_display_listening_hit(int x, int y, uint64_t *id)
{
    if (!listening_id || !listening_phase || listening_phase == 5 ||
        k_uptime_get() - listening_updated >= 8000) return false;
    /* A finger-sized margin helps taps near the animated edge land reliably. */
    if (!rounded(x, y, LISTEN_LEFT - 10, 0, LISTEN_RIGHT - LISTEN_LEFT + 20, 54, 12)) return false;
    *id = listening_id;
    return true;
}

void battery_display_tick(void)
{
    bool wifi_changed=wifi_ui_dirty();
#ifdef CONFIG_PICO_SENSE_LVGL_WIFI
    wifi_ui_lvgl_tick(wifi_changed);
    if(wifi_ui_active())return;
#endif
    if (wifi_changed && lcd) render_range(0, HEIGHT);
    int64_t now = k_uptime_get();
    if (lcd && now - listening_tick >= 40) {
        unsigned int previous = listening_alpha;
        bool active = listening_id && listening_phase && now - listening_updated < 8000;
        int step = MIN(now - listening_tick, 100) * 255 / (active ? 180 : 240);
        listening_alpha = active ? MIN(255, listening_alpha + step) : MAX(0, (int)listening_alpha - step);
        listening_wave = (now / 65) % 24;
        listening_tick = now;
        if (listening_alpha || previous) render_region(0, LISTEN_BOTTOM, LISTEN_LEFT, LISTEN_RIGHT);
    }
    if (!lcd || !last_status || k_uptime_get() - last_status < 8000) return;
    voltage_available = false;
    memset(rows, 0, sizeof(rows));
    strcpy(rows[0], "STATUS UNAVAILABLE");
    int ret = render_range(0, TEXT_TOP);
    voltage_drawn = ret == 0;
    memset(row_drawn, ret == 0, sizeof(row_drawn));
    if (ret == 0) last_status = 0;
}
