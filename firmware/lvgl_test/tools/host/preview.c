/* Runs the actual firmware screens, gesture layer, input adapter and power code. */
#include "lvgl.h"
#include "face_lvgl.h"
#include "eez_ui_glue.h"
#include "screens.h"
#include "power.h"
#include "tp_lvgl.h"
#include "tp.h"
#include "bezel_clock.h"
#include "watch_photo.h"
#include "flush_policy.h"
#include "misc/lv_text_private.h"
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
static int flush_mode;
static int trace_minute_flush;
/* Match main.c: band capacity changes where partial refreshes split. */
static uint16_t pixels[480*480], drawbuf[480*64];
static int mock_down, mock_x, mock_y;
static uint8_t brightness;
static unsigned flushed_pixels;
static unsigned flushes;        /* refresh strips pushed since the last reset */
static void check_glyphs(lv_obj_t *o)
{
    if(lv_obj_check_type(o,&lv_label_class)) {
        const char *s=lv_label_get_text(o);
        const lv_font_t *font=lv_obj_get_style_text_font(o,0);
        for(uint32_t i=0;s[i];) {
            uint32_t cp=lv_text_encoded_next(s,&i);
            lv_font_glyph_dsc_t glyph;
            if(cp!='\n') assert(lv_font_get_glyph_dsc(font,&glyph,cp,0));
        }
    }
    for(unsigned i=0;i<lv_obj_get_child_count(o);i++) check_glyphs(lv_obj_get_child(o,i));
}
int tp_int_asserted(void) { return mock_down; }
int tp_cst9220_read_points(uint8_t a,uint16_t *x,uint16_t *y)
{ (void)a; *x=mock_x; *y=mock_y; return mock_down; }
void st7802_write_cmd(uint8_t cmd,const uint8_t *p,uint32_t n)
{ if(cmd==0x51 && n) brightness=*p; }
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p)
{
    if(trace_minute_flush) printf("[minute-window] (%d,%d)-(%d,%d)\n",
                                (int)a->x1,(int)a->y1,(int)a->x2,(int)a->y2);
    /*
     * 2026-10-16 晚：在整分那一帧，把每个窗口按**从 px_map 起始**的原始字节算一次
     * CRC32（和固件 flushdiag 的 s_push_crc 完全同族：反射多项式 0xEDB88320、
     * 初值/异或全 1），写进 minute_crc.txt。
     *
     * 为什么非做这个不可：`[minute] 0 px differ` 比的是"增量 vs 整屏重绘**两条路径
     * 的最终帧**"，而两条路径都走同一个 disp_flush —— 它**看不见**"这一次 flush
     * 取缓冲的起始偏移对不对"。面板又不能回读 GRAM，所以以前没有任何手段把
     * "真机实际送出去的字节"与"主机预测的字节"逐字节对上。
     *
     * 现在能对上了：真机 `minroll dump` 会打出那一帧每一块的窗口 + 发送侧 CRC。
     * 只要这里的 (70,91)-(409,180) / (70,181)-(409,264) 两条 CRC 与真机一致，
     * 就证明**送出去的字节完全一致**，问题就只剩下"面板怎么落这笔像素"；
     * 不一致，就直接指出是哪一块、从哪个字节开始错的。
     */
    if(trace_minute_flush) {
        static FILE *f;
        uint32_t crc=0xFFFFFFFFu;
        const uint8_t *bp=(const uint8_t*)p;
        uint32_t nbytes=(uint32_t)(a->x2-a->x1+1)*(uint32_t)(a->y2-a->y1+1)*2u;
        for(uint32_t i=0;i<nbytes;i++) {
            crc^=bp[i];
            for(int k=0;k<8;k++) crc=(crc>>1)^(0xEDB88320u&(uint32_t)(-(int32_t)(crc&1u)));
        }
        if(!f) f=fopen("minute_crc.txt","a");
        if(f) fprintf(f,"(%d,%d)-(%d,%d) px=%u crc=%08X\n",
                      (int)a->x1,(int)a->y1,(int)a->x2,(int)a->y2,
                      (unsigned)((a->x2-a->x1+1)*(a->y2-a->y1+1)),(unsigned)(~crc));
    }
    assert(a->x1 >= 0 && a->x2 < 480 && a->y1 >= 0 && a->y2 < 480);
    assert((a->x2-a->x1+1)*(a->y2-a->y1+1)*2 <= sizeof(drawbuf));
    if(flush_mode == 1) assert(!(a->x1 & 1) && (a->x2 & 1));
    flushes++;
    flushed_pixels+=(a->x2-a->x1+1)*(a->y2-a->y1+1);
    uint16_t *src=(uint16_t*)p;
    for(int y=a->y1;y<=a->y2;y++) for(int x=a->x1;x<=a->x2;x++) pixels[y*480+x]=*src++;
    lv_display_flush_ready(d);
}
static void step(unsigned ms)
{ for(unsigned n=0;n<ms;n+=5) { lv_tick_inc(5);lv_timer_handler(); } }
static void press(int x,int y) { mock_x=x;mock_y=y;mock_down=1;step(25); }
static void release(void) { mock_down=0;step(400); }   /* 400ms：够收尾动画（上限 190ms）跑完 */
static void tap(int x,int y) { press(x,y);release(); }
static void swipe(int x,int y,int xx,int yy)
{ press(x,y);mock_x=xx;mock_y=yy;step(25);release(); }
static void open_settings(void)
{ swipe(340,230,260,230);swipe(340,230,260,230);swipe(340,230,260,230); }
static void home(void) { swipe(240,180,245,270); }

