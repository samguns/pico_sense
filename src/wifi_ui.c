/* SPDX-License-Identifier: Apache-2.0 */
#include "wifi_ui.h"
#include "display_font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#define MAX_NETS 32
static struct wifi_network nets[MAX_NETS];
static unsigned count, page, selected, layout, password_len;
static bool active, dirty, busy, ap, keyboard, upper, tracking;
static unsigned touch_session;
static int slider_start = -1, slider_end;
static char password[65], message[100], ap_ssid[65], ap_password[65], ap_address[48];
static const char *keys[][3] = {
    {"qwertyuiop", "asdfghjkl", "zxcvbnm"},
    {"1234567890", "!@#$%^&*()", "-_=+[]{}\\|"},
    {"~`'\";:,.?", "/<>" , ""}
};

static void erase_password(void) { memset(password, 0, sizeof(password)); password_len = 0; }
static void event(const char *action) { printf("\nEVENT wifi %s\n", action); }
static void connect_selected(void)
{
    char encoded[129];
    for (unsigned i = 0; i < password_len; i++) sprintf(encoded + 2*i, "%02x", (unsigned char)password[i]);
    if (!password_len) strcpy(encoded, "-");
    printf("\nEVENT wifi connect %u %s\n", selected, encoded);
    memset(encoded, 0, sizeof(encoded)); erase_password(); keyboard = false;
    busy = true; strcpy(message, "Connecting..."); dirty = true;
}

bool wifi_ui_active(void) { return active; }
bool wifi_ui_dirty(void) { bool result = dirty; dirty = false; return result; }

static bool decode(const char *hex, char *out, unsigned capacity)
{
    if (!strcmp(hex, "-")) { out[0] = 0; return true; }
    unsigned length = strlen(hex);
    if (length % 2 || length / 2 >= capacity) return false;
    for (unsigned i = 0; i < length; i += 2) {
        if (!isxdigit((unsigned char)hex[i]) || !isxdigit((unsigned char)hex[i+1])) return false;
        char byte[3] = {hex[i], hex[i+1], 0};
        unsigned value = strtoul(byte, NULL, 16);
        if (value < 32 || value == 127) return false;
        out[i/2] = value;
    }
    out[length/2] = 0; return true;
}

int wifi_ui_command(const char *command)
{
    char field[201], a[131], b[131], c[101], extra;
    unsigned index, saved, security, signal;
    if (sscanf(command, "mode %200s %c", field, &extra) == 1) {
        if (strcmp(field,"ap") && strcmp(field,"station")) return -EINVAL;
        ap = !strcmp(field,"ap");
    } else if (sscanf(command,"busy %u %c", &index, &extra) == 1 && index <= 1) busy = index;
    else if (!strcmp(command,"clear")) { count = 0; }
    else if (sscanf(command,"message %200s %c",field,&extra) == 1) {
        if (!decode(field,message,sizeof(message))) return -EINVAL;
    } else if (sscanf(command,"net %u %u %u %u %130s %c", &index,&saved,&security,&signal,a,&extra) == 5) {
        if (index >= MAX_NETS || saved > 1 || security > 2 || signal > 100 || !decode(a,nets[index].ssid,sizeof(nets[index].ssid))) return -EINVAL;
        nets[index].saved = saved; nets[index].security = security; nets[index].signal = signal;
        if (index >= count) count = index + 1;
    } else if (sscanf(command,"ap %130s %130s %100s %c",a,b,c,&extra) == 3) {
        if (!decode(a,ap_ssid,sizeof(ap_ssid)) || !decode(b,ap_password,sizeof(ap_password)) || !decode(c,ap_address,sizeof(ap_address))) return -EINVAL;
    } else return -EINVAL;
    if (!strncmp(command,"ap ",3) && page * 5 >= count) page = 0;
    dirty = active; return 0;
}

