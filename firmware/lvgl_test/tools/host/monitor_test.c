#include "lvgl.h"
#include "pc_monitor.h"
#include "eez_ui_glue.h"
#include "power.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static lv_obj_t *face;
lv_obj_t *face_lvgl_page(void) { return face; }
static unsigned char panel_brightness;
void st7802_write_cmd(uint8_t cmd,const uint8_t *p,uint32_t n)
{ if(cmd==0x51 && n) panel_brightness=*p; }
static uint16_t pixels[480*480],buf[480*64];
static int pressed,x,y;
static int flush_calls;
static lv_area_t flush_union;
static void reset_flush(void)
{
    flush_calls=0;
    flush_union.x1=480;flush_union.y1=480;flush_union.x2=-1;flush_union.y2=-1;
}
static void mark_flush(const lv_area_t *a)
{
    flush_calls++;
    if(a->x1<flush_union.x1) flush_union.x1=a->x1;
    if(a->y1<flush_union.y1) flush_union.y1=a->y1;
    if(a->x2>flush_union.x2) flush_union.x2=a->x2;
    if(a->y2>flush_union.y2) flush_union.y2=a->y2;
}
static void read_pointer(lv_indev_t *i,lv_indev_data_t *d)
{ (void)i;d->point.x=x;d->point.y=y;d->state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED; }
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p)
{
    uint16_t *s=(uint16_t*)p;
    mark_flush(a);
    assert(a->x1>=0 && a->y1>=0 && a->x2<480 && a->y2<480);
    for(int yy=a->y1;yy<=a->y2;yy++) for(int xx=a->x1;xx<=a->x2;xx++) pixels[yy*480+xx]=*s++;
    lv_display_flush_ready(d);
}
static void step(int ms) { for(int i=0;i<ms;i+=5) { lv_tick_inc(5);lv_timer_handler(); } }
static void swipe(int from,int to)
{ x=from;y=240;pressed=1;step(40);x=to;step(40);pressed=0;step(150); }
static void save(const char *path)
{
    lv_refr_now(NULL);FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n480 480\n255\n");
    for(int i=0;i<480*480;i++) { uint16_t p=pixels[i];unsigned char rgb[]={((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};fwrite(rgb,1,3,f); }
    fclose(f);
}
static char reply[160];
static bool command(pm_link_t *s,char *line,uint32_t now,bool usb)
{
    char copy[128],*args[32];int n=0;
    assert(strlen(line)<sizeof(copy));strcpy(copy,line);
    for(char *p=strtok(copy," ");p && n<32;p=strtok(NULL," ")) args[n++]=p;
    return pm_receive(s,n,args,now,usb,reply);
}
static uint32_t crc32_ieee(const uint8_t *p,size_t n)
{
    uint32_t c=0xffffffffu;
    while(n--) { c^=*p++;for(int i=0;i<8;i++) c=(c&1u)?(c>>1)^0xedb88320u:(c>>1); }
    return c^0xffffffffu;
}
static uint32_t fnv1a32(const uint8_t *p,size_t n)
{
    uint32_t h=0x811c9dc5u;
    while(n--) { h^=*p++;h*=0x01000193u; }
    return h;
}
static uint16_t crc16_ccitt(const uint8_t *p,size_t n)
{
    uint16_t c=0xffffu;
    while(n--) { c^=(uint16_t)((uint16_t)*p++<<8);for(int i=0;i<8;i++) c=(c&0x8000u)?(uint16_t)((c<<1)^0x1021u):(uint16_t)(c<<1); }
    return c;
}
static void hex_of(char *out,const uint8_t *p,size_t n)
{
    static const char d[]="0123456789abcdef";
    for(size_t i=0;i<n;i++) { out[i*2]=d[p[i]>>4];out[i*2+1]=d[p[i]&15]; }
    out[n*2]=0;
}
static bool ink(int x1,int y1,int x2,int y2)
{
    for(int y=y1;y<=y2;y++) for(int x=x1;x<=x2;x++) if(pixels[y*480+x]) return true;
    return false;
}
static bool all_black(int x1,int y1,int x2,int y2)
{
    for(int y=y1;y<=y2;y++) for(int x=x1;x<=x2;x++) if(pixels[y*480+x]) return false;
    return true;
}
static void assert_glyphs(lv_obj_t *o)
{
    if(lv_obj_check_type(o,&lv_label_class)) {
        const char *t=lv_label_get_text(o);const lv_font_t *font=lv_obj_get_style_text_font(o,0);
        extern uint32_t (*lv_text_encoded_next)(const char*,uint32_t*);
        for(uint32_t i=0;t[i];) { uint32_t cp=lv_text_encoded_next(t,&i);lv_font_glyph_dsc_t d;assert(lv_font_get_glyph_dsc(font,&d,cp,0)); }
    }
    for(uint32_t i=0;i<lv_obj_get_child_count(o);i++) assert_glyphs(lv_obj_get_child(o,i));
}
static bool has_visible_label(lv_obj_t *o,const char *text)
{
    if(lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN)) return false;
    if(lv_obj_check_type(o,&lv_label_class) && !strcmp(lv_label_get_text(o),text)) return true;
    uint32_t n=lv_obj_get_child_count(o);
    for(uint32_t i=0;i<n;i++) if(has_visible_label(lv_obj_get_child(o,i),text)) return true;
    return false;
}
static int count_visible_images(lv_obj_t *o)
{
    if(lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN)) return 0;
    int n=lv_obj_check_type(o,&lv_image_class)?1:0;
    uint32_t c=lv_obj_get_child_count(o);
    for(uint32_t i=0;i<c;i++) n+=count_visible_images(lv_obj_get_child(o,i));
    return n;
}
static lv_obj_t *find_label(lv_obj_t *o,const char *text)
{
    if(lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN)) return NULL;
    if(lv_obj_check_type(o,&lv_label_class) && !strcmp(lv_label_get_text(o),text)) return o;
    for(uint32_t i=0;i<lv_obj_get_child_count(o);i++) {
        lv_obj_t *found=find_label(lv_obj_get_child(o,i),text);
        if(found) return found;
    }
    return NULL;
}
int main(void)
{
    lv_init();lv_display_t *d=lv_display_create(480,480);
    lv_display_set_buffers(d,buf,NULL,sizeof(buf),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);
    face=lv_obj_create(lv_screen_active());lv_obj_set_size(face,480,480);
    lv_indev_t *in=lv_indev_create();lv_indev_set_type(in,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(in,read_pointer);
    eez_ui_init();power_init();power_set_brightness(64);power_set_idle_ms(15000);
    pm_link_t s={0};
    step(200);assert(eez_ui_page()==0);
    swipe(170,300);assert(eez_ui_page()==4);pc_monitor_update(&s,true);step(100);
    assert_glyphs(eez_ui_page_obj(4));save("monitor-offline.ppm");
    swipe(310,180);assert(eez_ui_page()==0);
    swipe(310,180);assert(eez_ui_page()==1); /* Existing left-swipe page survives. */
    eez_ui_leave();
    assert(!command(&s,"pcmon hello 0123456789abcdef",100,false));
    assert(command(&s,"pcmon hello 0123456789abcdef",100,true));
    assert(strstr(reply,"PROTORIG_ES 1 480 480"));
    assert(command(&s,"pcmon host 0123456789abcdef 544553542d574f524b53544154494f4e",101,true));
    assert(!command(&s,"pcmon host 0123456789abcdef eda080",102,true));
    assert(!command(&s,"pcmon frame ffffffffffffffff 1 34 520 474 2458 12595 166 1 1",102,true));
    assert(command(&s,"pcmon frame 0123456789abcdef 1 34 520 474 2458 12595 166 1 1",103,true));
    pm_link_t good=s;
    assert(!command(&s,"pcmon frame 0123456789abcdef 2 1001 520 474 2458 12595 166 1 1",104,true));
    assert(!memcmp(&s,&good,sizeof(s)));
    assert(!command(&s,"pcmon frame 0123456789abcdef 1 50 50 50 0 0 166 1 1",105,true));
    pc_monitor_update(&s,false);assert(panel_brightness==64 && !power_monitor_active());
    swipe(170,300);assert(eez_ui_page()==4);pc_monitor_update(&s,true);step(100);
    assert(panel_brightness==166 && power_monitor_active());
    assert(power_get_brightness()==64 && power_get_idle_ms()==15000 && !power_get_always_on());
    assert_glyphs(eez_ui_page_obj(4));save("monitor-online.ppm");
    /* Bottom is truly black; no pagination overlay or plugin decoration. */
    for(int yy=250;yy<390;yy++) for(int xx=120;xx<360;xx++) assert(pixels[yy*480+xx]==0);
    for(int i=0;i<20;i++) { s.revision++;pc_monitor_update(&s,true);step(1000);power_tick();assert(power_is_on()); }
    eez_ui_leave();pc_monitor_update(&s,false);assert(panel_brightness==64 && !power_monitor_active());
    step(15010);power_tick();assert(!power_is_on());
    /* Turning monitor AOD off must allow idle even while frames keep arriving. */
    power_screen_on();s.always_on=0;s.revision++;eez_ui_monitor();pc_monitor_update(&s,true);
    for(int i=0;i<16;i++) { s.revision++;pc_monitor_update(&s,true);step(1000);power_tick(); }
    assert(!power_is_on());
    s.always_on=1;s.revision++;pc_monitor_update(&s,true);assert(power_is_on());
    assert(pm_poll(&s,5103,true));pc_monitor_update(&s,true);assert(!power_monitor_active() && panel_brightness==64);
    save("monitor-disconnected.ppm");
    assert(command(&s,"pcmon hello 1111111111111111",6000,true));
    assert(pm_poll(&s,6001,false));
    /* Millisecond counter wrap must retain the five-second lease. */
    assert(command(&s,"pcmon hello 1111111111111111",UINT32_MAX-100,true));
    assert(!pm_poll(&s,100,true));assert(pm_poll(&s,5000,true));

    /* ---- protocol v1.1: cap/bmpq/tick + single-focus 20px title bitmap ----- */
    char line[160],hex[2*PM_BMP_BLOCK_BYTES+1],expect[160];
    assert(command(&s,"pcmon hello 2222222222222222",10000,true));
    assert(command(&s,"pcmon cap 2222222222222222",10001,true));
    assert(!strcmp(reply,"@PM1 CAP 2222222222222222 31"));
    assert(!s.tick && !s.frame);
    assert(command(&s,"pcmon bmpq 2222222222222222 2",10002,true));
    assert(!strcmp(reply,"@PM1 BMPQ 2222222222222222 00000000 0 00000000 0"));
    assert(!command(&s,"pcmon bmpq ffffffffffffffff 3",10002,true));

    char tickline[]="pcmon tick 2222222222222222 20 34 12 47 2458 166 166 1 1 1 2 1 bash 3 2 read 7 62";
    assert(strlen(tickline)<126);
    assert(command(&s,tickline,10003,true));
    assert(s.tick && s.frame);
    assert(s.seq==20 && s.cpu==34 && s.gpu==12 && s.ram==47);
    assert(s.rx==2458 && s.tx==166 && s.brightness==166 && s.always_on==1);
    assert(s.need==1 && s.run==2 && s.ctx==62);
    assert(s.lane_state[0]==PM_STATE_RUNNING && !strcmp(s.lane_tool[0],"bash") && s.lane_step[0]==3);
    assert(s.lane_state[1]==PM_STATE_NEEDS && !strcmp(s.lane_tool[1],"read") && s.lane_step[1]==7);
    assert(s.bmp_valid==0 && s.bmp_w[0]==0 && s.bmp_hash[0]==0);
    pc_monitor_update(&s,true);step(100);
    assert(has_visible_label(eez_ui_page_obj(4),"RUNNING"));
    assert(has_visible_label(eez_ui_page_obj(4),"bash · 3 STEP"));
    assert(!has_visible_label(eez_ui_page_obj(4),"read"));      /* lane1 ignored */
    assert(!has_visible_label(eez_ui_page_obj(4),"S2"));
    assert(has_visible_label(eez_ui_page_obj(4),"S1"));         /* no bitmap yet */
    assert(has_visible_label(eez_ui_page_obj(4),"CTX 62%"));
    assert(count_visible_images(eez_ui_page_obj(4))==0);
    assert(ink(190,256,290,274));       /* state, ui20 baseline 272            */
    assert(ink(60,318,300,336));        /* tool/steps, ui16 baseline 334 x=90  */
    assert(ink(60,344,200,362));        /* CTX text, baseline 360 x=90         */
    assert(ink(150,352,390,358));       /* context bar x150..390 y352..357     */
    assert(pixels[355*480+250]==0x8DDF);/* #8FB8FF fill at ctx 62%             */
    assert_glyphs(eez_ui_page_obj(4));
    save("monitor-focus.ppm");

    /* five focus states are addressable through s0 = 0..4 */
    const int states[]={PM_STATE_IDLE,PM_STATE_RUNNING,PM_STATE_NEEDS,PM_STATE_DONE,PM_STATE_FAILED};
    const char *state_text[]={"IDLE","RUNNING","NEEDS YOU","DONE","FAILED"};
    for(int i=0;i<5;i++) {
        s.lane_state[0]=states[i];s.revision++;
        pc_monitor_update(&s,true);step(50);
        assert(has_visible_label(eez_ui_page_obj(4),state_text[i]));
    }
    s.lane_state[0]=PM_STATE_RUNNING;s.revision++;pc_monitor_update(&s,true);step(50);
    assert(!command(&s,"pcmon tick 2222222222222222 21 34 12 47 2458 166 166 1 1 1 2 5 bash 3 0 - 0 62",10004,true));

    /* empty tool uses the '-' placeholder, ctx -1 means unavailable (focus IDLE) */
    assert(command(&s,"pcmon tick 2222222222222222 22 34 12 47 2458 166 166 1 1 0 0 0 - 0 0 - 0 -1",10005,true));
    assert(s.lane_state[0]==PM_STATE_IDLE && !strcmp(s.lane_tool[0],"-") && s.ctx==-1);
    pc_monitor_update(&s,true);step(50);
    assert(has_visible_label(eez_ui_page_obj(4),"IDLE"));
    assert(has_visible_label(eez_ui_page_obj(4),"- · 0 STEP"));
    assert(has_visible_label(eez_ui_page_obj(4),"CTX --"));

    /* widest legal 20px title: 296 px (8-multiple) = 740 B = 25 x 30 B blocks */
    uint8_t bits[(296/8)*PM_BMP_H];
    for(int i=0;i<(int)sizeof(bits);i++) bits[i]=(uint8_t)(0xA5u^(unsigned)(i*7u));
    uint32_t hash=fnv1a32(bits,sizeof(bits)),crc=crc32_ieee(bits,sizeof(bits));
    uint16_t crc0=crc16_ccitt(bits,PM_BMP_BLOCK_BYTES);
    hex_of(hex,bits,PM_BMP_BLOCK_BYTES);

    snprintf(line,sizeof(line),"pcmon bmp 2222222222222222 30 0 304 0 26 %08lx %04x %s",
             (unsigned long)hash,(unsigned)crc0,hex);
    assert(!command(&s,line,10010,true));                       /* w > 300 */
    snprintf(line,sizeof(line),"pcmon bmp 2222222222222222 31 0 300 0 26 %08lx %04x %s",
             (unsigned long)hash,(unsigned)crc0,hex);
    assert(!command(&s,line,10010,true));                       /* w not 8-aligned */
    snprintf(line,sizeof(line),"pcmon bmp 2222222222222222 32 1 296 0 25 %08lx %04x %s",
             (unsigned long)hash,(unsigned)crc0,hex);
    assert(!command(&s,line,10010,true));                       /* slot1 data rejected */
    snprintf(line,sizeof(line),"pcmon bmp 2222222222222222 33 0 296 25 25 %08lx %04x %s",
             (unsigned long)hash,(unsigned)crc0,hex);
    assert(!command(&s,line,10010,true));                       /* i >= n */
    snprintf(line,sizeof(line),"pcmon bmp 2222222222222222 34 0 296 0 25 %08lx %04x %s",
             (unsigned long)hash,(unsigned)(crc0^1u),hex);
    uint32_t rev=s.revision;
    assert(!command(&s,line,10010,true));                       /* bad block CRC16 */
    assert(s.revision==rev && s.bmp_valid==0);
    snprintf(line,sizeof(line),"pcmon bmp 2222222222222222 35 0 296 0 24 %08lx %04x %s",
             (unsigned long)hash,(unsigned)crc0,hex);
    assert(!command(&s,line,10011,true));                       /* n != ceil(740/30) */

    /* 25-block transfer, intentionally starting with the last block */
    int last_len=(int)sizeof(bits)-24*PM_BMP_BLOCK_BYTES;
    hex_of(hex,bits+24*PM_BMP_BLOCK_BYTES,(size_t)last_len);
    snprintf(line,sizeof(line),"pcmon bmp 2222222222222222 100 0 296 24 25 %08lx %04x %s",
             (unsigned long)hash,
             (unsigned)crc16_ccitt(bits+24*PM_BMP_BLOCK_BYTES,(size_t)last_len),hex);
    assert(strlen(line)<126);
    assert(command(&s,line,10012,true));
    for(int i=0;i<24;i++) {
        hex_of(hex,bits+i*PM_BMP_BLOCK_BYTES,PM_BMP_BLOCK_BYTES);
        snprintf(line,sizeof(line),"pcmon bmp 2222222222222222 %d 0 296 %d 25 %08lx %04x %s",
                 101+i,i,(unsigned long)hash,
                 (unsigned)crc16_ccitt(bits+i*PM_BMP_BLOCK_BYTES,PM_BMP_BLOCK_BYTES),hex);
        assert(strlen(line)<126);
        assert(command(&s,line,10013,true));
    }
    hex_of(hex,bits,PM_BMP_BLOCK_BYTES);
    snprintf(line,sizeof(line),"pcmon bmp 2222222222222222 200 0 296 0 25 %08lx %04x %s",
             (unsigned long)hash,(unsigned)crc0,hex);
    assert(command(&s,line,10014,true));                        /* duplicate block */

    snprintf(line,sizeof(line),"pcmon bmpend 2222222222222222 200 0 296 12 %08lx %08lx",
             (unsigned long)hash,(unsigned long)crc);
    assert(!command(&s,line,10015,true));                       /* h must be 20 */
    snprintf(line,sizeof(line),"pcmon bmpend 2222222222222222 201 0 296 20 %08lx %08lx",
             (unsigned long)(hash^1u),(unsigned long)crc);
    assert(!command(&s,line,10020,true));                       /* hash mismatch */
    snprintf(line,sizeof(line),"pcmon bmpend 2222222222222222 202 0 296 20 %08lx %08lx",
             (unsigned long)hash,(unsigned long)(crc^1u));
    assert(!command(&s,line,10021,true));                       /* crc32 mismatch */
    assert(!(s.bmp_valid&1));
    snprintf(line,sizeof(line),"pcmon bmpend 2222222222222222 203 0 296 20 %08lx %08lx",
             (unsigned long)hash,(unsigned long)crc);
    assert(command(&s,line,10022,true));
    assert((s.bmp_valid&1) && s.bmp_w[0]==296 && s.bmp_h[0]==20 && s.bmp_hash[0]==hash);

    uint16_t gw=0,gh=0;
    const uint8_t *a8=pm_bitmap_get(0,&gw,&gh);
    assert(a8 && gw==296 && gh==20);
    for(int y=0;y<20;y++) for(int x=0;x<296;x++) {
        int bit=(bits[y*37+(x>>3)]>>(7-(x&7)))&1;
        assert(a8[y*296+x]==(bit?255:0));
    }
    assert(pm_bitmap_get(1,&gw,&gh)==NULL);
    assert(command(&s,"pcmon bmpq 2222222222222222 204",10023,true));
    snprintf(expect,sizeof(expect),"@PM1 BMPQ 2222222222222222 %08lx 296 00000000 0",(unsigned long)hash);
    assert(!strcmp(reply,expect));

    pc_monitor_update(&s,true);step(100);
    assert(!has_visible_label(eez_ui_page_obj(4),"S1"));
    assert(count_visible_images(eez_ui_page_obj(4))==1);
    assert(pixels[286*480+91]==0);       /* centred: 296px starts at x=92      */
    assert(pixels[286*480+92]==0xF79E);  /* first title pixel: byte0 bit7=1    */
    assert(pixels[286*480+93]==0);       /* byte0 bit6=0                       */
    assert(pixels[286*480+94]==0xF79E);  /* byte0 bit5=1                       */
    assert(all_black(92,306,387,306));   /* below the 20px title row           */
    assert_glyphs(eez_ui_page_obj(4));
    save("monitor-focus-bitmap.ppm");

    assert(s.vram==-1);
    assert(!command(&s,"pcmon vram 2222222222222222 205 1001 1",10024,true));
    assert(!command(&s,"pcmon vram 2222222222222222 205 225 4",10024,true));
    assert(s.vram==-1 && s.seq==204);
    assert(command(&s,"pcmon vram 2222222222222222 205 225 1",10024,true));
    assert(command(&s,"pcmon vram 2222222222222222 205 225 1",10025,true)); /* lost ACK */
    assert(!command(&s,"pcmon vram 2222222222222222 205 226 1",10025,true));
    assert(!command(&s,"pcmon vram 2222222222222222 204 225 1",10025,true));
    pc_monitor_update(&s,true);step(100);
    assert(has_visible_label(eez_ui_page_obj(4),"VRAM 22.5%"));
    assert(lv_color_eq(lv_obj_get_style_text_color(find_label(eez_ui_page_obj(4),"GPU"),0),lv_color_hex(0x76B900)));
    s.ctx=22;pc_monitor_update(&s,true);step(100);save("monitor-vram-nvidia.ppm");
    s.ctx=100;pc_monitor_update(&s,true);step(100);
    lv_obj_t *ctx=find_label(eez_ui_page_obj(4),"CTX 100%");assert(ctx);
    assert(180-lv_obj_get_x(ctx)-lv_obj_get_width(ctx)>=12);
    assert(command(&s,"pcmon vram 2222222222222222 206 1000 2",10026,true));
    pc_monitor_update(&s,true);step(100);
    assert(has_visible_label(eez_ui_page_obj(4),"VRAM 100.0%"));
    assert(lv_color_eq(lv_obj_get_style_text_color(find_label(eez_ui_page_obj(4),"GPU"),0),lv_color_hex(0xED1C24)));
    assert(command(&s,"pcmon vram 2222222222222222 207 0 3",10027,true));
    pc_monitor_update(&s,true);step(100);
    assert(has_visible_label(eez_ui_page_obj(4),"VRAM 0.0%"));
    assert(lv_color_eq(lv_obj_get_style_text_color(find_label(eez_ui_page_obj(4),"GPU"),0),lv_color_hex(0x0091FF)));
    assert(command(&s,"pcmon vram 2222222222222222 208 -1 0",10028,true));
    pc_monitor_update(&s,true);step(100);
    assert(has_visible_label(eez_ui_page_obj(4),"VRAM --"));
    assert(lv_color_eq(lv_obj_get_style_text_color(find_label(eez_ui_page_obj(4),"GPU"),0),lv_color_hex(0x8FB8FF)));

    /* Only changed regions are invalidated: identical state -> no flush at all,
     * and a ctx-only change touches just the ctx label + bar. */
    reset_flush();
    pc_monitor_update(&s,true);step(100);
    assert(flush_calls==0);
    s.ctx=90;s.revision++;
    pc_monitor_update(&s,true);step(100);
    assert(flush_calls>0);
    assert(flush_union.x1>=80 && flush_union.x2<=400);
    assert(flush_union.y1>=340 && flush_union.y2<=370);
    assert(pixels[355*480+300]==0xFFC0); /* >=80% fill turns yellow            */
    s.ctx=62;s.revision++;pc_monitor_update(&s,true);step(100);
    assert(pixels[355*480+250]==0x8DDF); /* back to #8FB8FF                    */

    /* unused slot1: all-zero bmpend is a no-op ACK; non-zero forms are rejected */
    rev=s.revision;
    assert(command(&s,"pcmon bmpend 2222222222222222 210 1 0 0 00000000 00000000",10030,true));
    assert(s.revision==rev+1 && s.seq==210 && (s.bmp_valid&1) && s.bmp_w[1]==0 && s.bmp_hash[1]==0);
    assert(!command(&s,"pcmon bmpend 2222222222222222 211 1 8 20 00000000 00000000",10031,true));
    assert(!command(&s,"pcmon bmpend 2222222222222222 212 1 8 20 11111111 22222222",10032,true));
    assert((s.bmp_valid&1) && s.bmp_w[0]==296);
    assert(command(&s,"pcmon bmpq 2222222222222222 213",10033,true));
    snprintf(expect,sizeof(expect),"@PM1 BMPQ 2222222222222222 %08lx 296 00000000 0",(unsigned long)hash);
    assert(!strcmp(reply,expect));

    /* clear slot0 -> placeholder comes back */
    assert(command(&s,"pcmon bmpend 2222222222222222 214 0 0 0 00000000 00000000",10034,true));
    assert(!(s.bmp_valid&1) && s.bmp_w[0]==0 && s.bmp_hash[0]==0);
    assert(pm_bitmap_get(0,NULL,NULL)==NULL);
    pc_monitor_update(&s,true);step(100);
    assert(has_visible_label(eez_ui_page_obj(4),"S1"));
    assert(count_visible_images(eez_ui_page_obj(4))==0);
    assert(command(&s,"pcmon bmpend 2222222222222222 215 0 0 0 00000000 00000000",10035,true));
    assert(!(s.bmp_valid&1));                                 /* idempotent */

    /* staging timeout (3 s) on a small 8x20 bitmap: 20 B = 1 block */
    uint8_t small[20];
    for(int i=0;i<20;i++) small[i]=0xFF;
    uint32_t h2=fnv1a32(small,20),c2=crc32_ieee(small,20);
    hex_of(hex,small,20);
    snprintf(line,sizeof(line),"pcmon bmp 2222222222222222 220 0 8 0 1 %08lx %04x %s",
             (unsigned long)h2,(unsigned)crc16_ccitt(small,20),hex);
    assert(command(&s,line,12000,true));
    snprintf(line,sizeof(line),"pcmon bmpend 2222222222222222 221 0 8 20 %08lx %08lx",
             (unsigned long)h2,(unsigned long)c2);
    assert(!command(&s,line,15001,true));                     /* staging expired */
    assert(!(s.bmp_valid&1));

    hex_of(hex,small,20);
    snprintf(line,sizeof(line),"pcmon bmp 2222222222222222 222 0 8 0 1 %08lx %04x %s",
             (unsigned long)h2,(unsigned)crc16_ccitt(small,20),hex);
    assert(command(&s,line,15010,true));
    snprintf(line,sizeof(line),"pcmon bmpend 2222222222222222 223 0 8 20 %08lx %08lx",
             (unsigned long)h2,(unsigned long)c2);
    assert(command(&s,line,15011,true));
    assert((s.bmp_valid&1) && pm_bitmap_get(0,NULL,NULL)!=NULL);
    assert(pm_poll(&s,20011,true));                           /* 5 s lease expired */
    assert(!s.session && !s.tick && s.bmp_valid==0);
    assert(pm_bitmap_get(0,NULL,NULL)==NULL);                 /* session reset drops pixels */
    pc_monitor_update(&s,true);step(100);
    assert(all_black(120,270,360,388));                       /* lower half cleared */

    /* Old desktop fallback: a v1 frame without tick keeps the lower half black. */
    assert(command(&s,"pcmon hello 3333333333333333",30000,true));
    assert(command(&s,"pcmon frame 3333333333333333 1 34 520 474 2458 12595 166 1 1",30001,true));
    pc_monitor_update(&s,true);step(100);
    assert(all_black(120,270,360,388));

    printf("pm_link_t = %u bytes\n",(unsigned)sizeof(pm_link_t));
    assert(sizeof(pm_link_t)<=192);
    puts("MONITOR HOST PASS: right/left navigation, fonts, bounded protocol, black reserve, isolated power and disconnect lease, v1.1 single-focus 20px title (5 states, 25x30B blocks, slot1 no-op, timeout/clear/hash), old-frame fallback");
    return 0;
}
