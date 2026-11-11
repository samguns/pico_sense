/* SPDX-License-Identifier: Apache-2.0 */
/* LVGL owns only the Wi-Fi panel; all calls except flush-ready run on the
 * console thread. The existing display worker owns SPI/DMA transfers. */
#include "wifi_ui.h"
#include "display_font.h"
#include <zephyr/kernel.h>
#include <lvgl_mem.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static lv_display_t *display;
static lv_indev_t *pointer;
static lv_obj_t *dashboard;
static lv_obj_t *screen, *slider, *net_list, *status_label, *ap_label;
static lv_obj_t *ssid_label, *entry, *keyboard, *refresh, *password_button;
static bool shown, editing, pointer_blocked;
static lv_indev_data_t input;
static unsigned input_session;

/* Reuse the existing bounded Unifont glyph source, including CJK glyphs. */
static bool glyph_descriptor(const lv_font_t *font, lv_font_glyph_dsc_t *out,
                             uint32_t code, uint32_t next)
{
    (void)font; (void)next;
    if (code < 32) return false;
    unsigned width; display_font_glyph(code, &width);
    out->adv_w=width; out->box_w=width; out->box_h=16;
    out->ofs_x=0; out->ofs_y=0; out->format=LV_FONT_GLYPH_FORMAT_A1;
    out->stride=2; out->gid.index=code;
    return true;
}
static const void *glyph_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *buf)
{
    unsigned width; const uint8_t *bits=display_font_glyph(glyph->gid.index,&width);
    if (glyph->req_raw_bitmap) return bits;
    unsigned stride=lv_draw_buf_width_to_stride(width,LV_COLOR_FORMAT_A8);
    for(unsigned y=0;y<16;y++) for(unsigned x=0;x<width;x++)
        buf->data[y*stride+x]=(bits[y*2+x/8] & (0x80U>>(x%8))) ? 255 : 0;
    return buf;
}
const lv_font_t network_font={.get_glyph_dsc=glyph_descriptor,
    .get_glyph_bitmap=glyph_bitmap,.line_height=16,.base_line=0};

/* Twice-sized glyphs preserve the dashboard's 32-pixel CJK text without
 * storing a second font or allocating a framebuffer. */
static bool large_descriptor(const lv_font_t *font,lv_font_glyph_dsc_t *out,uint32_t code,uint32_t next)
{
    if(!glyph_descriptor(font,out,code,next))return false;
    out->adv_w*=2;out->box_w*=2;out->box_h*=2;
    out->format=LV_FONT_GLYPH_FORMAT_A8;out->stride=out->box_w;return true;
}
static const void *large_bitmap(lv_font_glyph_dsc_t *glyph,lv_draw_buf_t *buf)
{
    unsigned width;const uint8_t *bits=display_font_glyph(glyph->gid.index,&width);
    unsigned stride=lv_draw_buf_width_to_stride(width*2,LV_COLOR_FORMAT_A8);
    for(unsigned y=0;y<32;y++)for(unsigned x=0;x<width*2;x++)
        buf->data[y*stride+x]=(bits[(y/2)*2+x/16] & (0x80U>>((x/2)%8))) ? 255 : 0;
    return buf;
}
const lv_font_t dashboard_font={.get_glyph_dsc=large_descriptor,
    .get_glyph_bitmap=large_bitmap,.line_height=32,.base_line=0};