struct wifi_view wifi_ui_view(void)
{
    return (struct wifi_view){active,busy,ap,keyboard,count,selected,nets,
                             message,ap_ssid,ap_password,ap_address};
}
void wifi_ui_action(enum wifi_action action, unsigned index)
{
    if(action==WIFI_OPEN) {
        if(!active){active=true;dirty=true;busy=true;strcpy(message,"Loading...");event("open");}
        return;
    }
    if (action == WIFI_CLOSE) {
        active=false; keyboard=false; erase_password(); event("close"); dirty=true; return;
    }
    if (busy) return;
    switch (action) {
    case WIFI_SCAN: event("scan"); busy=true; strcpy(message,"Scanning..."); break;
    case WIFI_AP: case WIFI_STATION:
        if ((action==WIFI_AP)!=ap) {
            event(action==WIFI_AP ? "ap" : "station"); busy=true; strcpy(message,"Working...");
        }
        break;
    case WIFI_SELECT:
        if (index>=count) return;
        selected=index;
        if (nets[index].security==2 && !nets[index].saved) strcpy(message,"Enterprise/WEP needs host setup");
        else if (nets[index].saved || !nets[index].security) connect_selected();
        else {erase_password(); keyboard=true;}
        break;
    case WIFI_PASSWORD:
        if(selected<count && nets[selected].security==1) {erase_password(); keyboard=true;}
        break;
    case WIFI_CANCEL_PASSWORD: erase_password(); keyboard=false; break;
    default: return;
    }
    dirty=true;
}
void wifi_ui_join(const char *value)
{
    if (busy || !keyboard || !value || strlen(value)>64) return;
    password_len=strlen(value); memcpy(password,value,password_len+1); connect_selected();
}

static void tap(int x, int y)
{
    if (y < 40 && x >= 410) {
        active = false; keyboard = false; erase_password(); event("close"); dirty = true; return;
    }
    if (busy) return;
    if (keyboard) {
        if (y >= 170 && y < 266 && x >= 10 && x < 470) {
            unsigned row = (y-170)/32, col = (x-10)/46;
            if (col < strlen(keys[layout][row]) && password_len < 64) {
                char ch = keys[layout][row][col];
                password[password_len++] = upper && layout == 0 ? toupper((unsigned char)ch) : ch;
                password[password_len] = 0;
            }
        } else if (y >= 274) {
            if (x < 78) upper = !upper;
            else if (x < 158) layout = (layout+1)%3;
            else if (x < 238 && password_len < 64) { password[password_len++]=' '; password[password_len]=0; }
            else if (x < 318) { if(password_len) password[--password_len]=0; }
            else if (x < 398) { erase_password(); keyboard=false; }
            else connect_selected();
        }
    } else if (y >= 46 && y < 98 && x >= 50 && x < 430) {
        slider_start = x; slider_end = x;
    } else if (y >= 104 && y < 264 && !ap) {
        unsigned index = page*5 + (y-104)/32;
        if (index < count) {
            selected = index;
            if (nets[index].security == 2 && !nets[index].saved) strcpy(message,"Enterprise/WEP needs host setup");
            else if (nets[index].saved || !nets[index].security) connect_selected();
            else { erase_password(); keyboard=true; layout=0; upper=false; }
        }
    } else if (y >= 275 && !ap) {
        if (x < 120) { page = page ? page-1 : 0; }
        else if (x < 240) { if((page+1)*5 < count) page++; }
        else if (x < 360) { event("scan"); busy=true; }
        else if (selected < count && nets[selected].security == 1) { keyboard=true; erase_password(); }
    }
    dirty = true;
}

bool wifi_ui_touch(const struct volume_touch *touch)
{
#ifdef CONFIG_PICO_SENSE_LVGL_WIFI
    if (active) { wifi_ui_lvgl_touch(touch); return true; }
#endif
    if (touch->session != touch_session) { touch_session=touch->session; tracking=false; slider_start=-1; }
    if (touch->count && (touch->count > 1 || touch->multitouch)) { slider_start=-1; tracking=true; return active; }
    if (touch->count == 1) {
        int x=0,y=0;
        for(unsigned i=0;i<VOLUME_TOUCH_SLOTS;i++) if(touch->contacts[i].down) { x=touch->contacts[i].x; y=touch->contacts[i].y; break; }
        if (!tracking) {
            tracking=true;
            if (!active && (
#ifdef CONFIG_PICO_SENSE_LVGL_WIFI
                (y>=290) || (x>=380 && y<48)
#else
                y >= 40 && y < 66
#endif
                )) { active=true; dirty=true; busy=true; strcpy(message,"Loading..."); event("open"); }
            else if (active) tap(x,y);
        }
        if (slider_start >= 0) slider_end=x;
    } else if (tracking) {
        if (active && slider_start >= 0 && !busy) {
            bool next = slider_end >= 240;
            if(next != ap) { event(next ? "ap" : "station"); busy=true; strcpy(message,"Working..."); }
            slider_start=-1; dirty=true;
        }
        tracking=false;
    }
    return active || dirty;
}

