/* SPDX-License-Identifier: Apache-2.0 */
/* Model and LVGL objects belong to the console thread, including animations. */
#include "battery_display.h"
#include "dashboard_lvgl.h"
#include "wifi_ui.h"
#include "ui_cards.h"
#include <zephyr/kernel.h>
#include <lvgl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const lv_font_t network_font, dashboard_font;
static lv_obj_t *root,*state,*voltage,*network,*history,*texts[8];
static lv_obj_t *volume,*fill,*volume_label,*listen,*listen_label,*waves[11];
static uint64_t ids[8],listening_id;
static bool roles[8];
static unsigned count,phase,alpha;
static uint32_t last_status,last_listen,last_tick;
static bool status_valid;
static lv_obj_t *controls,*control_state,*power_button,*arm_button,*power_text,*arm_text,*feedback,*hold_bar;
static char robot_state[79]="STATE UNAVAILABLE";
static bool control_open,robot_pending,touch_tracking,touch_blocked;
static unsigned touch_session;
static int start_x,start_y,last_y;
static uint32_t pressed_at;
static uint64_t request_id;
static lv_obj_t *latest;
static bool reviewing;
static int last_x;
static void control_refresh(void);
static lv_obj_t *label(lv_obj_t *parent,const char *value,int x,int y,int width)
{
    lv_obj_t *obj=lv_label_create(parent);lv_obj_set_pos(obj,x,y);
    lv_obj_set_width(obj,width);lv_label_set_text(obj,value);return obj;
}
static void visible(lv_obj_t *obj,bool show)
{
    if(show)lv_obj_remove_flag(obj,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj,LV_OBJ_FLAG_HIDDEN);
}
static lv_obj_t *panel(lv_obj_t *parent,int x,int y,int w,int h)
{
    lv_obj_t *obj=lv_obj_create(parent);lv_obj_set_pos(obj,x,y);lv_obj_set_size(obj,w,h);
    lv_obj_remove_flag(obj,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(obj,0,0);lv_obj_set_style_pad_all(obj,0,0);
    lv_obj_set_style_radius(obj,18,0);lv_obj_set_style_bg_color(obj,lv_color_hex(0x202830),0);
    return obj;
}
/* Original 32x24 speaker silhouette, drawn directly without a symbol font. */
static const uint8_t speaker_bits[96]={
    0x00,0x00,0x00,0x00,0x00,0x00,0x0e,0x00,0x00,0x00,0x47,0x80,0x00,0x00,0xc1,0xc0,
    0x00,0x01,0xc0,0xe0,0x00,0x07,0xc0,0x60,0x00,0x0f,0xcc,0x30,0x00,0x1f,0xce,0x10,
    0x1f,0xff,0xc6,0x18,0x1f,0xff,0xc3,0x18,0x1f,0xff,0xc3,0x08,0x1f,0xff,0xc1,0x08,
    0x1f,0xff,0xc1,0x08,0x1f,0xff,0xc3,0x08,0x1f,0xff,0xc3,0x18,0x1f,0xff,0xc6,0x18,
    0x00,0x1f,0xce,0x10,0x00,0x0f,0xcc,0x30,0x00,0x07,0xc0,0x60,0x00,0x01,0xc0,0xe0,
    0x00,0x00,0xc1,0xc0,0x00,0x00,0x47,0x80,0x00,0x00,0x0e,0x00,0x00,0x00,0x00,0x00
};
/* Draw the original bitmap exactly once. This avoids text wrapping and
 * glyph fallback without changing the speaker's pixels or alignment. */
static void draw_speaker(lv_event_t *event)
{
    lv_obj_t *obj=lv_event_get_target_obj(event);
    lv_layer_t *layer=lv_event_get_layer(event);
    lv_area_t bounds;lv_obj_get_coords(obj,&bounds);
    lv_draw_rect_dsc_t rect;lv_draw_rect_dsc_init(&rect);
    rect.bg_color=lv_color_hex(0xfffbff);rect.bg_opa=LV_OPA_COVER;
    for(unsigned y=0;y<24;y++) {
        unsigned x=0;
        while(x<32) {
            while(x<32 && !(speaker_bits[y*4+x/8] & (0x80U>>(x%8))))x++;
            unsigned start=x;
            while(x<32 && (speaker_bits[y*4+x/8] & (0x80U>>(x%8))))x++;
            if(x>start) {
                lv_area_t area={bounds.x1+12+start,bounds.y1+y,
                                bounds.x1+12+x-1,bounds.y1+y};
                lv_draw_rect(layer,&rect,&area);
            }
        }
    }
}

void dashboard_init(void)
{
    lv_obj_t *canvas=lv_screen_active();lv_obj_remove_flag(canvas,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(canvas,lv_color_hex(0x030609),0);
    root=panel(canvas,0,0,480,320);lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root,lv_color_hex(0x080c10),0);
    lv_obj_set_style_text_color(root,lv_color_hex(0xeeeeee),0);
    lv_obj_t *badge=panel(root,8,6,154,38);
    state=label(badge,"UNAVAILABLE",8,11,142);
    voltage=label(root,"--.-- V",204,16,110);
    lv_obj_set_style_text_font(voltage,&dashboard_font,0);
    lv_obj_set_style_text_align(voltage,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_font(voltage,&lv_font_montserrat_14,0);
    lv_obj_t *battery=panel(root,176,16,24,13);
    lv_obj_set_style_radius(battery,3,0);lv_obj_set_style_border_width(battery,2,0);
    lv_obj_set_style_border_color(battery,lv_color_hex(0x75dfa9),0);
    lv_obj_t *charge=panel(battery,3,3,15,7);lv_obj_set_style_radius(charge,1,0);
    lv_obj_set_style_bg_color(charge,lv_color_hex(0x75dfa9),0);
    lv_obj_t *terminal=panel(root,200,20,3,5);lv_obj_set_style_radius(terminal,1,0);
    lv_obj_set_style_bg_color(terminal,lv_color_hex(0x75dfa9),0);
    network=label(root,"",16,293,448);lv_obj_set_style_text_font(network,&network_font,0);
    lv_obj_t *hint=label(root,"WI-FI",402,17,72);
    lv_obj_set_style_text_color(hint,lv_color_hex(0x00ffff),0);
    history=lv_obj_create(root);lv_obj_set_pos(history,16,106);lv_obj_set_size(history,448,180);
    lv_obj_set_style_bg_opa(history,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(history,0,0);
    lv_obj_set_style_pad_all(history,0,0);lv_obj_set_style_pad_row(history,5,0);
    lv_obj_set_flex_flow(history,LV_FLEX_FLOW_COLUMN);lv_obj_set_scrollbar_mode(history,LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(history,LV_OBJ_FLAG_SCROLLABLE);
    for(unsigned i=0;i<8;i++) {
        texts[i]=label(history,"",0,0,456);lv_obj_set_style_text_font(texts[i],&dashboard_font,0);
        lv_obj_set_style_text_line_space(texts[i],4,0);visible(texts[i],false);
    }
    latest=panel(root,386,260,78,26);label(latest,"Latest",12,5,65);visible(latest,false);
    volume=panel(root,414,34,56,268);volume_label=label(volume,"25%",0,12,56);
    lv_obj_set_style_text_align(volume_label,LV_TEXT_ALIGN_CENTER,0);
    fill=lv_bar_create(volume);lv_obj_set_pos(fill,10,32);lv_obj_set_size(fill,36,198);
    lv_bar_set_range(fill,0,25600);lv_bar_set_value(fill,6400,LV_ANIM_OFF);
    lv_obj_set_style_bg_color(fill,lv_color_hex(0xffffff),LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(fill,lv_color_hex(0x636363),LV_PART_MAIN);
    lv_obj_t *icon=panel(volume,0,237,56,24);
    lv_obj_set_style_bg_opa(icon,LV_OPA_TRANSP,0);
    lv_obj_add_event_cb(icon,draw_speaker,LV_EVENT_DRAW_MAIN,NULL);
    visible(volume,false);
    listen=panel(root,112,52,256,46);listen_label=label(listen,"LISTENING - TAP TO STOP",0,5,256);
    lv_obj_set_style_text_align(listen_label,LV_TEXT_ALIGN_CENTER,0);
    for(unsigned i=0;i<11;i++)waves[i]=panel(listen,75+i*10,30,6,8);
    visible(listen,false);last_status=k_uptime_get_32();status_valid=true;
    controls=panel(canvas,0,0,480,320);lv_obj_set_style_radius(controls,0,0);
    lv_obj_set_style_text_color(controls,lv_color_hex(0xeeeeee),0);
    lv_obj_set_style_bg_color(controls,lv_color_hex(0x080c10),0);
    label(controls,"< SWIPE >",16,18,100);label(controls,"ROBOT CONTROL",166,18,230);
    control_state=label(controls,"UNAVAILABLE",24,68,432);
    lv_obj_set_style_text_align(control_state,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_font(control_state,&lv_font_montserrat_24,0);
    power_button=panel(controls,20,112,212,96);power_text=label(power_button,"Wake",0,34,212);
    arm_button=panel(controls,248,112,212,96);arm_text=label(arm_button,"Arm",0,34,212);
    lv_obj_set_style_text_align(power_text,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_align(arm_text,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_font(power_text,&lv_font_montserrat_24,0);
    lv_obj_set_style_text_font(arm_text,&lv_font_montserrat_24,0);
    lv_obj_set_style_bg_color(power_button,lv_color_hex(0x163b43),0);
    hold_bar=lv_bar_create(controls);lv_obj_set_pos(hold_bar,260,194);lv_obj_set_size(hold_bar,188,4);
    lv_bar_set_range(hold_bar,0,800);visible(hold_bar,false);
    feedback=label(controls,"",24,232,432);
    lv_obj_set_style_text_font(feedback,&network_font,0);
    label(controls,"Hold Arm to confirm. Disarm responds to a tap.",24,286,432);
    ui_cards_init(canvas,controls,root,wifi_ui_lvgl_card());
}
void dashboard_set_control_page(bool active){control_open=active;control_refresh();}
void dashboard_cancel_touch(void){touch_tracking=false;touch_blocked=true;visible(hold_bar,false);}
static bool can_disarm(void)
{
    return strcmp(robot_state,"ARMED")==0 || strcmp(robot_state,"ARMING")==0 ||
           strcmp(robot_state,"DISARM_REQUIRED")==0;
}
static void control_refresh(void)
{
    bool sleep=strcmp(robot_state,"SLEEP")==0, disarmed=strcmp(robot_state,"DISARMED")==0;
    bool armed=strcmp(robot_state,"ARMED")==0;
    lv_label_set_text(control_state,robot_state);
    lv_obj_set_style_text_color(control_state,lv_color_hex(armed ? 0xffb454 : 0x75dfa9),0);
    lv_label_set_text(power_text,sleep ? "Wake" : "Sleep");
    lv_label_set_text(arm_text,can_disarm() ? "Disarm" : "Hold to arm");
    lv_obj_set_style_bg_color(arm_button,lv_color_hex(armed ? 0x493124 : 0x163b43),0);
    lv_obj_set_style_opa(power_button,!robot_pending && (sleep || disarmed) ? 255 : 85,0);
    lv_obj_set_style_opa(arm_button,!robot_pending && (can_disarm() || disarmed) ? 255 : 85,0);
}
int dashboard_robot_reply(const char *reply)
{
    unsigned long long id;unsigned busy;int offset=0;
    if(sscanf(reply,"%llu %u %n",&id,&busy,&offset)!=2 || !offset || busy>1)return -EINVAL;
    if(robot_pending && id!=request_id)return 0;
    request_id=id;robot_pending=busy;
    lv_label_set_text(feedback,strcmp(reply+offset,"-")==0 ? "" : reply+offset);control_refresh();return 0;
}
static void request_robot(const char *action)
{
    if(robot_pending)return;
    /* Host feedback seeds this counter, including after Pico-only reboot. */
    request_id++;
    robot_pending=true;lv_label_set_text(feedback,"Updating state...");
    control_refresh();printf("\nEVENT robot %llu %s\n",(unsigned long long)request_id,action);
}
bool dashboard_touch(const struct volume_touch *touch)
{
    if(wifi_ui_active())return false;
    if(touch->session!=touch_session){touch_session=touch->session;touch_tracking=false;touch_blocked=false;}
    if(touch->multitouch || touch->count>1)touch_blocked=true;
    int x=0,y=0;for(unsigned i=0;i<VOLUME_TOUCH_SLOTS;i++)if(touch->contacts[i].down){x=touch->contacts[i].x;y=touch->contacts[i].y;break;}
    bool consumed=control_open;
    if(touch->count==1 && !touch_blocked) {
        if(!touch_tracking){touch_tracking=true;start_x=x;start_y=last_y=y;pressed_at=k_uptime_get_32();}
        if(!control_open && start_x<164 && start_y<48)consumed=true;
        if(!control_open && reviewing && start_x>=386 && start_y>=260 && start_y<290)consumed=true;
        if(!control_open && start_x<240 && start_y>=106 && start_y<286) {
            lv_obj_scroll_by(history,0,y-last_y,LV_ANIM_OFF);consumed=true;
        }
        last_y=y;last_x=x;
        if(!control_open && start_x<240 && start_y>=106 && start_y<260 && abs(y-start_y)>8) {
            reviewing=true;visible(latest,true);
        }
        bool hold=control_open && !robot_pending && strcmp(robot_state,"DISARMED")==0 &&
            start_x>=248 && start_y>=112 && start_y<208 && x>=248 && y>=112 && y<208;
        visible(hold_bar,hold);if(hold)lv_bar_set_value(hold_bar,k_uptime_get_32()-pressed_at,LV_ANIM_OFF);
    } else if(!touch->count && touch_tracking) {
        touch_tracking=false;visible(hold_bar,false);
        if(!touch_blocked && control_open && last_x-start_x>100 && abs(last_y-start_y)<50) {
            ui_cards_go(UI_HOME);
        } else if(!touch_blocked && abs(last_y-start_y)<18 && abs(last_x-start_x)<18) {
            if(!control_open && start_x<164 && start_y<48){ui_cards_go(UI_ROBOT);consumed=true;}
            else if(!control_open && reviewing && start_x>=386 && start_y>=260 && start_y<290) {
                reviewing=false;visible(latest,false);lv_obj_scroll_to_y(history,lv_obj_get_scroll_bottom(history)+lv_obj_get_scroll_y(history),LV_ANIM_OFF);consumed=true;
            }
            else if(control_open && !robot_pending && start_y>=112 && start_y<208) {
                if(start_x>=20 && start_x<232) {
                    if(strcmp(robot_state,"SLEEP")==0)request_robot("wake");
                    else if(strcmp(robot_state,"DISARMED")==0)request_robot("sleep");
                    else lv_label_set_text(feedback,"Disarm before sleeping.");
                } else if(start_x>=248 && start_x<460) {
                    if(can_disarm())request_robot("disarm");
                    else if(strcmp(robot_state,"DISARMED")==0 && k_uptime_get_32()-pressed_at>=800)request_robot("arm");
                    else lv_label_set_text(feedback,"Wake first, then hold Arm to confirm.");
                }
            }
        }
    }
    return consumed;
}
static void status_seen(void){last_status=k_uptime_get_32();status_valid=true;}
int battery_display_update(unsigned int centivolts)
{
    if(!root)return -ENODEV;
    status_seen();
    lv_label_set_text_fmt(voltage,"%02u.%02u V",centivolts/100,centivolts%100);
    return dashboard_flush();
}
int battery_display_unavailable(void)
{
    if(!root)return -ENODEV;
    status_seen();lv_label_set_text(voltage,"--.-- V");return dashboard_flush();
}
int battery_display_row(unsigned int row,const char *text)
{
    if(!root)return -ENODEV;
    if(!text || row>=6 || strlen(text)>=79)return -EINVAL;
    status_seen();if(row>=2)return 0;
    lv_obj_t *obj=row ? network : state;lv_label_set_text(obj,text);
    if(!row) {
        snprintf(robot_state,sizeof(robot_state),"%s",text);control_refresh();
        bool armed=strcmp(text,"ARMED")==0;
        bool fault=strcmp(text,"FAULT")==0 || strcmp(text,"EMERGENCY_STOP")==0;
        const char *caption=text;
        if(strcmp(text,"STATE UNAVAILABLE")==0)caption="UNAVAILABLE";
        if(strcmp(text,"EMERGENCY_STOP")==0)caption="E-STOP";
        if(strcmp(text,"SLEEPING_DOWN")==0)caption="SLEEPING";
        if(strcmp(text,"DISARM_REQUIRED")==0)caption="DISARM NEEDED";
        lv_label_set_text(state,caption);
        lv_obj_set_style_text_color(state,lv_color_hex(fault ? 0xff7070 : armed ? 0xffb454 : 0xeeeeee),0);
        lv_obj_set_style_text_font(state,&lv_font_montserrat_14,0);
    }
    return dashboard_flush();
}
int battery_display_text(uint64_t id,bool spoken,const char *text)
{
    if(!root)return -ENODEV;
    if(!text || strlen(text)>=768)return -EINVAL;
    unsigned i;for(i=0;i<count;i++)if(ids[i]==id)break;
    if(i<count && roles[i]==spoken && strcmp(lv_label_get_text(texts[i]),text)==0)
        return dashboard_flush();
    if(i==8) {
        for(unsigned j=0;j<7;j++) {
            ids[j]=ids[j+1];roles[j]=roles[j+1];
            lv_label_set_text(texts[j],lv_label_get_text(texts[j+1]));
            lv_obj_set_style_text_color(texts[j],lv_color_hex(roles[j] ? 0x00ff00 : 0xeeeeee),0);
        }
        count--;i--;
    }
    if(i==count)count++;
    ids[i]=id;roles[i]=spoken;visible(texts[i],true);lv_label_set_text(texts[i],text);
    lv_obj_set_style_text_color(texts[i],lv_color_hex(spoken ? 0x00ff00 : 0xeeeeee),0);
    /* Keep latest complete turns in view. Scroll the clipped container without
     * introducing another touch gesture that conflicts with volume control. */
    lv_obj_update_layout(history);if(reviewing)return dashboard_flush();
    lv_obj_scroll_to_y(history,0,LV_ANIM_OFF);
    lv_obj_update_layout(history);
    int bottom=lv_obj_get_y(texts[count-1])+lv_obj_get_height(texts[count-1]);
    lv_obj_scroll_to_y(history,bottom>180 ? bottom-180 : 0,LV_ANIM_OFF);
    return dashboard_flush();
}
int battery_display_volume(unsigned int percent,unsigned int level_q8,unsigned int opacity)
{
    if(!root)return -ENODEV;
    if(percent>100)percent=100;
    if(level_q8>25600)level_q8=25600;
    if(opacity>255)opacity=255;
    visible(volume,opacity!=0);lv_obj_set_style_opa(volume,opacity,0);
    lv_bar_set_value(fill,level_q8,LV_ANIM_OFF);lv_label_set_text_fmt(volume_label,"%u%%",percent);
    /* Existing volume controller supplies the eased fill and fade values. */
    return 0;
}
int battery_display_listening(uint64_t id,unsigned int value)
{
    if(!root)return -ENODEV;
    if(value>5 || (!id && value))return -EINVAL;
    listening_id=id;phase=value;last_listen=k_uptime_get_32();return 0;
}
bool battery_display_listening_hit(int x,int y,uint64_t *id)
{
    if(ui_cards_page()!=UI_HOME || ui_cards_moving() || wifi_ui_active() || !listening_id || !phase || phase==5 ||
       (uint32_t)(k_uptime_get_32()-last_listen)>=8000)return false;
    if(x<100 || x>=380 || y<48 || y>=104)return false;
    *id=listening_id;return true;
}
void battery_display_tick(void)
{
    if(!root)return;
    wifi_ui_lvgl_tick(wifi_ui_dirty());
    ui_cards_tick();
    uint32_t now=k_uptime_get_32();
    if(status_valid && (uint32_t)(now-last_status)>=8000) {
        lv_label_set_text(state,"STATUS UNAVAILABLE");lv_obj_set_style_text_font(state,&lv_font_montserrat_14,0);lv_obj_set_style_text_color(state,lv_color_hex(0xeeeeee),0);
        lv_label_set_text(voltage,"--.-- V");lv_label_set_text(network,"");status_valid=false;
        strcpy(robot_state,"UNAVAILABLE");control_refresh();
    }
    if((uint32_t)(now-last_tick)>=40) {
        bool active=listening_id && phase && (uint32_t)(now-last_listen)<8000;
        unsigned elapsed=now-last_tick;if(elapsed>100)elapsed=100;
        unsigned step=elapsed*255/(active ? 180 : 240);
        alpha=active ? (alpha+step>255 ? 255 : alpha+step) : (alpha>step ? alpha-step : 0);
        last_tick=now;visible(listen,alpha!=0);lv_obj_set_style_opa(listen,alpha,0);
        lv_obj_set_y(listen,52-(255-(int)alpha)*8/255);
        static const char *labels[]={"","LISTENING - TAP TO STOP","THINKING","SPEAKING","PAUSED","CLOSING"};
        lv_label_set_text(listen_label,labels[phase]);
        lv_color_t color=lv_color_hex(phase==1 ? 0x00ffff : phase==3 ? 0x00ff00 : 0xb080ff);
        lv_obj_set_style_text_color(listen_label,color,0);
        for(unsigned i=0;i<11;i++) {
            int pulse=(now/65+i*3)%24;int half=phase==4 ? 3 : 3+(pulse>12 ? pulse-12 : 12-pulse)/2;
            lv_obj_set_pos(waves[i],75+i*10,34-half);lv_obj_set_height(waves[i],half*2);
            lv_obj_set_style_bg_color(waves[i],color,0);
        }
    }
    lv_timer_handler();dashboard_flush();
}
