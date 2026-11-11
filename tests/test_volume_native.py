"""Exercise the real gesture/animation controller without Zephyr or hardware."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
harness = r'''
#include "volume_control.h"
#include <assert.h>
#include <stdio.h>
static struct volume_control control;
static struct volume_touch touch;
static struct volume_view view;
static unsigned int actual=25;
static int64_t now=1000;
static bool tick(void) {
    now+=20;
    bool changed=volume_control_tick(&control,&touch,actual,now,&view);
    if(changed) actual=view.percent;
    assert(view.percent<=100 && view.level_q8<=25600 && view.alpha<=255);
    return changed;
}
static void report(int slot,int x,int y,bool down) {
    volume_touch_report(&touch,slot,x,y,down);
}
int main(void) {
    volume_control_init(&control,actual,now);
    report(0,350,220,true); assert(!tick());
    for(int i=0;i<150;i++) assert(!tick() && !view.alpha); /* waiting before first swipe */
    report(0,350,217,true); assert(!tick() && !view.alpha); /* jitter */
    report(0,350,100,true); assert(tick() && actual==75 && view.alpha>0);
    assert(view.level_q8>25*256 && view.level_q8<75*256); /* easing */
    for(int i=0;i<150;i++) assert(!tick() && actual==75);
    assert(view.alpha==255); /* stationary hold must not fade */
    report(0,350,148,true); assert(tick() && actual==55); /* slide down */
    report(0,350,0,true); assert(tick() && actual==100);
    report(0,350,24,true); assert(tick() && actual==90); /* immediate reversal */
    report(0,350,319,true); assert(tick() && actual==0);
    report(0,350,295,true); assert(tick() && actual==10);
    for(int i=0;i<150;i++) assert(!tick() && actual==10);
    assert(view.alpha==255);
    report(0,350,295,false); tick();
    for(int i=0;i<40;i++) tick();
    assert(view.alpha==255);
    unsigned int last=view.alpha;
    for(int i=0;i<60;i++) { tick(); assert(view.alpha<=last); last=view.alpha; }
    assert(view.alpha==0);
    /* Starts on the left, remains blocked even if dragged into the right half. */
    report(3,100,220,true); tick(); report(3,350,100,true); assert(!tick() && actual==10);
    report(3,350,100,false); tick();
    /* Horizontal movement/taps do not reveal HUD. */
    report(3,300,200,true); tick(); report(3,390,192,true); assert(!tick() && !view.alpha);
    report(3,390,192,false); tick();
    /* Multitouch latch catches a second finger even if released between ticks. */
    report(3,350,220,true); tick();
    report(4,400,200,true); report(4,400,200,false);
    report(3,350,100,true); assert(!tick() && actual==10);
    report(3,350,100,false); tick();
    report(15,350,220,true); tick(); report(15,350,100,true); assert(tick() && actual==60);
    report(15,350,100,false); tick();
    /* Fresh gesture after all-up between snapshots cannot inherit an old anchor. */
    report(2,350,220,true); tick();
    report(2,350,220,false); report(2,350,50,true); assert(!tick() && actual==60);
    report(2,350,50,false); tick();
    volume_control_set(&control,37,now); actual=37;
    for(int i=0;i<10;i++) { tick(); }
    assert(view.percent==37 && view.alpha==255);
    for(int i=0;i<80;i++) { tick(); }
    assert(view.alpha==0 && view.level_q8==37*256);
    report(16,350,200,true); assert(touch.count==0); /* malformed ID ignored */
    puts("PASS: direction, limits, reversal, taps, left half, multitouch latch, sessions, stationary holds, release timeout, easing and fade");
}
'''
with tempfile.TemporaryDirectory() as directory:
    temporary = Path(directory)
    (temporary / 'test.c').write_text(harness)
    executable = temporary / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I' + str(root / 'src'), str(root / 'src/volume_control.c'),
        str(temporary / 'test.c'), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