/*
 * ── 翻页成本的度量 ────────────────────────────────────────────────────
 * flushed_pixels 是 LVGL 实际推给面板的像素数（= 传输字节数 / 2），
 * 与主机速度无关，可以直接换算成真机的送屏时间：
 *     QSPI 4 线 @ 40 MHz = 160 Mbit/s，RGB565 = 16 bit/px
 *     ms = pixels * 16 / 160e6 * 1000 = pixels / 10000
 * 一个 480x480 整帧 = 230,400 px ≈ 23.0 ms。
 *
 * 这条数字是"滑动到底贵在哪"的唯一硬指标：动画每帧重绘整屏 vs 只推新露出的
 * 竖条，差一个数量级。改完翻页实现后用它 A/B，别靠肉眼估。
 */
static void cost_probe(const char *tag)
{
    printf("[cost] %-30s strips=%3u pixels=%7u = %6.1f ms  (%5.1f%% of one full frame)\n",
           tag, flushes, flushed_pixels, flushed_pixels / 10000.0,
           100.0 * flushed_pixels / (480.0 * 480.0));
}
static void swipe_cost(const char *tag,int x,int y,int xx,int yy)
{
    flushed_pixels=0; flushes=0;
    swipe(x,y,xx,yy);
    cost_probe(tag);
}