static void label(const char *text, int x, int top, int y, uint16_t color, void (*pixel)(int,uint16_t))
{
    if(y < top || y >= top+16) return;
    const unsigned char *p=(const unsigned char *)text;
    while(*p && x < 468) {
        uint32_t code=*p++; unsigned n=0;
        if(code>=0xc2 && code<=0xdf) {code &=31;n=1;} else if(code>=0xe0 && code<=0xef){code &=15;n=2;} else if(code>=0xf0 && code<=0xf4){code &=7;n=3;}
        while(n-- && *p) code=(code<<6)|(*p++ & 63);
        unsigned width; const uint8_t *bits=display_font_glyph(code,&width);
        unsigned row=(bits[(y-top)*2]<<8)|bits[(y-top)*2+1];
        for(unsigned i=0;i<width && x+(int)i<468;i++) if(row & (0x8000U>>i)) pixel(x+i,color);
        x+=width;
    }
}

void wifi_ui_render(int y, void (*pixel)(int,uint16_t))
{
    if(!active) return;
    for(int x=0;x<480;x++) pixel(x,0x0841);
    label("WI-FI",12,14,y,0x07ff,pixel);label("CLOSE",418,14,y,0xef7d,pixel);
    if(y>=50 && y<90) for(int x=70;x<410;x++) pixel(x,((x>=240)==ap)?0x03ef:0x2124);
    label("STATION",125,62,y,0xef7d,pixel); label("AP",315,62,y,0xef7d,pixel);
    if(keyboard) {
        label(nets[selected].ssid,12,104,y,0x07ff,pixel);
        char hidden[66];memset(hidden,'*',password_len);hidden[password_len]=0;label(hidden,12,134,y,0xef7d,pixel);
        for(unsigned row=0;row<3;row++) for(unsigned col=0;col<strlen(keys[layout][row]);col++) {
            int x=10+46*col,top=170+32*row;
            if(y>=top && y<top+29) for(int dx=0;dx<43;dx++)pixel(x+dx,0x2124);
            char key[2]={keys[layout][row][col],0};if(upper&&layout==0)key[0]=toupper((unsigned char)key[0]);
            label(key,x+17,top+6,y,0xef7d,pixel);
        }
        const char *buttons[]={upper?"lower":"SHIFT","123/#","SPACE","DEL","BACK","JOIN"};
        for(unsigned i=0;i<6;i++)label(buttons[i],8+80*i,282,y,0x07ff,pixel);
    } else if(ap) {
        label("SSID",12,112,y,0x07ff,pixel);label(ap_ssid,80,112,y,0xef7d,pixel);
        label("KEY",12,146,y,0x07ff,pixel);label(ap_password,80,146,y,0xef7d,pixel);
        label("IP",12,180,y,0x07ff,pixel);label(ap_address,80,180,y,0xef7d,pixel);
        label(message,12,230,y,0xef7d,pixel);
    } else {
        for(unsigned i=page*5;i<count && i<(page+1)*5;i++) {
            int top=104+32*(i%5);label(nets[i].ssid,12,top,y,0xef7d,pixel);
            char info[32];snprintf(info,sizeof(info),"%u%% %s",nets[i].signal,nets[i].saved?"SAVED":nets[i].security?"LOCK":"OPEN");label(info,340,top,y,0x07ff,pixel);
        }
        label("PREV",12,279,y,0x07ff,pixel);label("NEXT",132,279,y,0x07ff,pixel);label("REFRESH",246,279,y,0x07ff,pixel);label("PASSWORD",365,279,y,0x07ff,pixel);
        label(message,12,301,y,busy?0xfd20:0xef7d,pixel);
    }
}
