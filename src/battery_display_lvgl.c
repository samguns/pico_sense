/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_LV_TXT_ENC_UTF8
#error "The Pico LVGL display requires CONFIG_LV_TXT_ENC_UTF8=y"
#endif
#include "battery_display.h"
#include "wifi_ui.h"
#include "dashboard_lvgl.h"
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <stdio.h>
#define WIDTH 480
#define HEIGHT 320
#define STRIP_LINES 16
static const struct device *lcd;
static int refresh_error;
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
    int previous=finish_transfer(job);
    if(previous<0)refresh_error=previous;
    lv_draw_sw_rgb565_swap(pixels,width*height);

    job->x=area->x1;job->y=area->y1;
    job->desc=(struct display_buffer_descriptor){.buf_size=width*height*2,
        .width=width,.height=height,.pitch=width};
    job->lv_display=display;job->pending=true;
    k_msgq_put(&flush_queue,&job,K_FOREVER);
}
#endif

/* ACK only after all queued SPI writes complete. Retry a failed refresh as a
 * full LVGL invalidation; there is no shadow framebuffer to become stale. */
int dashboard_flush(void)
{
    if (!lcd) return -ENODEV;
    lv_refr_now(NULL);
    int result=refresh_error;
    refresh_error=0;
    for(unsigned i=0;i<ARRAY_SIZE(transfers);i++) {
        int ret=finish_transfer(&transfers[i]);if(ret<0)result=ret;
    }
    if(result<0)lv_obj_invalidate(lv_screen_active());
    return result;
}
int battery_display_init(const struct device *device)
{
    struct display_capabilities caps;
    if(!device_is_ready(device))return -ENODEV;
    display_get_capabilities(device,&caps);
    if(caps.x_resolution!=WIDTH || caps.y_resolution!=HEIGHT)return -EINVAL;
    lcd=device;
    for(unsigned i=0;i<ARRAY_SIZE(transfers);i++)k_sem_init(&transfers[i].done,0,1);
    k_thread_create(&flush_thread,flush_stack,K_THREAD_STACK_SIZEOF(flush_stack),
                    flush_worker,NULL,NULL,NULL,-1,0,K_NO_WAIT);
    int ret=wifi_ui_lvgl_init(transfers[0].pixels,transfers[1].pixels,
                              sizeof(transfers[0].pixels),lvgl_flush);
    if(ret==0){dashboard_init();ret=dashboard_flush();}
    if(ret==0)ret=display_blanking_off(lcd);
    if(ret<0)lcd=NULL;
    return ret;
}
