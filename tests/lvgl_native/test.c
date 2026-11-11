#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <lvgl.h>
#include "battery_display.h"
#include "dashboard_lvgl.h"
#include "ui_cards.h"
int dashboard_flush(void){lv_refr_now(NULL);return 0;}
uint32_t test_clock;
static char events[8192];
static int capture(const char *format,...) {
 va_list args;va_start(args,format);
 int n=vsnprintf(events+strlen(events),sizeof(events)-strlen(events),format,args);va_end(args);return n;
}
#define printf capture
#include "wifi_ui.c"
#undef printf
static unsigned flushes;
static uint16_t frame[320][480];
static uint8_t buffers[2][480*16*2];
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *pixels) {
 assert(a->x1>=0 && a->y1>=0 && a->x2<480 && a->y2<320);
 assert((a->x2-a->x1+1)*(a->y2-a->y1+1)*2 <= sizeof(buffers[0]));
 assert(pixels==buffers[0] || pixels==buffers[1]);for(int y=a->y1;y<=a->y2;y++)memcpy(frame[y]+a->x1,pixels+(y-a->y1)*(a->x2-a->x1+1)*2,(a->x2-a->x1+1)*2);
 flushes++;lv_display_flush_ready(d);
}
static void step(void){test_clock+=40;battery_display_tick();}
static void settle(void){for(unsigned i=0;i<7;i++)step();}
static unsigned gesture_session=1000;
static void swipe(int x,int y,int dx,int dy) {
 struct volume_touch t={0};t.session=++gesture_session;t.count=1;t.contacts[0]=(struct volume_contact){x,y,true};
 ui_cards_touch(&t);
 for(int i=1;i<=8;i++){test_clock+=20;t.contacts[0].x=x+dx*i/8;t.contacts[0].y=y+dy*i/8;ui_cards_touch(&t);battery_display_tick();}
 t.count=0;t.contacts[0].down=false;ui_cards_touch(&t);settle();
}
static void three_down(void) {
 struct volume_touch t={0};t.session=++gesture_session;t.count=3;t.multitouch=true;
 const unsigned slots[3]={0,2,5};
 for(unsigned i=0;i<3;i++)t.contacts[slots[i]]=(struct volume_contact){100+(int)i*100,70,true};
 ui_cards_touch(&t);test_clock+=40;
 for(unsigned i=0;i<3;i++)t.contacts[slots[i]].y+=80;
 ui_cards_touch(&t);battery_display_tick();
 /* Holding three contacts across the animation must not retrigger it. */
 settle();ui_cards_touch(&t);assert(!ui_cards_moving());
 t.count=0;for(unsigned i=0;i<3;i++)t.contacts[slots[i]].down=false;
 ui_cards_touch(&t);settle();assert(ui_cards_page()==UI_HOME);
}
static void tap_at(int x,int y) {
 struct volume_touch t={0};t.count=1;t.contacts[0]=(struct volume_contact){x,y,true};
 wifi_ui_touch(&t);step();t.count=0;t.contacts[0].down=false;wifi_ui_touch(&t);step();step();
}
static lv_obj_t *find(lv_obj_t *root,const lv_obj_class_t *type) {
 if(lv_obj_check_type(root,type))return root;
 for(unsigned i=0;i<lv_obj_get_child_count(root);i++){lv_obj_t *p=find(lv_obj_get_child(root,i),type);if(p)return p;}
 return NULL;
}
static bool has_text(lv_obj_t *root,const char *text) {
 if(lv_obj_check_type(root,&lv_label_class) && strstr(lv_label_get_text(root),text))return true;
 for(unsigned i=0;i<lv_obj_get_child_count(root);i++)if(has_text(lv_obj_get_child(root,i),text))return true;
 return false;
}
static void save_frame(const char *path) {
 FILE *image=fopen(path,"wb");assert(image);fprintf(image,"P6\n480 320\n255\n");
 for(unsigned y=0;y<320;y++)for(unsigned x=0;x<480;x++) {
  uint16_t v=frame[y][x];uint8_t rgb[3]={(v>>11)*255/31,((v>>5)&63)*255/63,(v&31)*255/31};fwrite(rgb,1,3,image);
 }fclose(image);
}
int main(void) {
 setvbuf(stdout,NULL,_IONBF,0);puts("init LVGL");
 assert(wifi_ui_lvgl_init(buffers[0],buffers[1],sizeof(buffers[0]),flush)==0);
 dashboard_init();
 assert(battery_display_update(2454)==0);
 assert(battery_display_row(0,"ARMED")==0);
 assert(battery_display_row(1,"wlan1 192.168.20.91")==0);
 assert(battery_display_text(10001,false,"\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x8c\xe5\xb0\x8f\xe8\xbe\x9b\xe5\x90\x8c\xe5\xad\xa6\xe3\x80\x82")==0);
 assert(battery_display_text(10002,true,"\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x81How can I help?")==0);
 battery_display_listening(42,1);test_clock+=200;battery_display_tick();save_frame("home-preview.ppm");
 char long_text[760];memset(long_text,'A',759);long_text[759]=0;
 for(unsigned i=0;i<8;i++)assert(battery_display_text(i+1,i%2,long_text)==0);
 assert(battery_display_text(9,true,"\xe5\xa5\xbd\xe7\x9a\x84\xef\xbc\x8c\xe5\xb0\x8f\xe8\xbe\x9b\xe5\x90\x8c\xe5\xad\xa6\xe3\x80\x82Hello robot.")==0);
 assert(battery_display_listening(42,1)==0);
 uint64_t id=0;assert(battery_display_listening_hit(240,70,&id) && id==42);
 battery_display_volume(75,19200,255);test_clock+=40;battery_display_tick();
 FILE *dash=fopen("dashboard-lvgl.ppm","wb");assert(dash);fprintf(dash,"P6\n480 320\n255\n");
 for(unsigned y=0;y<320;y++)for(unsigned x=0;x<480;x++) {
 uint16_t v=frame[y][x];uint8_t rgb[3]={(v>>11)*255/31,((v>>5)&63)*255/63,(v&31)*255/31};fwrite(rgb,1,3,dash);
 }fclose(dash);
 puts("open panel");tap_at(100,300);settle();wifi_ui_command("mode station");wifi_ui_command("busy 0");
 for(unsigned i=0;i<32;i++) {char c[150];snprintf(c,sizeof(c),"net %u 0 1 80 4e6574776f726b%02x",i,'A'+i%26);assert(wifi_ui_command(c)==0);}
 assert(wifi_ui_command("net 0 0 1 80 e5b08fe8be9b2d57694669")==0);
 puts("render 32 networks");step();puts("rendered");assert(flushes>0);assert(!strstr(events,"EVENT wifi scan"));
 FILE *image=fopen("wifi-lvgl.ppm","wb");assert(image);fprintf(image,"P6\n480 320\n255\n");
 for(unsigned y=0;y<320;y++)for(unsigned x=0;x<480;x++) {
  uint16_t v=frame[y][x];uint8_t rgb[3]={(v>>11)*255/31,((v>>5)&63)*255/63,(v&31)*255/31};fwrite(rgb,1,3,image);
 }
 fclose(image);
 events[0]=0;tap_at(50,282);assert(strstr(events,"EVENT wifi scan"));
 wifi_ui_command("busy 0");wifi_ui_action(WIFI_SELECT,0);step();
 lv_obj_t *text=find(lv_screen_active(),&lv_textarea_class);
 lv_obj_t *keys=find(lv_screen_active(),&lv_keyboard_class);assert(text && keys && wifi_ui_view().keyboard);
 lv_textarea_set_text(text,"pA ss!42");events[0]=0;lv_obj_send_event(keys,LV_EVENT_READY,NULL);step();
 assert(strstr(events,"EVENT wifi connect 0 7041207373213432"));assert(!wifi_ui_view().keyboard);
 assert(!lv_textarea_get_text(text)[0]);
 wifi_ui_command("busy 0");wifi_ui_action(WIFI_SELECT,1);step();
 lv_textarea_set_text(text,"temporary");lv_obj_send_event(keys,LV_EVENT_CANCEL,NULL);step();
 assert(!wifi_ui_view().keyboard && !lv_textarea_get_text(text)[0]);
 wifi_ui_command("busy 0");events[0]=0;tap_at(388,85);assert(strstr(events,"EVENT wifi ap"));
 wifi_ui_command("mode ap");wifi_ui_command("busy 0");step();events[0]=0;tap_at(90,85);assert(strstr(events,"EVENT wifi station"));
 ui_cards_go(UI_HOME);settle();assert(!wifi_ui_active());
 assert(battery_display_listening(42,1)==0);battery_display_tick();
 assert(battery_display_listening_hit(240,70,&id));
 struct volume_touch t={0};t.session=99;t.count=1;t.contacts[0]=(struct volume_contact){40,20,true};
 assert(dashboard_touch(&t));t.count=0;t.contacts[0].down=false;assert(dashboard_touch(&t));
 settle();
 assert(!battery_display_listening_hit(240,70,&id));
 assert(battery_display_row(0,"DISARMED")==0);
 assert(dashboard_robot_reply("123 1 Wake requested.")==0);
 assert(dashboard_robot_reply("123 0 Robot is awake.")==0);
 battery_display_tick();save_frame("robot-controls-preview.ppm");
 t.session++;t.count=1;t.contacts[0]=(struct volume_contact){300,150,true};dashboard_touch(&t);
 test_clock+=200;t.count=0;t.contacts[0].down=false;dashboard_touch(&t);
 assert(has_text(lv_screen_active(),"Wake first, then hold Arm"));
 t.session++;t.count=1;t.contacts[0]=(struct volume_contact){300,150,true};dashboard_touch(&t);
 test_clock+=850;dashboard_touch(&t);t.count=0;t.contacts[0].down=false;dashboard_touch(&t);
 assert(has_text(lv_screen_active(),"Updating state"));
 unsigned long long request=124;
 char reply[120];snprintf(reply,sizeof(reply),"%llu 0 Robot is armed.",request);
 assert(dashboard_robot_reply(reply)==0);assert(battery_display_row(0,"ARMED")==0);
 t.session++;t.count=1;t.contacts[0]=(struct volume_contact){100,150,true};dashboard_touch(&t);
 t.count=0;t.contacts[0].down=false;dashboard_touch(&t);
 assert(has_text(lv_screen_active(),"Disarm before sleeping"));
 t.session++;t.count=1;t.contacts[0]=(struct volume_contact){300,150,true};dashboard_touch(&t);
 test_clock+=20;t.count=0;t.contacts[0].down=false;dashboard_touch(&t);
 assert(has_text(lv_screen_active(),"Updating state"));
 ui_cards_go(UI_HOME);settle();
 assert(battery_display_listening_hit(240,70,&id));
 test_clock+=8001;battery_display_tick();assert(!battery_display_listening_hit(240,70,&id));
 assert(dashboard_robot_reply("125 0 Robot is disarmed.")==0);battery_display_row(0,"DISARMED");
 /* A watchdog stop must expose an explicit Disarm recovery tap. */
 ui_cards_go(UI_ROBOT);settle();
 battery_display_row(0,"DISARM_REQUIRED");
 assert(has_text(lv_screen_active(),"Disarm"));
 t.session++;t.count=1;t.contacts[0]=(struct volume_contact){300,150,true};dashboard_touch(&t);
 test_clock+=20;t.count=0;t.contacts[0].down=false;dashboard_touch(&t);
 assert(has_text(lv_screen_active(),"Updating state"));
 assert(dashboard_robot_reply("126 0 -")==0);battery_display_row(0,"DISARMED");
 ui_cards_go(UI_HOME);settle();
 puts("card gestures");events[0]=0;
 swipe(330,180,-160,0);assert(ui_cards_page()==UI_WIFI);assert(wifi_ui_active());
 assert(!strstr(events,"EVENT wifi scan"));wifi_ui_command("busy 0");
 /* Short horizontal movement snaps back rather than selecting a network. */
 events[0]=0;swipe(200,150,20,0);assert(ui_cards_page()==UI_WIFI);assert(!strstr(events,"connect"));
 wifi_ui_action(WIFI_SELECT,0);step();assert(wifi_ui_view().keyboard);
 text=find(lv_screen_active(),&lv_textarea_class);lv_textarea_set_text(text,"secret");
 swipe(200,140,130,0);assert(ui_cards_page()==UI_WIFI);assert(wifi_ui_view().keyboard);
 three_down();assert(!wifi_ui_active());assert(!wifi_ui_view().keyboard);assert(!lv_textarea_get_text(text)[0]);
 swipe(100,160,160,0);assert(ui_cards_page()==UI_ROBOT);
 assert(dashboard_robot_reply("888 0 Ready.")==0);
 /* A page swipe beginning on Arm cannot become an Arm tap. */
 swipe(300,150,-160,0);assert(ui_cards_page()==UI_HOME);assert(has_text(lv_screen_active(),"Ready."));
 swipe(100,160,160,0);assert(ui_cards_page()==UI_ROBOT);
 struct volume_touch h={0};h.session=++gesture_session;h.count=1;h.contacts[0]=(struct volume_contact){300,150,true};
 ui_cards_touch(&h);test_clock+=40;ui_cards_touch(&h);test_clock+=850;ui_cards_touch(&h);
 h.count=0;h.contacts[0].down=false;ui_cards_touch(&h);assert(has_text(lv_screen_active(),"Updating state."));
 assert(dashboard_robot_reply("889 0 Robot is armed.")==0);battery_display_row(0,"ARMED");
 three_down();
 /* Two contacts cannot invoke Home or resurrect a single-finger tap. */
 swipe(100,160,160,0);assert(ui_cards_page()==UI_ROBOT);
 h=(struct volume_touch){0};h.session=++gesture_session;h.count=2;h.multitouch=true;
 h.contacts[0]=(struct volume_contact){100,60,true};h.contacts[1]=(struct volume_contact){200,60,true};
 ui_cards_touch(&h);h.contacts[0].y+=100;h.contacts[1].y+=100;ui_cards_touch(&h);
 h.count=0;h.contacts[0].down=h.contacts[1].down=false;ui_cards_touch(&h);settle();assert(ui_cards_page()==UI_ROBOT);
 three_down();
 /* Vertical right-half movement remains available to volume control. */
 h=(struct volume_touch){0};h.session=++gesture_session;h.count=1;h.contacts[0]=(struct volume_contact){400,200,true};
 assert(ui_cards_touch(&h));h.contacts[0].y=150;test_clock+=40;assert(!ui_cards_touch(&h));
 h.count=0;h.contacts[0].down=false;ui_cards_touch(&h);
 /* A swipe entirely between display ticks must not become a tap. */
 h=(struct volume_touch){0};h.session=++gesture_session;h.count=1;h.contacts[5]=(struct volume_contact){330,160,true};
 ui_cards_touch(&h);test_clock+=80;h.count=0;h.contacts[5]=(struct volume_contact){170,160,false};
 ui_cards_touch(&h);settle();assert(ui_cards_page()==UI_WIFI);wifi_ui_command("busy 0");three_down();
 puts("PASS: animated cards, short swipe snapback, keyboard lock, three nonconsecutive contact IDs, held-contact latch, two-finger rejection, swipe-over-arm suppression, held Arm and volume routing");
 puts("PASS: dashboard history rollover, mixed CJK/English, listening hit/expiry, overlays and screen return; real LVGL rendering, 32 networks in 64 KiB heap, refresh-only scan, keyboard join/cancel, mode slider and close");
 return 0;
}
