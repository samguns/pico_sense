/* SPDX-License-Identifier: Apache-2.0 */
/* Gesture arbitration and object translation only: no framebuffer or extra DMA. */
#include "ui_cards.h"
#include "dashboard_lvgl.h"
#include "battery_display.h"
#include "wifi_ui.h"
#include <zephyr/kernel.h>
#include <stdlib.h>
#include <stdio.h>

enum gesture { IDLE, DECIDING, HORIZONTAL, VERTICAL, THREE, BLOCKED };
static lv_obj_t *pages[3],*dots[3];
static enum ui_page page=UI_HOME,target=UI_HOME;
static enum gesture gesture;
static unsigned session,three_slots[3];
static int start_x,start_y,last_x,last_y,offset,from,to,three_x[3],three_y[3];
static uint32_t last_at,animation_at;
static int indicator_target=-1;
static uint32_t started_at;
static int velocity;
static bool moving,home_drop,three_seen;
static struct volume_touch initial;

static void layout(int shift)
{
    for(int i=0;i<3;i++) {
        lv_obj_set_pos(pages[i],(i-(int)page)*480+shift,0);
        if(indicator_target==(int)target)continue;
        lv_obj_set_style_bg_color(dots[i],lv_color_hex(i==(int)target ? 0x65d9d0 : 0x34414b),0);
        lv_obj_set_width(dots[i],i==(int)target ? 18 : 5);
        lv_obj_move_foreground(dots[i]);
    }
    indicator_target=target;
}
static void settled(void)
{
    page=target;moving=home_drop=false;offset=0;layout(0);
    dashboard_set_control_page(page==UI_ROBOT);
    if(page==UI_WIFI)wifi_ui_action(WIFI_OPEN,0);
    else if(wifi_ui_active())wifi_ui_action(WIFI_CLOSE,0);
}
static void animate(enum ui_page next,bool drop)
{
    target=next;home_drop=drop;from=offset;
    to=drop ? 320 : ((int)page-(int)next)*480;
    animation_at=k_uptime_get_32();moving=true;
    wifi_ui_lvgl_cancel_touch();dashboard_cancel_touch();
}
void ui_cards_go(enum ui_page next)
{
    if(!pages[0] || next>UI_WIFI)return;
    if(moving)return;
    if(next==page){settled();return;}
    offset=0;animate(next,false);
}
enum ui_page ui_cards_page(void){return page;}
bool ui_cards_moving(void){return moving;}
void ui_cards_init(lv_obj_t *canvas,lv_obj_t *robot,lv_obj_t *home,lv_obj_t *wifi)
{
    pages[0]=robot;pages[1]=home;pages[2]=wifi;
    for(unsigned i=0;i<3;i++) {
        lv_obj_set_parent(pages[i],canvas);lv_obj_set_size(pages[i],480,320);
        lv_obj_remove_flag(pages[i],LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_radius(pages[i],14,0);
        lv_obj_set_style_border_width(pages[i],1,0);
        lv_obj_set_style_border_color(pages[i],lv_color_hex(0x27323c),0);
        dots[i]=lv_obj_create(canvas);lv_obj_remove_flag(dots[i],LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(dots[i],5,4);lv_obj_set_pos(dots[i],218+i*18,315);
        lv_obj_set_style_border_width(dots[i],0,0);lv_obj_set_style_radius(dots[i],2,0);
        lv_obj_set_style_pad_all(dots[i],0,0);
    }
    layout(0);dashboard_set_control_page(false);
}
void ui_cards_tick(void)
{
    if(!pages[0])return;
    if(moving) {
        unsigned elapsed=k_uptime_get_32()-animation_at;
        if(elapsed>=220){settled();return;}
        int remaining=220-elapsed;
        int eased=1000-(remaining*remaining*remaining)/(220*220*220/1000);
        int position=from+(to-from)*eased/1000;
        if(home_drop) {
            lv_obj_set_pos(pages[UI_HOME],0,0);
            if(page!=UI_HOME)lv_obj_set_pos(pages[page],0,position);
            else lv_obj_set_y(pages[UI_HOME],position*(320-position)/320);
        } else layout(position);
    } else if(gesture==IDLE) {
        /* Keep compatibility with host/model-initiated panel opening. */
        if(wifi_ui_active() && page!=UI_WIFI)ui_cards_go(UI_WIFI);
        else if(!wifi_ui_active() && page==UI_WIFI)ui_cards_go(UI_HOME);
    }
}
static void cancel_widgets(const struct volume_touch *touch)
{
    (void)touch;wifi_ui_lvgl_cancel_touch();dashboard_cancel_touch();
}
static bool route_vertical(const struct volume_touch *touch)
{
    if(page==UI_WIFI){wifi_ui_lvgl_touch(touch);return true;}
    return dashboard_touch(touch);
}
bool ui_cards_touch(const struct volume_touch *touch)
{
    if(!pages[0])return false;
    uint32_t now=k_uptime_get_32();
    if(touch->session!=session){session=touch->session;gesture=IDLE;three_seen=false;}
    if(moving){gesture=BLOCKED;return true;}
    if(gesture==BLOCKED && touch->count)return true;
    if(touch->count>1 && gesture==HORIZONTAL){offset=0;layout(0);}
    if(touch->count>3){cancel_widgets(touch);gesture=BLOCKED;return true;}
    if(touch->count==3 && gesture!=BLOCKED) {
        if(!three_seen) {
            cancel_widgets(touch);three_seen=true;gesture=THREE;
            unsigned j=0;for(unsigned i=0;i<VOLUME_TOUCH_SLOTS && j<3;i++)if(touch->contacts[i].down) {
                three_slots[j]=i;three_x[j]=touch->contacts[i].x;three_y[j]=touch->contacts[i].y;j++;
            }
            offset=0;
        }
        int sum=0,min=1000,max=-1000;
        for(unsigned j=0;j<3;j++) {
            const struct volume_contact *c=&touch->contacts[three_slots[j]];
            if(!c->down){gesture=BLOCKED;return true;}
            int dy=c->y-three_y[j],dx=c->x-three_x[j];
            if(abs(dx)>45 || dy< -15){gesture=BLOCKED;layout(0);return true;}
            if(dy<min)min=dy;
            if(dy>max)max=dy;
            sum+=dy;
        }
        if(min>=55 && max-min<=40) {
            offset=sum/3;lv_obj_move_foreground(pages[page]);
            if(page!=UI_HOME){lv_obj_set_pos(pages[UI_HOME],0,0);lv_obj_set_y(pages[page],offset);}
            animate(UI_HOME,true);gesture=BLOCKED;
        }
        return true;
    }
    if(touch->count>1) {
        cancel_widgets(touch);
        /* Two fingers may be the intermediate frame while placing three. */
        if(gesture!=THREE)gesture=DECIDING;
        return true;
    }
    if(three_seen && touch->count){gesture=BLOCKED;return true;}
    if(touch->count==1 && touch->multitouch){gesture=BLOCKED;return true;}
    int x=last_x,y=last_y;
    for(unsigned i=0;i<VOLUME_TOUCH_SLOTS;i++)if(touch->contacts[i].down){x=touch->contacts[i].x;y=touch->contacts[i].y;break;}
    if(!touch->count) {
        bool consumed=true;
        /* A full swipe can occur during one SPI refresh. Use the retained
         * release coordinates, so it cannot be mistaken for a button tap. */
        if((gesture==DECIDING || gesture==HORIZONTAL) && !touch->multitouch) {
            for(unsigned i=0;i<VOLUME_TOUCH_SLOTS;i++)if(initial.contacts[i].down) {
                int dx=touch->contacts[i].x-start_x,dy=touch->contacts[i].y-start_y;
                bool local=page==UI_WIFI && (wifi_ui_view().keyboard ||
                    (start_y>=60 && start_y<100 && start_x>=70 && start_x<410));
                if(!local && abs(dx)>=12 && abs(dx)>abs(dy)*3/2) {
                    if(gesture!=HORIZONTAL)cancel_widgets(touch);
                    gesture=HORIZONTAL;offset=dx;
                    if((page==UI_ROBOT && dx>0) || (page==UI_WIFI && dx<0))offset=dx/4;
                    unsigned dt=now-started_at;if(dt)velocity=dx*1000/(int)dt;
                    layout(offset);
                } else if(abs(dx)>=18 || abs(dy)>=18) {
                    if(gesture==DECIDING) {
                        gesture=VERTICAL;route_vertical(&initial);
                        struct volume_touch end=*touch;end.count=1;end.contacts[i].down=true;
                        route_vertical(&end);
                    }
                }
                break;
            }
        }
        if(gesture==HORIZONTAL) {
            int next=page;
            if(abs(offset)>=70 || (abs(offset)>=24 && abs(velocity)>=500))next+=offset<0 ? 1 : -1;
            if(next<UI_ROBOT)next=UI_ROBOT;
            if(next>UI_WIFI)next=UI_WIFI;
            animate(next,false);
        } else if(gesture==VERTICAL)consumed=route_vertical(touch);
        else if(gesture==DECIDING && !touch->multitouch) {
            if(page==UI_WIFI) {
                wifi_ui_lvgl_touch(&initial);wifi_ui_lvgl_touch(touch);
            } else {
                dashboard_touch(&initial);dashboard_touch(touch);
                uint64_t id;
                if(page==UI_HOME && battery_display_listening_hit(start_x,start_y,&id))
                    printf("\nEVENT quit %llu\n",(unsigned long long)id);
            }
        }
        gesture=IDLE;three_seen=false;return consumed;
    }
    if(gesture==BLOCKED || gesture==THREE)return true;
    if(gesture==IDLE) {
        initial=*touch;start_x=last_x=x;start_y=last_y=y;last_at=started_at=now;velocity=0;offset=0;gesture=DECIDING;
        return true;
    }
    int dx=x-start_x,dy=y-start_y;
    if(gesture==DECIDING) {
        bool widget_drag=page==UI_WIFI && (wifi_ui_view().keyboard ||
            (start_y>=60 && start_y<100 && start_x>=70 && start_x<410));
        if(widget_drag && (abs(dx)>=12 || abs(dy)>=12)) {
            gesture=VERTICAL;route_vertical(&initial);
        } else if(!widget_drag && abs(dx)>=12 && abs(dx)>abs(dy)*3/2) {
            gesture=HORIZONTAL;cancel_widgets(touch);
        } else if(abs(dy)>=12 && abs(dy)>abs(dx)*3/2) {
            gesture=VERTICAL;route_vertical(&initial);
        } else if(page==UI_ROBOT && abs(dx)<12 && abs(dy)<12) {
            /* Hold feedback may draw now; robot actions still require release. */
            if(now-started_at>=40){dashboard_touch(&initial);dashboard_touch(touch);}
        }
    }
    if(gesture==HORIZONTAL) {
        if(now!=last_at)velocity=(x-last_x)*1000/(int)(now-last_at);
        offset=dx;
        if((page==UI_ROBOT && dx>0) || (page==UI_WIFI && dx<0))offset=dx/4;
        if(offset>480)offset=480;
        if(offset< -480)offset=-480;
        layout(offset);
    }
    bool consumed=gesture==VERTICAL ? route_vertical(touch) : true;
    last_x=x;last_y=y;last_at=now;return consumed;
}
