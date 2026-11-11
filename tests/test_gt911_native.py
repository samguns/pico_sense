"""Exercise the actual board GT911 report reader with I2C faults and rapid taps."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'drivers/input_gt911.c').read_text(encoding='utf-8')
process=source[source.index('static int gt911_process('):source.index('/* Keep touch I2C')]
harness=r"""
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "volume_control.h"
#define CONFIG_INPUT_GT911_MAX_TOUCH_POINTS 5
#define GT911_REG_STATUS 0x814e
#define GT911_TOUCH_STATUS_MSK 0x80
#define GT911_TOUCH_POINTS_MSK 15
#define GT911_REG_POINT_ADDR(n) (0x814f+8*(n))
#define INPUT_ABS_MT_SLOT 1
#define INPUT_BTN_TOUCH 2
#define K_FOREVER 0
#define min(a,b) ((a)<(b)?(a):(b))
struct gt911_point_reg {uint8_t id,low_x,high_x,low_y,high_y,low_size,high_size,reserved;};
struct gt911_data {uint8_t prev_points;struct gt911_point_reg prev_point_reg[5];};
struct device {void *data;};
static uint32_t gt911_frames;
#define atomic_inc(p) ((*(p))++)
static uint8_t status;
static struct gt911_point_reg point;
static bool acked,fail_point,fail_ack;
static unsigned reads,events,clears;
static struct volume_touch touch;
static struct volume_control control;
static struct volume_view view;
static int slot,x,y;
static uint16_t sys_cpu_to_be16(uint16_t value){return (value>>8)|(value<<8);}
static int gt911_i2c_write_read(const struct device *d,const void *reg,size_t n,void *out,size_t size) {
 (void)d;(void)n;
 uint16_t addr=sys_cpu_to_be16(*(const uint16_t *)reg);
 if(addr==GT911_REG_STATUS){assert(size==1);*(uint8_t *)out=status;return 0;}
 assert(addr==GT911_REG_POINT_ADDR(0));assert(!acked); /* coordinates must precede ACK */
 if(fail_point)return -EIO;
 assert(size==sizeof(point));memcpy(out,&point,size);reads++;return 0;
}
static int gt911_i2c_write(const struct device *d,const uint8_t *bytes,uint32_t n) {
 (void)d;assert(n==3 && bytes[0]==0x81 && bytes[1]==0x4e && bytes[2]==0);
 if(fail_ack)return -EIO;
 assert(!(status&15) || reads==1);acked=true;clears++;return 0;
}
static void input_report_abs(const struct device *d,int code,int value,bool sync,int timeout) {
 (void)d;(void)sync;(void)timeout;assert(acked && code==INPUT_ABS_MT_SLOT);slot=value;
}
static void input_touchscreen_report_pos(const struct device *d,int col,int row,int timeout) {
 (void)d;(void)timeout;assert(acked);x=col;y=row;
}
static void input_report_key(const struct device *d,int code,int value,bool sync,int timeout) {
 (void)d;(void)sync;(void)timeout;assert(acked && code==INPUT_BTN_TOUCH);events++;
 volume_touch_report(&touch,slot,x,y,value);
}
static void frame(unsigned count,int track,int px,int py) {
 status=0x80|count;point=(struct gt911_point_reg){track,px&255,px>>8,py&255,py>>8,0,0,0};
 reads=0;acked=false;
}
"""
checks=r"""
int main(void) {
 struct gt911_data data={0};struct device dev={&data};
 volume_control_init(&control,25,1000);
 frame(1,3,350,220);fail_point=true;
 assert(gt911_process(&dev)==-EIO && !acked && !events && !data.prev_points);
 fail_point=false;assert(gt911_process(&dev)==0 && acked && touch.count==1);
 frame(0,0,0,0);fail_ack=true;unsigned old_events=events;
 assert(gt911_process(&dev)==-EIO && events==old_events && data.prev_points==1);
 fail_ack=false;assert(gt911_process(&dev)==0 && touch.count==0);
 int64_t now=1000;unsigned actual=25;
 for(unsigned i=0;i<200;i++) {
  frame(1,i%16,350,220);assert(gt911_process(&dev)==0);
  now+=20;assert(!volume_control_tick(&control,&touch,actual,now,&view));
  frame(1,i%16,350,100);assert(gt911_process(&dev)==0);
  now+=20;volume_control_tick(&control,&touch,actual,now,&view);actual=view.percent;
  assert(view.alpha>0 && control.adjusting && !control.blocked);
  frame(0,0,0,0);assert(gt911_process(&dev)==0 && touch.count==0);
  now+=20;volume_control_tick(&control,&touch,actual,now,&view);
  assert(!control.blocked && !control.tracking);
 }
 status=0;acked=false;unsigned old_clears=clears;
 assert(gt911_process(&dev)==0 && !acked && clears==old_clears);
 puts("PASS: read-before-ack, read/ack error retry, release delivery and 200 fresh volume gestures");
}
"""
with tempfile.TemporaryDirectory() as directory:
 path=Path(directory);(path/'test.c').write_text(harness+process+checks,encoding='utf-8')
 exe=path/'test'
 subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',
 '-I'+str(root/'src'),str(path/'test.c'),str(root/'src/volume_control.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
