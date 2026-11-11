"""Exercise real touch controls/keyboard without a board or Wi-Fi mutations."""
from pathlib import Path
import subprocess
import os
import tempfile
root = Path(__file__).resolve().parents[1]
harness = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static char events[4096];
static int capture(const char *format, ...) {
    va_list args; va_start(args,format);
    int n=vsnprintf(events+strlen(events),sizeof(events)-strlen(events),format,args);
    va_end(args);return n;
}
#define printf capture
#include "wifi_ui.c"
#undef printf
const unsigned char *display_font_glyph(uint32_t code,unsigned *width) {
    (void)code;static unsigned char blank[32];*width=8;return blank;
}
static void touch(int x,int y,bool down) {
    struct volume_touch t={0};t.count=down;t.contacts[0].down=down;t.contacts[0].x=x;t.contacts[0].y=y;
    wifi_ui_touch(&t);
}
static void tap_at(int x,int y) {touch(x,y,true);touch(x,y,false);}
int main(void) {
    tap_at(100,48);assert(active && strstr(events,"EVENT wifi open"));
    assert(wifi_ui_command("mode station")==0);assert(wifi_ui_command("busy 0")==0);
    assert(wifi_ui_command("net 0 1 1 80 5361766564")==0);
    assert(wifi_ui_command("net 1 0 1 70 4e6577")==0);
    assert(wifi_ui_command("net 32 0 1 70 4e6577")<0);
    events[0]=0;tap_at(25,110);assert(strstr(events,"EVENT wifi connect 0 -"));assert(!keyboard);
    wifi_ui_command("busy 0");events[0]=0;tap_at(25,142);assert(keyboard && !events[0]);
    tap_at(33,180);tap_at(20,282);tap_at(80,180);
    assert(!strcmp(password,"qW"));tap_at(438,282);
    assert(strstr(events,"EVENT wifi connect 1 7157"));assert(!keyboard && !password[0]);
    wifi_ui_command("busy 0");events[0]=0;touch(100,64,true);assert(!events[0]);touch(300,64,true);touch(300,64,false);
    assert(strstr(events,"EVENT wifi ap"));
    wifi_ui_command("busy 0");wifi_ui_command("mode ap");events[0]=0;tap_at(100,64);assert(strstr(events,"EVENT wifi station"));
    wifi_ui_command("busy 0");wifi_ui_command("mode station");events[0]=0;
    struct volume_touch multi={0};multi.count=2;multi.multitouch=true;wifi_ui_touch(&multi);
    multi.count=0;wifi_ui_touch(&multi);
    tap_at(320,64);assert(strstr(events,"EVENT wifi ap"));
    wifi_ui_command("busy 0");wifi_ui_command("mode station");events[0]=0;
    tap_at(280,282);assert(strstr(events,"EVENT wifi scan"));
    tap_at(440,20);assert(!active && !password[0]);
    /* The LVGL adapter uses these model actions rather than legacy hit boxes. */
    tap_at(100,48);wifi_ui_command("busy 0");wifi_ui_command("mode station");
    events[0]=0;wifi_ui_action(WIFI_SELECT,1);assert(wifi_ui_view().keyboard);
    wifi_ui_join("pA ss!42");assert(strstr(events,"EVENT wifi connect 1 7041207373213432"));
    wifi_ui_command("busy 0");events[0]=0;wifi_ui_action(WIFI_SELECT,32);assert(!events[0]);
    wifi_ui_action(WIFI_SCAN,0);assert(strstr(events,"EVENT wifi scan"));
    events[0]=0;wifi_ui_action(WIFI_AP,0);assert(!events[0]); /* busy blocks mode changes */
    wifi_ui_action(WIFI_CLOSE,0);assert(!wifi_ui_active() && !password[0]);
    puts("PASS: LVGL model actions, saved/new network selection, masked keyboard state, mixed-case encoding, slider release and password clearing");
}
'''
with tempfile.TemporaryDirectory() as directory:
    temporary = Path(directory)
    (temporary / 'test.c').write_text(harness)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(root/'src'),
                    str(temporary/'test.c'), '-o', str(temporary/'test')],check=True)
    subprocess.run([str(temporary/'test')],check=True)