static void save(const char *name)
{
    lv_refr_now(NULL);
    FILE *f=fopen(name,"wb");assert(f);
    fprintf(f,"P6\n480 480\n255\n");
    for(int y=0;y<480;y++) for(int x=0;x<480;x++) {
        uint16_t p=pixels[y*480+x];
        uint8_t rgb[]={((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};
        if((x-240)*(x-240)+(y-240)*(y-240)>240*240) rgb[0]=rgb[1]=rgb[2]=16;
        fwrite(rgb,1,3,f);
    }
    fclose(f);
}
static void check_hands(int hh,int mm,int ss)
{
    /*
     * 指针的载体已经改成 480x480 全屏对象（为了修"旧位置标脏被
     * lv_obj_area_is_visible() 丢弃导致指针刮花"），位置由**角度**算出，
     * 所以这里查角度，不能再靠对象坐标推位置。
     */
    lv_obj_update_layout(face_lvgl_screen());
    for(int k=0;k<3;k++) {   /* 时针 + 分针 + 秒针（秒针 2026-10-15 加回） */
        /* 指针不再有载体对象（改为在 chrome 回调里直接绘制），只校验角度 */
        int expect=bezel_angle(k,hh,mm,ss);
        int got=face_lvgl_hand_angle(k);
        printf("[hand] %d angle=%d expected=%d\n",k,got,expect);
        /* 按模 3600 比：秒针跨零点（59->00）时补间故意停在 3600 不回卷
         * （这样它会向前扫而不是倒着退一圈），画出来和 0 完全等价。 */
        assert(((got%3600)+3600)%3600==expect);
    }
}
static void clock_tests(void)
{
    assert(bezel_angle(0,12,0,0)==0 && bezel_angle(0,3,0,0)==900);
    assert(bezel_angle(1,0,15,0)==900 && bezel_angle(2,0,0,30)==1800);
    unsigned total=0;
    for(int i=0;i<60;i++) {
        flushed_pixels=0;
        face_lvgl_set_time(2026,10,11,0,10,8,36+i/10);
        step(100);
        char path[40];snprintf(path,sizeof(path),"motion_%02d.ppm",i);save(path);
        total+=flushed_pixels;
        if(i%10==9) {
            /* 秒针有 180ms 补间（clock_update 里的 lv_anim），而上面只 step(100)，
             * 校验时动画还在半路 -> 角度是插值而非终值。先让它跑完再比。 */
            step(150);
            check_hands(10,8,36+i/10);
            if(flush_mode == 0) assert(total<480*480/3); /* 每秒多了秒针的"旧∪新" ~2K px（见 face_lvgl.c 的说明） */
            printf("[clock] second=%d transmitted_pixels=%u\n",36+i/10,total);total=0;
        }
    }
    face_lvgl_set_time(2026,10,11,0,23,59,59);step(300);check_hands(23,59,59);
    face_lvgl_set_time(2026,10,12,1,0,0,0);step(300);check_hands(0,0,0);save("midnight.ppm");
    face_lvgl_set_time(2026,10,12,1,12,0,0);step(300);check_hands(12,0,0);
    face_lvgl_set_time(2026,10,11,0,10,8,36);step(300);
}
/*
 * ── 拖影 A/B 测试 ─────────────────────────────────────────────────────
 * 板上现象：表圈那一圈被秒针"刮花"（几十个旧位置叠在一起）。
 *
 * 这个测试在 PC 上直接判定"旧位置到底有没有被擦掉"：
 *   A = 只把秒数从 36 推到 37（走增量/脏区路径），等补间跑完再刷新待处理脏区
 *   B = 同样一帧，但强制整屏重绘（标准答案）
 * A 和 B 的差异像素 = **没有被擦掉的旧手印**。
 *
 * 帧缓冲是持久的、flush 只写脏区，所以漏掉的标脏一定会留下差异。
 * 之前一直没做这个测试，白白烧了好几次板子。
 * ───────────────────────────────────────────────────────────────────── */
static void trail_test(void)
{
    static uint16_t inc[480*480];
    static uint16_t full[480*480];
    int n=0,x0=99999,y0=99999,x1=-1,y1=-1;

    home();
    /* 共同起点：36 秒 + 整屏重绘 */
    face_lvgl_set_time(2026,10,11,0,10,8,36);step(400);
    lv_obj_invalidate(lv_screen_active());lv_refr_now(NULL);

    /* A：增量路径 */
    face_lvgl_set_time(2026,10,11,0,10,8,37);step(400);
    lv_refr_now(NULL);
    memcpy(inc,pixels,sizeof(inc));

    /* B：同样一帧，但从干净状态强制整屏重绘 */
    face_lvgl_set_time(2026,10,11,0,10,8,36);step(400);
    lv_obj_invalidate(lv_screen_active());lv_refr_now(NULL);
    face_lvgl_set_time(2026,10,11,0,10,8,37);step(400);
    lv_obj_invalidate(lv_screen_active());lv_refr_now(NULL);
    memcpy(full,pixels,sizeof(full));

    for(int y=0;y<480;y++) for(int x=0;x<480;x++) {
        if(inc[y*480+x]!=full[y*480+x]) {
            n++;
            if(x<x0)x0=x; if(x>x1)x1=x; if(y<y0)y0=y; if(y>y1)y1=y;
        }
    }
    printf("[trail] incremental-vs-full: %d px differ",n);
    if(n) printf("  bbox=(%d,%d)-(%d,%d)",x0,y0,x1,y1);
    printf("\n");

    /* 存两张图，方便肉眼看差异在哪 */
    memcpy(pixels,inc,sizeof(inc));save("trail_inc.ppm");
    memcpy(pixels,full,sizeof(full));save("trail_full.ppm");
}
static void offset_probe(const char *tag)
{
    int x=0,y=0;
    face_lvgl_get_draw_offset(&x,&y);
    printf("[offset] %-26s s_draw=(%d,%d)\n",tag,x,y);
}
/*
 * ── 整分切换（秒针走过 12 点）时的像素错位 ────────────────────────────
 * 真机反馈：秒针过 12 点时，大时间的末位数字刷新会"有一点像素错位"。
 * 手法同 trail_test：先走增量路径（58->59->整分->01），再和"同一时刻
 * 强制整屏重绘"逐像素比。帧缓冲持久、flush 只写脏区，漏标脏就会留下差异。
 */
static void minute_roll_test(void)
{
    static uint16_t inc[480*480];
    int n=0,x0=99999,y0=99999,x1=-1,y1=-1;

    home();step(400);
    face_lvgl_set_time(2026,10,11,0,21,59,57);step(400);
    lv_obj_invalidate(lv_screen_active());step(300);      /* 干净整屏起点 */

    /* 增量：先普通走两秒，再跨整分（秒针此刻扫过 12 点） */
    face_lvgl_set_time(2026,10,11,0,21,59,58);step(300);
    face_lvgl_set_time(2026,10,11,0,21,59,59);step(300);
    face_lvgl_set_time(2026,10,11,0,22, 0, 0);step(300);
    face_lvgl_set_time(2026,10,11,0,22, 0, 1);step(300);
    lv_refr_now(NULL);
    memcpy(inc,pixels,sizeof(inc));

    /* 标准答案：同一时刻，从干净状态整屏重绘 */
    lv_obj_invalidate(lv_screen_active());lv_refr_now(NULL);

    for(int y=0;y<480;y++) for(int x=0;x<480;x++) {
        if(inc[y*480+x]!=pixels[y*480+x]) {
            n++;
            if(x<x0)x0=x; if(x>x1)x1=x; if(y<y0)y0=y; if(y>y1)y1=y;
        }
    }
    printf("[minute] incremental-vs-full at roll: %d px differ",n);
    if(n) printf("  bbox=(%d,%d)-(%d,%d)",x0,y0,x1,y1);
    printf("\n");
    {   /* 两张图都留下，肉眼看差异 */
        static uint16_t full[480*480];
        memcpy(full,pixels,sizeof(full));
        memcpy(pixels,inc,sizeof(inc));save("minute_inc.ppm");
        memcpy(pixels,full,sizeof(full));save("minute_full.ppm");
    }
    assert(n == 0);

    /* Check the first refreshed frame, not just the frame one second later.
     * Include the photographed 20->21 transition and every decimal carry. */
    for(int minute=0;minute<60;minute++) {
        face_lvgl_set_time(2026,10,11,0,21,minute,59);step(400);
        face_lvgl_set_time(2026,10,11,0,minute==59?22:21,(minute+1)%60,0);
        trace_minute_flush = minute == 20;
        lv_refr_now(NULL);
        trace_minute_flush = 0;
        memcpy(inc,pixels,sizeof(inc));
        lv_obj_invalidate(lv_screen_active());lv_refr_now(NULL);
        n=0;
        for(unsigned i=0;i<480*480;i++) if(inc[i]!=pixels[i]) n++;
        printf("[minute-first] %02d->%02d: %d px differ\n",minute,(minute+1)%60,n);
        assert(n == 0);
        step(300);
        memcpy(inc,pixels,sizeof(inc));
        lv_obj_invalidate(lv_screen_active());lv_refr_now(NULL);
        assert(!memcmp(inc,pixels,sizeof(inc)));
    }
}
/* 一次翻页（横向滑动 / 竖向回表盘）的送屏成本，见 cost_probe() 的说明 */
static void transition_cost_test(void)
{
    home();step(500);
    printf("[cost] --- one page change ---\n");
    swipe_cost("face -> network",340,230,260,230);
    swipe_cost("network -> meets",340,230,265,238);
    swipe_cost("meets -> settings",340,230,265,230);
    swipe_cost("settings -> meets",160,200,240,201);
    swipe_cost("meets -> network",160,200,240,201);
    swipe_cost("network -> face (down)",240,180,245,270);
    home();step(500);
}
/*
 * ── 即时翻页自检 ─────────────────────────────────────────────────────
 * 定版是"不做滑动动画"：手指划过门槛的那一帧就换页（不用等松手），
 * 一次换页的代价就是一个整帧。这里判两件事：
 *   · 门槛之下的慢拖**不**翻页（别把轻点/抖动当滑动）
 *   · 过门槛（拖够 12% 或轻甩）→ 手指还没抬页就变了（即时反馈）
 */
static void drag_tests(void)
{
    home();step(400);assert(eez_ui_page()==0);

    /* 慢拖 30px（6%）：不够门槛，不翻页 */
    press(300,230);
    for(int i=1;i<=6;i++){mock_x=300-i*5;step(30);}
    release();step(200);
    assert(eez_ui_page()==0);

    /* 拖 170px（35%）：越过门槛的当下就翻页，不等松手 */
    press(300,230);mock_x=130;step(60);
    assert(eez_ui_page()==1);
    release();step(200);
    assert(eez_ui_page()==1);

    /* 反方向轻甩 60px：也立刻翻回去，且旧页要停到屏幕外 */
    press(200,230);mock_x=260;step(30);
    assert(eez_ui_page()==0);
    release();step(200);
    assert((int)lv_obj_get_x(eez_ui_page_obj(1))==480);
    assert((int)lv_obj_get_x(eez_ui_page_obj(0))==0);

    /* 竖向：功能页下拉 120px -> 立刻回表盘 */
    swipe(340,230,260,230);step(300);assert(eez_ui_page()==1);
    press(240,150);mock_y=270;step(60);
    assert(eez_ui_page()==0);
    release();step(200);
    printf("[nav] instant switching OK\n");
    home();step(300);
}

/*
 * ── 翻页后像素是否正确（不许为了便宜而漏掉带）────────────────────────
 * 帧缓冲是持久的、flush 只写脏区：跟手滑动结束后，把当前页强制整屏重绘，
 * 再和"一路拖过来"的结果逐像素比。有任何差异 = 拖动过程漏画了区域。
 */
static void slide_pixel_test(void)
{
    static uint16_t incremental[480*480];
    int i,n=0,x0=99999,y0=99999,x1=-1,y1=-1;

    home();step(500);
    swipe(340,230,260,230);step(400);          /* 表盘 -> 协议网络 */
    swipe(300,240,140,244);step(500);          /* 再翻一页，用更长的拖 */
    lv_refr_now(NULL);
    memcpy(incremental,pixels,sizeof(incremental));

    /* 标准答案：同一页，强制整屏重绘 */
    lv_obj_invalidate(eez_ui_page_obj(eez_ui_page()));
    lv_refr_now(NULL);

    for(int y=0;y<480;y++) for(int x=0;x<480;x++) {
        if(incremental[y*480+x]!=pixels[y*480+x]) {
            n++;
            if(x<x0)x0=x; if(x>x1)x1=x; if(y<y0)y0=y; if(y>y1)y1=y;
        }
    }
    printf("[slide] dragged-vs-full-redraw: %d px differ",n);
    if(n) printf("  bbox=(%d,%d)-(%d,%d)",x0,y0,x1,y1);
    printf("\n");
    if(n) {
        static uint16_t keep[480*480];
        memcpy(keep,pixels,sizeof(keep));
        memcpy(pixels,incremental,sizeof(incremental));save("slide_inc.ppm");
        memcpy(pixels,keep,sizeof(keep));save("slide_full.ppm");
    }
    assert(n==0);
    (void)i;
    home();step(500);
}
/*
 * 一次换页的送屏预算 —— 定版"即时翻页"的核心数字。
 * 一个整帧 = 230,400 px = 23.0 ms（40 MHz QSPI）；只要换页走这条路，
 * 一次翻页就应该正好是这个量级。多了就说明有重复绘制。
 */
static void page_cost_probe(void)
{
    home();step(400);
    swipe_cost("face -> network (instant)",340,230,260,230);
    swipe_cost("network -> meets (instant)",340,230,265,238);
    swipe_cost("meets -> network (instant)",160,200,240,201);
    swipe_cost("network -> face (down)",240,180,245,270);
    home();step(400);
}
static void photo_idle_test(void)
{
    home(); step(700); lv_refr_now(NULL);
    flushed_pixels=0;step(12000);
    assert(flushed_pixels==0); /* static photo must not drive idle redraws */
    static uint16_t saved[480*480];memcpy(saved,pixels,sizeof(pixels));
    lv_obj_invalidate(lv_screen_active());lv_refr_now(NULL);
    assert(!memcmp(saved,pixels,sizeof(pixels)));
    puts("[photo] 12 seconds idle: zero refresh; full redraw identical");
}

int main(int argc, char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);   /* 断言 abort 时不要丢掉最后几行日志 */
    flush_mode = argc > 1 ? atoi(argv[1]) : 0;
    assert(flush_mode >= 0 && flush_mode <= 2);
    lv_init();lv_display_t *d=lv_display_create(480,480);
    lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d,drawbuf,NULL,sizeof(drawbuf),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);
    lv_display_add_event_cb(d,flush_policy_event,LV_EVENT_INVALIDATE_AREA,&flush_mode);
    tp_lvgl_init();face_lvgl_create();eez_ui_init();power_init();
    check_glyphs(face_lvgl_screen());check_glyphs(eez_ui_page_obj(1));
    check_glyphs(eez_ui_page_obj(2));check_glyphs(eez_ui_page_obj(3));
    trail_test();
    minute_roll_test();

    /* 偏移探针：看 s_draw 在各种状态下是不是 (0,0) */
    offset_probe("boot");
    open_settings();          offset_probe("after open_settings");
    home();                   offset_probe("after home");
    swipe(340,230,260,230);   offset_probe("mid swipe");
    home();                   offset_probe("after home again");
    step(400);                offset_probe("after settle");
    face_lvgl_set_time(2026,10,11,0,10,8,36);step(300);save("face.ppm");
    clock_tests();
    tap(240,180);assert(!eez_ui_is_open()); /* empty area never navigates */
    swipe(340,230,260,233);assert(eez_ui_page()==1);save("network.ppm");
    swipe(340,230,265,238);assert(eez_ui_page()==2);save("meets.ppm");
    swipe(340,230,265,230);assert(eez_ui_page()==3);save("settings.ppm");
    tap(122,200);assert(power_get_brightness()==64 && brightness==64);
    tap(240,302);assert(power_get_idle_ms()==30000);
    press(240,205);mock_x=250;mock_y=211;step(20);release();
    assert(power_get_brightness()==153); /* ordinary finger jitter remains a tap */
    press(122,200);mock_x=151;step(20);mock_x=122;step(20);release();
    assert(power_get_brightness()==153); /* excursion then return must not click */
    tap(122,200);assert(power_get_brightness()==64);
    press(240,240);mock_down=0;lv_tick_inc(130);lv_timer_handler();
    int down=1;tp_lvgl_get_state(NULL,NULL,&down,NULL);assert(!down);
    release(); /* release duration is wall time even after a blocked render */
    power_screen_off();mock_x=120;mock_y=403;mock_down=1;
    assert(tp_lvgl_poll_raw());power_screen_on();assert(brightness==64);
    step(40);release();assert(eez_ui_is_open()); /* complete wake contact swallowed */
    home();assert(!eez_ui_is_open()); /* next stroke responds normally */
    swipe(140,220,230,220);assert(!eez_ui_is_open()); /* bounded first page */
    open_settings();assert(eez_ui_page()==3);
    swipe(160,200,240,201);assert(eez_ui_page()==2); /* swipe over setting != setting tap */
    swipe(240,180,245,270);assert(!eez_ui_is_open());
    for(int i=0;i<30;i++){open_settings();home();}
    assert(!eez_ui_is_open());
    drag_tests();
    slide_pixel_test();
    page_cost_probe();
    transition_cost_test();
    /* Shell rebuild must not delete an EEZ screen or leak duplicate screens. */
    open_settings();eez_ui_leave();face_lvgl_create();eez_ui_init();step(200);
    open_settings();assert(eez_ui_page()==3);
    home();assert(!eez_ui_is_open());
    photo_idle_test();
    puts("PASS: pages, target taps, settings, wake suppression, direction, repeated navigation");
    return 0;
}
