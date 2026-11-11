"""Exercise the actual console parser with UTF-8, controls, and overflow."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/main.c').read_text(encoding='utf-8')
parser = source[source.index('static void console_monitor_poll('):
                source.index('static void console_monitor_thread(')]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define IS_ENABLED(x) 0
struct device { int unused; };
static const unsigned char *input;
static char captured[1024];
static unsigned calls;
struct spinlock { int unused; };static struct spinlock touch_lock;
typedef int k_spinlock_key_t;
static k_spinlock_key_t k_spin_lock(struct spinlock *l){(void)l;return 0;}
static void k_spin_unlock(struct spinlock *l,k_spinlock_key_t k){(void)l;(void)k;}
struct volume_touch {unsigned count,session;bool multitouch;};
static struct volume_touch touch_state;
static int64_t last_touch_report;
static struct {bool blocked,tracking,adjusting;unsigned alpha_q8;} speaker_control;
static void pico_touch_diagnostics(uint32_t *p,uint32_t *e,uint32_t *f,int *r){*p=*e=*f=0;*r=0;}
static int uart_poll_in(const struct device *d, unsigned char *c) {
    (void)d; if (!*input) return -1; *c = *input++; return 0;
}
static void uart_poll_out(const struct device *d, unsigned char c) { (void)d; (void)c; }
static void enter_usb_bootloader(void) { assert(0); }
static int battery_display_text(uint64_t id, bool spoken, const char *text) {
    assert(id == 42); assert(spoken); strcpy(captured, text); calls++; return 0;
}
static int battery_display_row(unsigned r, const char *t) { (void)r; (void)t; return 0; }
static unsigned listen_calls;
static int battery_display_listening(uint64_t id, unsigned phase) {
    listen_calls++; return phase <= 5 && (id || !phase) ? 0 : -EINVAL;
}
static int wifi_ui_command(const char *c) { (void)c; return 0; }
static int battery_display_unavailable(void) { return 0; }
static int battery_voltage_parse(const char *t, unsigned *v) { (void)t; *v=0; return 0; }
static int battery_display_update(unsigned v) { (void)v; return 0; }
static void audio_output_test_tone(void) {}
static unsigned audio_output_get_volume(void) { return 0; }
static void audio_output_set_volume(long v) { (void)v; }
static long k_uptime_get(void) { return 0; }
static void volume_control_set(void *s, long v, long t) { (void)s; (void)v; (void)t; }
static void audio_set_loopback(bool v) { (void)v; }
'''
checks = r'''
int main(void) {
    const char *text="\xe6\x88\x91\xe5\x9c\xa8\xe3\x80\x82 Hello";
    char command[1200];
    snprintf(command,sizeof(command),"talk 42 g %s\n",text);
    input=(const unsigned char *)command; console_monitor_poll(NULL);
    assert(calls==1 && strcmp(captured,text)==0);
    input=(const unsigned char *)"talk 42 g X\bHello\x01\r\n";
    console_monitor_poll(NULL); assert(calls==2 && strcmp(captured,"Hello")==0);
    memset(command,'X',sizeof(command)); command[1198]='\n'; command[1199]=0;
    input=(const unsigned char *)command; console_monitor_poll(NULL); assert(calls==2);
    input=(const unsigned char *)"talk 42 g recovered\n";
    console_monitor_poll(NULL); assert(calls==3 && strcmp(captured,"recovered")==0);
    input=(const unsigned char *)"listen 18446744073709551615 1\n";
    console_monitor_poll(NULL); assert(listen_calls==1);
    input=(const unsigned char *)"listen 5 1 trailing\n";
    console_monitor_poll(NULL); assert(listen_calls==1);
    input=(const unsigned char *)"touchstatus\n";console_monitor_poll(NULL);
    puts("PASS: Chinese UTF-8 survives console transport; controls and overflow recover");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path/'test.c').write_text(harness + parser + checks, encoding='utf-8')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
