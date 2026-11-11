"""Compile the real renderer against a capture LCD to check batching and retries."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
headers = {
    'zephyr/device.h': '''#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
struct device { int unused; };
static inline bool device_is_ready(const struct device *d) { return d != 0; }
''',
    'zephyr/drivers/display.h': '''#pragma once
#include <zephyr/device.h>
#include <stddef.h>
struct display_capabilities { int x_resolution, y_resolution; };
struct display_buffer_descriptor { size_t buf_size; int width, height, pitch; };
void display_get_capabilities(const struct device *, struct display_capabilities *);
int display_write(const struct device *, int, int, const struct display_buffer_descriptor *, const void *);
int display_blanking_off(const struct device *);
''',
    'zephyr/sys/util.h': '''#pragma once
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define BIT(n) (1U << (n))
#define ARG_UNUSED(a) (void)(a)
#define MAX(a,b) ((a)>(b)?(a):(b))
#define CLAMP(v,lo,hi) MIN(MAX(v,lo),hi)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define BUILD_ASSERT(c) _Static_assert(c, #c)
#define __aligned(n) __attribute__((aligned(n)))
''',
    'zephyr/kernel.h': '''#pragma once
#include <stdint.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#define K_FOREVER 0
#define K_NO_WAIT 0
struct k_sem { sem_t value; };
struct k_thread { pthread_t value; };
struct k_msgq { sem_t available, space; pthread_mutex_t lock; void *items[2]; int head, tail; };
#define K_MSGQ_DEFINE(name,size,count,align) struct k_msgq name
#define K_THREAD_STACK_DEFINE(name,size) char name[size]
#define K_THREAD_STACK_SIZEOF(name) sizeof(name)
static inline void k_sem_init(struct k_sem *s, int initial, int limit) { (void)limit; sem_init(&s->value,0,initial); }
static inline void k_sem_take(struct k_sem *s, int timeout) { (void)timeout; sem_wait(&s->value); }
static inline void k_sem_give(struct k_sem *s) { sem_post(&s->value); }
static inline void k_msgq_get(struct k_msgq *q, void *out, int timeout) {
    (void)timeout; sem_wait(&q->available); pthread_mutex_lock(&q->lock);
    memcpy(out,&q->items[q->head++ % 2],sizeof(void *));
    pthread_mutex_unlock(&q->lock); sem_post(&q->space);
}
static inline void k_msgq_put(struct k_msgq *q, const void *in, int timeout) {
    (void)timeout; sem_wait(&q->space); pthread_mutex_lock(&q->lock);
    memcpy(&q->items[q->tail++ % 2],in,sizeof(void *));
    pthread_mutex_unlock(&q->lock); sem_post(&q->available);
}
extern struct k_msgq flush_queue;
static void (*test_entry)(void *,void *,void *);
static void *test_start(void *unused) { (void)unused; test_entry(0,0,0); return 0; }
static inline void k_thread_create(struct k_thread *t, char *stack, size_t size,
    void (*entry)(void *,void *,void *), void *a, void *b, void *c, int priority, int options, int delay) {
    (void)stack; (void)size; (void)a; (void)b; (void)c; (void)priority; (void)options; (void)delay;
    sem_init(&flush_queue.available,0,0); sem_init(&flush_queue.space,0,2);
    pthread_mutex_init(&flush_queue.lock,0); test_entry=entry; pthread_create(&t->value,0,test_start,0);
}
int64_t k_uptime_get(void);
''',
}
harness = r'''
#include "battery_display.h"
#include <zephyr/drivers/display.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
static unsigned char frame[320][960];
static int writes, fail_next;
static size_t transferred;
static int64_t clock_ms = 1000;
int64_t k_uptime_get(void) { return clock_ms; }
void display_get_capabilities(const struct device *d, struct display_capabilities *c)
{ (void)d; c->x_resolution=480; c->y_resolution=320; }
int display_blanking_off(const struct device *d) { (void)d; return 0; }
int display_write(const struct device *d, int x, int y,
  const struct display_buffer_descriptor *desc, const void *buffer)
{
  (void)d; ++writes; transferred += desc->buf_size;
  assert(x >= 0 && x + desc->width <= 480 && y >= 0 && y + desc->height <= 320);
  assert(desc->width > 0 && desc->pitch == desc->width);
  assert(desc->height > 0 && desc->buf_size <= 15360);
  assert(desc->buf_size == (size_t)desc->height * desc->width * 2);
  assert(((uintptr_t)buffer & 3) == 0);
  unsigned char snapshot[15360]; memcpy(snapshot,buffer,desc->buf_size);
  struct timespec delay = {0,1000000}; nanosleep(&delay,0);
  assert(memcmp(snapshot,buffer,desc->buf_size)==0); /* no reuse during DMA */
  if (fail_next) { fail_next=0; return -EIO; }
  for(int row=0; row<desc->height; row++)
    memcpy(frame[y+row]+x*2, snapshot+row*desc->width*2, desc->width*2);
  return 0;
}
static unsigned pixel(int x, int y) { return frame[y][2*x]*256U + frame[y][2*x+1]; }
static void capture(const char *name) {
  const char *directory=getenv("DISPLAY_CAPTURE_DIR"); if(!directory) return;
  char path[512]; snprintf(path,sizeof(path),"%s/%s.ppm",directory,name);
  FILE *file=fopen(path,"wb"); assert(file); fprintf(file,"P6\n480 320\n255\n");
  for(int y=0;y<320;y++) for(int x=0;x<480;x++) {
    unsigned value=pixel(x,y);
    unsigned char rgb[3]={((value>>11)&31)*255/31,((value>>5)&63)*255/63,(value&31)*255/31};
    assert(fwrite(rgb,1,3,file)==3);
  }
  fclose(file);
}
int main(void)
{
  struct device lcd = {0};
  assert(battery_display_init(&lcd) == 0 && writes == 20);
  assert(pixel(100,222) == 0x0841);
  assert(battery_display_row(0,"ARMED") == 0);
  assert(pixel(12,15) == 0xfd20);
  int before=writes;
  assert(battery_display_row(0,"ARMED") == 0 && writes == before);
  assert(battery_display_row(0,"SLEEP") == 0);
  assert(pixel(12,15) != 0xfd20);
  assert(battery_display_update(2364) == 0);
  before=writes;
  assert(battery_display_update(2364) == 0 && writes == before);
  size_t bytes_before=transferred;
  assert(battery_display_update(2365)==0);
  assert(transferred-bytes_before < 480U*56U*2U); /* changed digit only */
  assert(battery_display_update(2364)==0);
  fail_next=1;
  assert(battery_display_update(2365) == -EIO);
  assert(battery_display_update(2365) == 0);
  unsigned char expected[sizeof(frame)]; memcpy(expected,frame,sizeof(frame));
  assert(battery_display_update(2364) == 0);
  assert(battery_display_update(2365) == 0);
  assert(memcmp(expected,frame,sizeof(frame))==0); /* retry matches clean render */
  fail_next=1;
  assert(battery_display_row(1,"WLAN1 192.168.31.130/24") == -EIO);
  assert(battery_display_row(1,"WLAN1 192.168.31.130/24") == 0);
  before=writes;
  clock_ms=7000;
  assert(battery_display_row(1,"WLAN1 192.168.31.130/24") == 0 && writes == before);
  clock_ms=10000; battery_display_tick(); assert(writes == before);
  clock_ms=16000; battery_display_tick(); assert(writes > before);
  before=writes; battery_display_tick(); assert(writes == before);
  assert(pixel(100,135)==0x0841);
  assert(battery_display_row(0,"SLEEP")==0);
  assert(battery_display_update(2364)==0);
  assert(battery_display_row(1,"WLAN1 192.168.31.130/24")==0);
  memcpy(expected,frame,sizeof(frame));
  capture("volume-hidden");
  assert(battery_display_volume(50,50*256,128)==0);
  capture("volume-fading");
  unsigned int halfway=pixel(442,240);
  assert(battery_display_volume(50,50*256,255)==0);
  assert(pixel(442,240)==0xffff && halfway!=0xffff && halfway!=0x0841);
  assert(pixel(442,100)==0x630c);
  capture("volume-visible");
  assert(battery_display_volume(0,0,255)==0 && pixel(442,240)==0x630c);
  before=writes;
  assert(battery_display_volume(0,0,255)==0 && writes==before);
  fail_next=1;
  assert(battery_display_volume(50,50*256,128)==-EIO);
  assert(battery_display_volume(50,50*256,128)==0);
  assert(battery_display_volume(50,50*256,0)==0);
  assert(memcmp(expected,frame,sizeof(frame))==0); /* fades restore every underlying pixel */
  clock_ms=17000;
  assert(battery_display_row(1,"ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUV")==0);
  memcpy(expected,frame,sizeof(frame));
  assert(battery_display_volume(100,100*256,255)==0);
  assert(battery_display_row(1,"WLAN1 192.168.31.130/24")==0);
  assert(battery_display_volume(100,100*256,0)==0);
  unsigned char updated[sizeof(frame)]; memcpy(updated,frame,sizeof(frame));
  assert(battery_display_row(1,"ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUV")==0);
  assert(memcmp(expected,frame,sizeof(frame))==0);
  assert(battery_display_row(1,"WLAN1 192.168.31.130/24")==0);
  assert(memcmp(updated,frame,sizeof(frame))==0); /* status changes beneath visible HUD */
  clock_ms=24000;
  assert(battery_display_volume(25,25*256,255)==0);
  clock_ms=26000; battery_display_tick();
  assert(battery_display_volume(25,25*256,0)==0 && pixel(100,135)==0x0841);
  assert(battery_display_text(1,false,"Hey Xin")==0);
  before=writes;
  assert(battery_display_text(1,false,"Hey Xin")==0 && writes==before);
  assert(battery_display_text(2,true,"Yes?")==0);
  unsigned whites=0, greens=0;
  for(int y=70;y<316;y++) for(int x=12;x<414;x++) {
    whites += pixel(x,y)==0xef7d; greens += pixel(x,y)==0x07e0;
  }
  assert(whites && greens);
  assert(battery_display_text(3,false,"\xe5\xb0\x8f\xe6\x96\xb0\xe5\x90\x8c\xe5\xad\xa6")==0);
  assert(battery_display_text(4,true,"\xe6\x88\x91\xe5\x9c\xa8\xe3\x80\x82")==0);
  assert(battery_display_row(0,"ARMED")==0);
  assert(battery_display_update(2460)==0);
  assert(battery_display_row(1,"eth0 192.168.31.228  1/2")==0);
  capture("conversation");
  memcpy(expected,frame,sizeof(frame));
  assert(battery_display_volume(50,50*256,255)==0);
  assert(battery_display_volume(50,50*256,0)==0);
  assert(memcmp(expected,frame,sizeof(frame))==0);
  assert(battery_display_text(5,false,"This sentence wraps naturally across several lines so old conversation scrolls upward.")==0);
  assert(battery_display_text(5,false,"This sentence updates in place instead of appending duplicate partial recognition entries.")==0);
  for(unsigned id=6;id<20;id++) assert(battery_display_text(id,id%2,"Latest conversation text")==0);
  capture("conversation-scrolled");
  memcpy(expected,frame,sizeof(frame));
  clock_ms=35000; battery_display_tick();
  assert(memcmp(expected+70*960,frame+70,246*960)==0); /* status watchdog preserves speech */
  assert(battery_display_row(0,"ARMED")==0);
  assert(battery_display_update(2460)==0);
  assert(battery_display_row(1,"eth0 192.168.31.228")==0);
  memcpy(expected,frame,sizeof(frame));
  assert(battery_display_listening(123,1)==0);
  uint64_t tapped=0;
  assert(battery_display_listening_hit(240,20,&tapped) && tapped==123);
  assert(!battery_display_listening_hit(400,20,&tapped));
  assert(!battery_display_listening_hit(240,60,&tapped));
  clock_ms=35100; battery_display_tick();
  clock_ms=35200; battery_display_tick();
  assert(memcmp(expected,frame,42*960)!=0);
  assert(memcmp(expected+44*960,frame+44,276*960)==0);
  assert(battery_display_listening_hit(140,45,&tapped)); /* enlarged touch margin */
  capture("listening");
  unsigned char animated[sizeof(frame)]; memcpy(animated,frame,sizeof(frame));
  clock_ms=35300; battery_display_tick();
  assert(memcmp(animated,frame,42*960)!=0);
  assert(battery_display_listening(123,2)==0);
  clock_ms=35400; battery_display_tick(); capture("thinking");
  assert(battery_display_listening(123,3)==0);
  clock_ms=35500; battery_display_tick(); capture("speaking");
  assert(battery_display_listening(123,5)==0);
  assert(!battery_display_listening_hit(240,20,&tapped));
  assert(battery_display_listening(0,0)==0);
  for(int i=0;i<4;i++) { clock_ms+=100; battery_display_tick(); }
  assert(memcmp(expected,frame,sizeof(frame))==0);
  assert(battery_display_listening(123,1)==0);
  clock_ms+=9000;
  assert(!battery_display_listening_hit(240,20,&tapped));
  puts("PASS: DMA ownership, dirty rectangles, retries, popup animation/hit region/fade restoration and stale-session watchdog");
}
'''
with tempfile.TemporaryDirectory() as directory:
    temporary = Path(directory)
    for name, content in headers.items():
        target = temporary / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)
    (temporary / 'test.c').write_text(harness)
    executable = temporary / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-pthread', '-Wall', '-Wextra', '-Werror',
        '-I' + str(temporary), '-I' + str(root / 'src'), str(root / 'src/battery_display.c'),
        str(root / 'src/display_font.c'), str(root / 'src/wifi_ui.c'), str(temporary / 'test.c'), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