static void hidden(lv_obj_t *obj,bool hide)
{
    if(hide) lv_obj_add_flag(obj,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj,LV_OBJ_FLAG_HIDDEN);
}
static void disabled(lv_obj_t *obj,bool value)
{
    if(value)lv_obj_add_state(obj,LV_STATE_DISABLED);
    else lv_obj_remove_state(obj,LV_STATE_DISABLED);
}
static void action(lv_event_t *event)
{
    wifi_ui_action((enum wifi_action)(uintptr_t)lv_event_get_user_data(event),0);
}
static void select_network(lv_event_t *event)
{
    wifi_ui_action(WIFI_SELECT,(uintptr_t)lv_event_get_user_data(event));
}
static void mode_release(lv_event_t *event)
{
    (void)event;
    wifi_ui_action(lv_slider_get_value(slider) ? WIFI_AP : WIFI_STATION,0);
}
static void keyboard_event(lv_event_t *event)
{
    if(lv_event_get_code(event)==LV_EVENT_READY) {
        wifi_ui_join(lv_textarea_get_text(entry)); lv_textarea_set_text(entry,"");
    } else if(lv_event_get_code(event)==LV_EVENT_CANCEL) {
        lv_textarea_set_text(entry,"");wifi_ui_action(WIFI_CANCEL_PASSWORD,0);
    }
}
static lv_obj_t *label(lv_obj_t *parent,const char *text,int x,int y)
{
    lv_obj_t *obj=lv_label_create(parent);lv_label_set_text(obj,text);lv_obj_set_pos(obj,x,y);return obj;
}
static lv_obj_t *button(const char *text,int x,int y,int width,enum wifi_action value)
{
    lv_obj_t *obj=lv_button_create(screen);lv_obj_set_pos(obj,x,y);lv_obj_set_size(obj,width,36);
    lv_obj_t *caption=lv_label_create(obj);lv_label_set_text(caption,text);lv_obj_center(caption);
    lv_obj_add_event_cb(obj,action,LV_EVENT_CLICKED,(void *)(uintptr_t)value);return obj;
}
static void read_pointer(lv_indev_t *device,lv_indev_data_t *data)
{
    (void)device;*data=input;
}
int wifi_ui_lvgl_init(void *buffer1,void *buffer2,unsigned size,lv_display_flush_cb_t flush)
{
    lvgl_heap_init();lv_init();lv_tick_set_cb(k_uptime_get_32);
    display=lv_display_create(480,320);
    if(!display)return -ENOMEM;
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_theme(display,lv_theme_default_init(display,lv_color_hex(0x168bd9),
        lv_color_hex(0x07b6c8),true,&lv_font_montserrat_14));
    lv_display_set_buffers(display,buffer1,buffer2,size,LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    dashboard=lv_screen_active();screen=lv_obj_create(dashboard);lv_obj_set_style_bg_color(screen,lv_color_hex(0x080c10),0);
    lv_obj_set_style_pad_all(screen,0,0);
    lv_obj_set_style_text_color(screen,lv_color_hex(0xeeeeee),0);
    lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    label(screen,"WI-FI",214,14);label(screen,"< SWIPE >",12,14);
    label(screen,"Station",80,48);label(screen,"AP",360,48);
    slider=lv_slider_create(screen);lv_obj_set_pos(slider,90,77);lv_obj_set_size(slider,300,18);
    lv_slider_set_range(slider,0,1);
    lv_obj_add_event_cb(slider,mode_release,LV_EVENT_RELEASED,NULL);
    net_list=lv_list_create(screen);lv_obj_set_pos(net_list,10,106);lv_obj_set_size(net_list,460,152);
    ap_label=label(screen,"",12,110);lv_obj_set_width(ap_label,456);
    lv_obj_set_style_text_font(ap_label,&network_font,0);
    refresh=button("Refresh",12,266,110,WIFI_SCAN);
    password_button=button("Password",132,266,110,WIFI_PASSWORD);
    status_label=label(screen,"",12,305);lv_obj_set_width(status_label,456);
    ssid_label=label(screen,"",12,107);lv_obj_set_width(ssid_label,456);
    lv_obj_set_style_text_font(ssid_label,&network_font,0);
    entry=lv_textarea_create(screen);lv_obj_set_pos(entry,12,128);lv_obj_set_size(entry,456,36);
    lv_textarea_set_one_line(entry,true);lv_textarea_set_max_length(entry,64);
    lv_textarea_set_accepted_chars(entry,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~");
    lv_textarea_set_password_mode(entry,true);lv_textarea_set_password_show_time(entry,0);
    keyboard=lv_keyboard_create(screen);lv_obj_set_pos(keyboard,0,170);lv_obj_set_size(keyboard,480,150);
    lv_keyboard_set_textarea(keyboard,entry);
    lv_obj_add_event_cb(keyboard,keyboard_event,LV_EVENT_READY,NULL);
    lv_obj_add_event_cb(keyboard,keyboard_event,LV_EVENT_CANCEL,NULL);
    pointer=lv_indev_create();if(!pointer)return -ENOMEM;
    lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(pointer,read_pointer);
    input.state=LV_INDEV_STATE_RELEASED;
    printf("\nLCD LVGL wifi ready; two shared 16-row DMA buffers\n");
    return 0;
}
lv_obj_t *wifi_ui_lvgl_card(void){return screen;}
void wifi_ui_lvgl_cancel_touch(void)
{
    if(!pointer)return;
    input.state=LV_INDEV_STATE_RELEASED;lv_indev_reset(pointer,NULL);
}
void wifi_ui_lvgl_touch(const struct volume_touch *touch)
{
    if(!pointer)return;
    if(touch->session!=input_session){input_session=touch->session;pointer_blocked=false;}
    if(touch->count>1 || touch->multitouch)pointer_blocked=true;
    input.state=LV_INDEV_STATE_RELEASED;
    if(touch->count==1 && !pointer_blocked) {
        for(unsigned i=0;i<VOLUME_TOUCH_SLOTS;i++)if(touch->contacts[i].down) {
            input.point.x=touch->contacts[i].x;input.point.y=touch->contacts[i].y;
            input.state=LV_INDEV_STATE_PRESSED;break;
        }
    }
    /* Preserve down/up samples even if a complete tap arrives in one loop. */
    lv_indev_read(pointer);
    if(!touch->count)pointer_blocked=false;
}
void wifi_ui_lvgl_tick(bool changed)
{
    if(!display)return;
    struct wifi_view view=wifi_ui_view();
    if(!view.active) {
        if(shown){lv_textarea_set_text(entry,"");input.state=LV_INDEV_STATE_RELEASED;lv_indev_reset(pointer,NULL);}
        shown=false;editing=false;return;
    }
    if(changed || !shown) {
        hidden(net_list,view.ap || view.keyboard);hidden(ap_label,!view.ap || view.keyboard);
        hidden(refresh,view.ap || view.keyboard);hidden(password_button,view.ap || view.keyboard);
        hidden(slider,view.keyboard);hidden(ssid_label,!view.keyboard);
        hidden(entry,!view.keyboard);hidden(keyboard,!view.keyboard);hidden(status_label,view.keyboard);
        disabled(slider,view.busy);disabled(net_list,view.busy);disabled(refresh,view.busy);
        disabled(password_button,view.busy);disabled(entry,view.busy);disabled(keyboard,view.busy);
        lv_slider_set_value(slider,view.ap,LV_ANIM_OFF);
        lv_label_set_text(status_label,view.message);
        if(view.keyboard) {
            lv_label_set_text(ssid_label,view.selected<view.count ? view.nets[view.selected].ssid : "");
            if(!editing)lv_textarea_set_text(entry,"");
        } else {
            if(editing)lv_textarea_set_text(entry,"");
            if(view.ap)lv_label_set_text_fmt(ap_label,"SSID: %s\n\nKEY: %s\n\nIP: %s",view.ap_ssid,view.ap_password,view.ap_address);
            else {
                lv_obj_clean(net_list);
                for(unsigned i=0;i<view.count;i++) {
                    char caption[100];snprintf(caption,sizeof(caption),"%s  %u%% %s",view.nets[i].ssid,view.nets[i].signal,view.nets[i].saved ? "Saved" : view.nets[i].security ? "Locked" : "Open");
                    lv_obj_t *obj=lv_list_add_button(net_list,NULL,caption);
                    lv_obj_set_style_text_font(obj,&network_font,0);
                    lv_obj_set_height(obj,36);
                    lv_obj_add_event_cb(obj,select_network,LV_EVENT_CLICKED,(void *)(uintptr_t)i);
                }
            }
        }
        editing=view.keyboard;
        shown=true;
    }
    lv_timer_handler();
}
