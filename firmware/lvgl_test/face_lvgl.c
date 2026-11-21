/* ENDFIELD INDUSTRIES 默认表盘（face 0）。下半区 480x232 @ y=248 的像素来自
 * tools/make_watch_photo.py 由 assets/watch_photo/source.png 生成的 watch_photo.c
 * （常驻 flash；装了 facepanel 面板则改用面板图）。标题/时间/日期是 lv_label；
 * 表圈刻度、框标、指针、分隔线全部在 LV_EVENT_DRAW_MAIN（face_draw_cb）里自绘，
 * 没有 lv_arc，也不做地形重算/扫描定时器。
 */
#include "face_lvgl.h"

#include "lvgl.h"
#include "watch_photo.h"
#include "tp_lvgl.h"
#include "board_power.h"
#include "bezel_clock.h"
#include "eez_ui_glue.h"        /* eez_ui_page()：表盘是不是当前页 */

#include "svc_device_id.h"     /* F1：管理员编号（原 derive_admin_id 已搬入服务） */
#include "svc_face_panel.h"    /* 76：默认表盘下半区面板（media 分区 I8 图，可缺省） */

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 字库（tools/gen_lvgl_fonts.py 生成）                                */
/* ------------------------------------------------------------------ */
LV_FONT_DECLARE(lv_font_num104);
LV_FONT_DECLARE(lv_font_ascii28);
LV_FONT_DECLARE(lv_font_ui20);
LV_FONT_DECLARE(lv_font_ui16);
LV_FONT_DECLARE(lv_font_ui14);

/* ------------------------------------------------------------------ */
/* 调色板（与原版完全一致）                                            */
/* ------------------------------------------------------------------ */
#define C_GOLD      0xF2ED65u   /* 主金 */
#define C_GOLD_HI   0xFFE27Au   /* 高光金 */
#define C_GOLD_DIM  0x90958Du   /* 暗金：标签、刻度、框标 */
#define C_WARM      0xECEEE5u   /* 暖白：时间、读数 */
#define C_WARN      0xFF6C30u   /* 告警橙 */
#define C_ARC_DIM   0x504418u   /* 弧的未走过部分 */
#define C_BATT_OFF  0x3E3718u   /* 电量空格 */

/* ------------------------------------------------------------------ */
/* 版面常量（对照 face_container.c，单位 = 480x480 设备像素）          */
/* ------------------------------------------------------------------ */
#define SCR_W       480
#define SCR_H       480
#define SCR_CX      240.0f
#define SCR_CY      240.0f

#define ARC_R       206.0f      /* 进度弧半径 */
#define ARC_RI      198.0f      /* 刻度内径 */
#define ARC_RO      210.0f      /* 刻度外径 */
#define ARC_A0      (-150.0f)   /* 弧起点角（数学坐标，y 向下） */
#define ARC_A1      (-30.0f)
/* 历史：ARC_* / C_ARC_DIM 是早期「进度弧」版留下的常量，现在 chrome 全用
 * d_line/bezel_arc 自绘、这些常量没有引用；改版面以 face_draw_cb 里的坐标为准。 */

#define TITLE_CY    88
#define TIME_CY     178
#define BRK_TL      122
#define BRK_TR      358
#define BRK_YT      122
#define BRK_YB      226
#define SEC_CX      402
#define SEC_CY      214
#define DATE_CY     246
#define WEEK_CX     400
#define DATA_X0     120
#define DATA_X1     360
#define DATA_CY     390
#define BATT_Y      454
#define BATT_H      9
#define BRK_ANG_R   224.0f
#define BRK_ANG_SZ  12.0f

#define BAR_Y0      369         /* 渐隐黑条起点 */
#define BAR_Y1      411
#define BAR_ALPHA   232

static lv_image_dsc_t s_photo_dsc;
static lv_image_dsc_t s_panel_dsc;  /* 下半区面板（I8，指向 media 分区 XIP） */

static lv_obj_t *s_face_scr;    /* 表盘所在的 screen（所有页面共用一个） */
static lv_obj_t *s_face_layer;  /* 表盘的页面容器 = 分页层里的第 0 页 */
static lv_obj_t *s_img;         /* 自定义图片图层 */

static lv_obj_t *s_chrome;      /* 刻度/框标/渐变/电量的绘制载体 */
static lv_obj_t *s_lbl_title, *s_lbl_time, *s_lbl_sec, *s_lbl_date,
                *s_lbl_week, *s_lbl_admin, *s_lbl_id, *s_lbl_meet, *s_lbl_batt;


/* 演示状态（真机上这些来自 RTC / eFuse / ADC） */
static int s_hh = 10, s_mm = 8, s_ss = 36;
static int s_year = 2026, s_mon = 9, s_day = 26, s_week = 6;

/*
 * 诊断用（deliverable 2，不改任何渲染语义）：
 * hh:mm 一变就置位，并由 main.c 的 flush 钩子在那一刻的记录完之后清零。
 * 只写 RAM、只在整分取证 armed 时被读，对正常 UI 零影响。
 */
volatile int s_time_minute_changed;
volatile int s_time_minute;
static uint32_t s_admin;
static int s_meets = 0;
static const char *const WEEK_CN[7] = {
    "周日", "周一", "周二", "周三", "周四", "周五", "周六"
};
static int s_draw_x, s_draw_y; /* screen animation offset for static chrome */

/* ------------------------------------------------------------------ */
/* 绘制小工具                                                          */
/* ------------------------------------------------------------------ */

static void d_line(lv_layer_t *L, float x1, float y1, float x2, float y2,
                   uint32_t rgb, lv_opa_t opa, int w)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = (lv_value_precise_t)(x1+s_draw_x);
    d.p1.y = (lv_value_precise_t)(y1+s_draw_y);
    d.p2.x = (lv_value_precise_t)(x2+s_draw_x);
    d.p2.y = (lv_value_precise_t)(y2+s_draw_y);
    d.color = lv_color_hex(rgb);
    d.opa = opa;
    d.width = w;
    lv_draw_line(L, &d);
}

/* Static arc drawing is clipped by LVGL to dirty rectangles. */
static void bezel_arc(lv_layer_t *layer,int radius,int width,int start,int end,uint32_t color,int opa)
{
    lv_draw_arc_dsc_t d;lv_draw_arc_dsc_init(&d);
    d.center.x=240+s_draw_x;d.center.y=240+s_draw_y;d.radius=radius;d.width=width;
    d.start_angle=start;d.end_angle=end;d.color=lv_color_hex(color);d.opa=opa;
    lv_draw_arc(layer,&d);
}
typedef struct { lv_obj_t *obj; int kind,angle,radius; } bezel_hand_t;
/*
 * 表圈指针数量：**时针 + 分针 + 秒针**（3 根）。
 *
 * 历史：
 *   · 2026-10-11 晚按用户要求去掉秒针（`FACE_HAND_COUNT 2`）：秒针每秒动一次
 *     （还带 180ms 补间），是表圈那圈里唯一"高频重绘"的东西；当时正查
 *     "指针刮花表盘"，先用减少 churn 的办法定位问题。
 *   · 2026-10-15 用户要求加回来（"再试一下"）：当时的两个可疑根因都已经处理掉了 ——
 *     ① 细笔画被刷坏 = 刷新窗口宽度取偶（`flush_policy.h`，真机 A/B 已确认）；
 *     ② 旧手印不被重绘 = `hand_move()` 用 480x480 载体标"旧 ∪ 新"并集。
 *     所以这里改回 3 即可；秒针的分支一直在 `hand_draw_one()` / `clock_update()` 里。
 *     每秒的代价：秒数标签 ~3.7K px + 指针并集 ~2K px ≈ 0.6 ms 送屏（40MHz）。
 */
#define FACE_HAND_COUNT 3
static bezel_hand_t s_hands[3];
static int s_clock_synced;
static uint32_t s_clock_tick;
/* 角度 -> 单位方向向量（用 LVGL 整数三角；定义在 hand_draw_one 前面） */
static void hand_unit(int angle, float *ux, float *uy);

static void hand_move(void *var,int32_t angle)
{
    bezel_hand_t *h=var;
    lv_area_t old_box, new_box, uni;
    int nx, ny;

    /* ── "指针刮花表盘" 的最终修法 ────────────────────────────────────
     *
     * 症状：秒针扫过留下扇形拖尾（旧手印不被重绘）。
     *
     * 前两次失败的教训：
     *  1) set_pos 之后再 lv_obj_invalidate_area(小对象, 旧坐标) —— 无效。
     *     LVGL 的 invalidate_area_core() 第一步是
     *        if(!lv_obj_area_is_visible(obj, area)) return LV_RESULT_INVALID;
     *     它拿对象**当前**坐标判定，移动后旧区域不相交 -> 静默丢弃。
     *  2) 改成走 lv_screen_active() 标脏 —— 仍然不行。
     *
     * 根本问题：载体是 32x32 的小对象，"旧位置"的可见性判断天生脆弱。
     * 最终修法（现在）：指针没有载体对象，直接在 chrome 的 DRAW_MAIN 里按角度绘制，
     * 标脏统一走 **480x480 的 chrome 对象**（见 clock_create），于是：
     *    · 任何区域对它而言都"可见" -> 可见性检查必然通过，标脏不会被丢弃
     *    · 只用并集小区域，不是整屏，性能不受影响
     * 指针本身的位置由**角度**算出（见 hand_draw_one），不再依赖对象坐标。
     * ────────────────────────────────────────────────────────────── */
    if(h->angle==angle) return;

    /* 旧位置仍按 32x32 包围盒估算（和 hand_draw_one 用**同一个** hand_unit） */
    {
        float oux,ouy;
        int ox,oy;
        hand_unit(h->angle,&oux,&ouy);
        ox=(int)lroundf(240+h->radius*oux)-16;
        oy=(int)lroundf(240+h->radius*ouy)-16;
        old_box.x1=ox;old_box.y1=oy;old_box.x2=ox+31;old_box.y2=oy+31;
    }

    h->angle=angle;
    {
        float ux,uy;
        hand_unit(angle,&ux,&uy);
        nx=(int)lroundf(240+h->radius*ux)-16;
        ny=(int)lroundf(240+h->radius*uy)-16;
    }
    new_box.x1=nx;new_box.y1=ny;new_box.x2=nx+31;new_box.y2=ny+31;

    /* 旧 ∪ 新，各外扩 3px 吃掉抗锯齿/取整误差 */
    uni.x1=LV_MIN(old_box.x1,new_box.x1)-3;
    uni.y1=LV_MIN(old_box.y1,new_box.y1)-3;
    uni.x2=LV_MAX(old_box.x2,new_box.x2)+3;
    uni.y2=LV_MAX(old_box.y2,new_box.y2)+3;
    /* 通过 chrome（480x480 全屏对象）标脏：任何区域的可见性检查都通过，
     * 不可能被 lv_obj_area_is_visible() 静默丢弃。 */
    if(s_chrome) lv_obj_invalidate_area(s_chrome,&uni);
}

/* 测试/诊断：第 k 根指针当前角度（顺时针十分之一度，12 点为 0） */
int face_lvgl_hand_angle(int k)
{
    if(k<0||k>2) return -1;
    return s_hands[k].angle;
}

/*
 * 诊断：当前表圈绘制的全局偏移 s_draw_x/s_draw_y。
 *
 * 这个偏移来自 chrome 对象的屏幕坐标，会被 d_line/d_rect/bezel_arc
 * **无条件叠加**到每一笔绘制上。如果它和 LVGL 自己的 screen 动画
 * 重复叠加，就会导致"画的位置"和"标脏的位置"错开：
 *   · 刻度反复画在错位处 -> 表圈那一圈糊成一团
 *   · 指针被脏区裁掉一角 -> 指针"变形"
 *   · 错位的线条盖到静态文字上 -> "周日 花了"
 * 而 LVGL 原生绘制的东西（标签）不受影响，所以照片里 `09` 是清晰的。
 */
void face_lvgl_get_draw_offset(int *x, int *y)
{
    if(x) *x = s_draw_x;
    if(y) *y = s_draw_y;
}
/*
 * 角度（顺时针十分之一度，12 点为 0）-> 单位方向向量（屏幕坐标，y 向下）。
 *
 * ★ 用 LVGL 自己的**整数**三角函数 lv_trigo_sin/cos，而不是 cosf/sinf。
 *
 * 为什么：板上表圈的**圆弧是好的**，而圆弧正是用 lv_trigo_* 画的；
 * 用 cosf/sinf 的只有指针，而指针恰恰在板上"变形"、在 PC 预览上正常。
 * 主机是 glibc、板上是 newlib，两边的浮点三角函数结果/精度并不保证一致 ——
 * 这是主机复现不出来的最后一个可疑差异。换成和圆弧同一套整数三角，
 * 既消除了这个差异，也让指针和圆弧的数学完全同源。
 */
static void hand_unit(int angle, float *ux, float *uy)
{
    int deg = (angle / 10) % 360;
    if(deg < 0) deg += 360;
    /* lv_trigo_* 返回 sin/cos × 32768 */
    *ux =  (float)lv_trigo_sin((int16_t)deg) / 32768.0f;
    *uy = -(float)lv_trigo_cos((int16_t)deg) / 32768.0f;  /* y 向下：12 点 = (0,-1) */
}

/*
 * 画第 k 根指针。**由 chrome 的 draw 回调调用**（和刻度同一笔），
 * 不再使用独立的载体对象。
 */
static void hand_draw_one(lv_layer_t *layer,int k)
{
    bezel_hand_t *h=&s_hands[k];
    float ux,uy,tx,ty,cx,cy;
    hand_unit(h->angle,&ux,&uy);
    tx=-uy;ty=ux;
    cx=240+h->radius*ux;cy=240+h->radius*uy;

    if(k==2) {
        /*
         * 秒针：**实心**金色小三角，向内指，比分针的三角小一圈。
         *   · 颜色与分针同为 C_GOLD（用户 2026-10-15 要求："颜色和分针一样"）；
         *   · 做成实心是为了和**空心描边**的分针区分开；
         *   · 尺寸：分针三角是 apex -6 / 底边 +6 / 半宽 7，这里缩到
         *            apex -5 / 底边 +4 / 半宽 5（"比分针小一圈"）；
         *   · 半径 233 仍在分针 223 之外，于是三根指针是"越外越小"的一圈标记。
         * 原来的"2px 短线 + 1px 尾巴"太草率，已被这版替换。
         */
        float x[3]={cx-5*ux,cx+4*ux+5*tx,cx+4*ux-5*tx};
        float y[3]={cy-5*uy,cy+4*uy+5*ty,cy+4*uy-5*ty};
        lv_draw_triangle_dsc_t d;lv_draw_triangle_dsc_init(&d);
        for(int j=0;j<3;j++){d.p[j].x=x[j];d.p[j].y=y[j];}
        d.color=lv_color_hex(C_GOLD);d.opa=255;lv_draw_triangle(layer,&d);
        return;
    }

    float x[3]={cx-6*ux,cx+6*ux+7*tx,cx+6*ux-7*tx};
    float y[3]={cy-6*uy,cy+6*uy+7*ty,cy+6*uy-7*ty};
    if(k==0) {
        lv_draw_triangle_dsc_t d;lv_draw_triangle_dsc_init(&d);
        for(int j=0;j<3;j++){d.p[j].x=x[j];d.p[j].y=y[j];}
        d.color=lv_color_hex(C_WARM);d.opa=255;lv_draw_triangle(layer,&d);
    } else {
        for(int j=0;j<3;j++) d_line(layer,x[j],y[j],x[(j+1)%3],y[(j+1)%3],C_GOLD,255,2);
    }
}

static void clock_create(lv_obj_t *scr)
{
    (void)scr;   /* 不再创建对象：指针由 face_draw_cb 直接画 */
    for(int k=0;k<FACE_HAND_COUNT;k++) {
        bezel_hand_t *h=&s_hands[k];
        h->kind=k;
        h->radius=k==0?212:k==1?223:233;
        h->obj=NULL;
        h->angle=bezel_angle(k,s_hh,s_mm,s_ss);
    }
}

/*
 * host 预览自检兼容钩子（历史：曾返回第 k 根表圈指针的 32x32 载体对象）。
 *
 * 为什么当初需要它：自检原来是"在 screen 的孩子里找 32x32 的对象"，
 * 但**秒数标签「36」恰好也是 32x32**，而且就在 SEC_CX/SEC_CY=(402,214)，
 * 于是自检把秒数标签当成了第 4 根指针，断言失败、预览直接退出。
 *
 * 现在指针在 chrome 的 DRAW_MAIN 里直接绘制、没有载体对象，这里恒返回 NULL；
 * 自检要改用 face_lvgl_hand_angle() 查角度（见 face_lvgl.h）。
 */
void *face_lvgl_hand_obj(int k)
{
    (void)k;
    return NULL;   /* 指针已改为在 chrome 回调里直接绘制，不再有载体对象 */
}
static void clock_update(int before,int after)
{
    uint32_t now=lv_tick_get();
    int animate=s_clock_synced && (after-before+86400)%86400==1 &&
                now-s_clock_tick<2000 && power_is_on() &&
                eez_ui_page()==0 && !tp_lvgl_active_recently();
    if(before==after && s_clock_synced) return;
    for(int k=0;k<FACE_HAND_COUNT;k++) {
        bezel_hand_t *h=&s_hands[k];int angle=bezel_angle(k,s_hh,s_mm,s_ss);
        lv_anim_delete(h,hand_move);
        if(animate && k==2) {
            int from=h->angle%3600;if(angle<from) angle+=3600;
            lv_anim_t a;lv_anim_init(&a);lv_anim_set_var(&a,h);
            lv_anim_set_exec_cb(&a,hand_move);lv_anim_set_values(&a,from,angle);
            lv_anim_set_duration(&a,180);lv_anim_set_path_cb(&a,lv_anim_path_ease_out);lv_anim_start(&a);
        } else hand_move(h,angle);
    }
    s_clock_synced=1;s_clock_tick=now;
}

/* ------------------------------------------------------------------ */
/* 一次画完所有「细线类」元素 + 渐隐条 + 电量                          */
/* ------------------------------------------------------------------ */
static void face_draw_cb(lv_event_t *e)
{
    lv_layer_t *L = lv_event_get_layer(e);
    int i;
    lv_area_t pos;lv_obj_get_coords(s_chrome,&pos);
    s_draw_x=pos.x1;s_draw_y=pos.y1;

    /* Recessed bezel: dark inner wall, rim highlights, interrupted outer rail. */
    bezel_arc(L,238,27,0,360,0x111611,255);
    bezel_arc(L,238,2,0,360,0x3B4438,220);
    bezel_arc(L,232,1,0,360,0xA29F50,145);
    bezel_arc(L,220,3,140,310,0x293025,230);
    bezel_arc(L,214,4,0,360,0x050705,255);
    bezel_arc(L,210,1,0,360,0x414A3D,200);
    /* Short contrasting segments produce the layered rim seen in the reference. */
    for(i=0;i<12;i++) {
        int start=i*30+4;
        bezel_arc(L,237,3,start,start+18,0xA5ADA0,(i%3==0)?220:125);
    }
    for(i=0;i<60;i++) {
        float a=(i*6-90)*3.14159265f/180;
        int major=i%5==0;
        float ri=major?218:223;
        d_line(L,240+ri*cosf(a),240+ri*sinf(a),
               240+228*cosf(a),240+228*sinf(a),
               major?C_WARM:0x727D69,major?220:160,major?2:1);
    }

    /* ---- 3. 标题两侧细线 + 菱形 ---- */
    d_line(L, 178, 104, 230, 104, C_GOLD_DIM, 200, 1);
    d_line(L, 250, 104, 302, 104, C_GOLD_DIM, 200, 1);
    d_line(L, 240, 100, 244, 104, C_GOLD, 235, 1);
    d_line(L, 244, 104, 240, 108, C_GOLD, 235, 1);
    d_line(L, 240, 108, 236, 104, C_GOLD, 235, 1);
    d_line(L, 236, 104, 240, 100, C_GOLD, 235, 1);

    /* ---- 4. 大时间两侧框标 + 底线 ---- */
    d_line(L, BRK_TL, BRK_YB, BRK_TR, BRK_YB, C_GOLD_DIM, 170, 1);

    /* ---- 7. 底部分隔线 ---- */
    d_line(L, DATA_X0, 373, DATA_X1, 373, C_GOLD_DIM, 110, 1);

    /* Registration marks, not a fabricated battery gauge. */
    /* 分页/App 列表是独立视图（apps/app_list.c），表盘这里不画分页点。 */

    /* ---- 10. 指针：和刻度同一笔画出来（见 hand_draw_one 的说明）---- */
    for(int k=0;k<FACE_HAND_COUNT;k++) hand_draw_one(L,k);

    s_draw_x=s_draw_y=0;
}

/* ------------------------------------------------------------------ */
/* 时间：由外部（main.c 的 RTC 轮询）推进，表盘自己不带时钟            */
/* ------------------------------------------------------------------ */

/* 只重画真正变了的那几块 —— 秒变就只脏"秒"那 60x36，绝不动表圈和刻度 */
void face_lvgl_set_time(int year, int mon, int day, int week,
                        int hh, int mm, int ss)
{
    char buf[24];
    int before=s_hh*3600+s_mm*60+s_ss;
    int after=hh*3600+mm*60+ss;

    if (year != s_year || mon != s_mon || day != s_day) {
        s_year = year;
        s_mon = mon;
        s_day = day;
        lv_snprintf(buf, sizeof(buf), "%04d·%02d·%02d", s_year, s_mon, s_day);
        lv_label_set_text(s_lbl_date, buf);
    }

    if (week >= 0 && week <= 6 && week != s_week) {
        s_week = week;
        lv_label_set_text(s_lbl_week, WEEK_CN[s_week]);
    }

    if (hh != s_hh || mm != s_mm) {
        s_hh = hh;
        s_mm = mm;

        lv_snprintf(buf, sizeof(buf), "%02d:%02d", s_hh, s_mm);
        lv_label_set_text(s_lbl_time, buf);

        /* 诊断信号：分钟（或小时）刚变，main.c 的 flush 钩子据此抓那一帧 */
        s_time_minute_changed = 1;
        s_time_minute = hh * 100 + mm;
    }

    if (ss != s_ss) {
        s_ss = ss;
        lv_snprintf(buf, sizeof(buf), "%02d", s_ss);
        lv_label_set_text(s_lbl_sec, buf);
    }

    clock_update(before,after);

}

/* 触摸唤醒后调用：只补"时间/日期/秒"那几块小区域，绝不重画整屏。
 *
 * 为什么可以这样：这块 AMOLED 驱动 IC 自带 GRAM（480x480 RGB565 一帧要
 * 24 ms 才送得完，屏却按 60 Hz 自己扫描 —— 主机根本喂不过来，
 * 所以显存必然在屏那边）。DISPOFF 只是停掉扫描输出，GRAM 内容还在，
 * DISPON 就把上一帧原样显示回来。之前在唤醒时整屏 invalidate 一遍，
 * 白白多了 24 ms 送屏 + 一次全屏渲染，这就是"唤醒感觉有延迟"的来源。 */
void face_lvgl_wake_refresh(void)
{
    lv_obj_invalidate(s_lbl_time);
    lv_obj_invalidate(s_lbl_sec);
    lv_obj_invalidate(s_lbl_date);
    lv_obj_invalidate(s_lbl_week);
    clock_update(-1,s_hh*3600+s_mm*60+s_ss);
}

/* ------------------------------------------------------------------ */
/* 创建表盘                                                            */
/* ------------------------------------------------------------------ */
void face_lvgl_create(void)
{
    lv_obj_t *scr;
    char buf[24];
    lv_obj_t *l;

    /*
     * 2026-10-12：表盘不再直接画在 screen 上，而是画在一个 480x480 的页面
     * 容器里 —— 和原生 App 页平级（PAGE_N=8，见 eez_ui_glue.c），由分页层统一摆放
     * （在屏幕外 = 零成本；被压住 = 被遮挡判断跳过）。见那边的长注释。
     */
    if (!s_face_layer) {
        s_face_layer = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(s_face_layer);
    }
    scr = s_face_layer;
    s_face_scr = lv_screen_active();

    for(int k=0;k<3;k++) lv_anim_delete(&s_hands[k],hand_move);
    s_clock_synced=0;
    lv_obj_clean(scr);
    lv_obj_set_size(scr, SCR_W, SCR_H);
    lv_obj_set_pos(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* F1：编号改由 services/svc_device_id.c 统一提供（与 BLE 广播名 / GET_INFO
     * 用的是同一个 24 bit 值）；界面仍显示 6 位十六进制。 */
    s_admin = svc_device_id_admin();

    printf("[face] admin_id = %06X (watch photo)%s\r\n", (unsigned)s_admin,
           svc_device_id_chip_ok() ? "" : " [efuse read failed: fallback]");

    /* ---- 1. Flash-resident custom photo, with offline edge gradients ---- */
    memset(&s_photo_dsc, 0, sizeof(s_photo_dsc));
    s_photo_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_photo_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_photo_dsc.header.w = WATCH_PHOTO_W;
    s_photo_dsc.header.h = WATCH_PHOTO_H;
    s_photo_dsc.header.stride = WATCH_PHOTO_W * 2;
    s_photo_dsc.data_size = WATCH_PHOTO_BYTES;
    s_photo_dsc.data = watch_photo_pixels;

    s_img = lv_image_create(scr);
    lv_image_set_src(s_img, &s_photo_dsc);
    lv_obj_set_pos(s_img, 0, WATCH_PHOTO_Y);

    /* ---- 2. 下半区面板（76 §2.2）：装了就把内置 watch photo 换掉 ----
     *
     * 面板和 watch_photo **完全同位置同尺寸**（0,248 / 480x232），所以 chrome
     * （刻度/框标/表针/日期/电量）在下面照旧创建，自动盖在图片上层 ——
     * 上半区时间/表针的实时重绘一点点都不受影响。
     *
     * s_panel_dsc 必须是静态的：LVGL 只保存指针，不拷贝 dsc，也不拷贝像素
     * （像素始终指向 media 分区的 XIP 地址，112 KB 一个字节都不进 RAM）。
     * 每次 face_lvgl_create() 都重新指一次，这样"安装/清除面板"只靠重建 face 0
     * 就能生效，不需要重开 LVGL 或重烧固件。
     *
     * 面板没装（header 无效/从未装过）时保持内置 watch photo = 现有 face 0 视觉。
     */
    if (face_panel_available()) {
        memset(&s_panel_dsc, 0, sizeof(s_panel_dsc));
        s_panel_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_panel_dsc.header.cf = LV_COLOR_FORMAT_I8;
        s_panel_dsc.header.w = (uint16_t)FACE_PANEL_W;
        s_panel_dsc.header.h = (uint16_t)FACE_PANEL_H;
        s_panel_dsc.header.stride = (uint16_t)FACE_PANEL_W;   /* I8：1 B/px */
        s_panel_dsc.data_size = FACE_PANEL_IMAGE_LEN;         /* palette + 像素 */
        s_panel_dsc.data = face_panel_image_ptr();
        lv_image_set_src(s_img, &s_panel_dsc);
    }

    /* ---- 3. chrome：刻度 / 框标 / 渐变 / 电量（一次 draw 事件画完）---- */
    s_chrome = lv_obj_create(scr);
    lv_obj_remove_style_all(s_chrome);
    lv_obj_set_size(s_chrome, SCR_W, SCR_H);
    lv_obj_set_pos(s_chrome, 0, 0);
    lv_obj_remove_flag(s_chrome, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_chrome, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_chrome, face_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    clock_create(scr);

    /* ---- 4. 文字 ---- */

    /* 标题 */
    l = s_lbl_title = lv_label_create(scr);
    lv_label_set_text(l, "ENDFIELD INDUSTRIES");
    lv_obj_set_style_text_font(l, &lv_font_ui16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_GOLD), 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, TITLE_CY - 240);

    /* 大时间 */
    l = s_lbl_time = lv_label_create(scr);
    lv_snprintf(buf, sizeof(buf), "%02d:%02d", s_hh, s_mm);
    lv_label_set_text(l, buf);
    lv_obj_set_style_text_font(l, &lv_font_num104, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_WARM), 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, TIME_CY - 240);

    /* 秒 */
    l = s_lbl_sec = lv_label_create(scr);
    lv_snprintf(buf, sizeof(buf), "%02d", s_ss);
    lv_label_set_text(l, buf);
    lv_obj_set_style_text_font(l, &lv_font_ascii28, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_GOLD), 0);
    lv_obj_align(l, LV_ALIGN_CENTER, SEC_CX - 240, SEC_CY - 240);

    /* 日期 */
    l = s_lbl_date = lv_label_create(scr);
    lv_snprintf(buf, sizeof(buf), "%04d·%02d·%02d", s_year, s_mon, s_day);
    lv_label_set_text(l, buf);
    lv_obj_set_style_text_font(l, &lv_font_ui20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_GOLD), 0);
    lv_obj_set_style_text_letter_space(l, 5, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, DATE_CY - 240);

    /* 周几 */
    l = s_lbl_week = lv_label_create(scr);
    lv_label_set_text(l, WEEK_CN[s_week]);
    lv_obj_set_style_text_font(l, &lv_font_ui14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_WARM), 0);
    lv_obj_set_style_text_letter_space(l, 3, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, WEEK_CX - 240, DATE_CY - 240);

    /* 左下：管理员 */
    l = s_lbl_admin = lv_label_create(scr);
    lv_label_set_text(l, "管理员");
    lv_obj_set_style_text_font(l, &lv_font_ui14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_GOLD_DIM), 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, DATA_X0, DATA_CY - 240);

    l = s_lbl_id = lv_label_create(scr);
    lv_snprintf(buf, sizeof(buf), "%06X", (unsigned)(s_admin & 0xFFFFFFu));
    lv_label_set_text(l, buf);
    lv_obj_set_style_text_font(l, &lv_font_ui16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_WARM), 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, DATA_X0 + 56, DATA_CY - 240);

    /* 右下：相遇 */
    l = s_lbl_meet = lv_label_create(scr);
    lv_snprintf(buf, sizeof(buf), "LOCAL / 01");
    lv_label_set_text(l, buf);
    lv_obj_set_style_text_font(l, &lv_font_ui14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(s_meets ? C_GOLD : C_GOLD_DIM), 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_align(l, LV_ALIGN_RIGHT_MID, DATA_X1 - SCR_W, DATA_CY - 240);

    /* 底部：真实电量（services/svc_battery.c 采样，不是写死的百分比）
     * 左对齐贴在「管理员」那一行下面，跟它组成一列；
     * 不下沉太多是为底部留白（原分页标记已随 F1 删除）。 */
    l = s_lbl_batt = lv_label_create(scr);
    lv_label_set_text(l, "BAT --.-V");
    lv_obj_set_style_text_font(l, &lv_font_ui14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_GOLD_DIM), 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, DATA_X0 + 6, 408 - 240);

    {
        const char *cardinal[]={"12","03","06","09"};
        const int xy[4][2]={{240,43},{436,240},{240,439},{44,240}};
        for(int k=0;k<4;k++) {
            l=lv_label_create(scr);lv_label_set_text(l,cardinal[k]);
            lv_obj_set_style_text_font(l,&lv_font_ui14,0);
            lv_obj_set_style_text_color(l,lv_color_hex(0x8E9788),0);
            lv_obj_align(l,LV_ALIGN_CENTER,xy[k][0]-240,xy[k][1]-240);
        }
    }
    /* Static image: redraw only for actual UI invalidation. */
}

void *face_lvgl_screen(void)
{
    return s_face_scr;
}

void *face_lvgl_page(void)
{
    return s_face_layer;
}

/*
 * 更新电量读数。
 *
 * 格式化成 "BAT 3.92V 62%"，充电时追加 " CHG"（2026-10-17 起 U5 的 CHRG
 * 飞线到了 IO21，所以"在不在充电"是硬件真值，不再是猜的）。
 *
 * 刻意做两件省事：变化不足 8mV 就不刷；低于 15% 换成告警橙。
 * 这样上层可以放心地周期调用（比如每 5 秒）。
 */
static int s_bat_mv = -100000;
static int s_bat_pct = -1;
static int s_bat_chg;
static int s_bat_warn;          /* 0=正常 1=低电 2=极低，见 face_lvgl_set_power_warn */
static int s_bat_shown_mv = -100000;
static int s_bat_shown_chg = -1;
static int s_bat_shown_warn = -1;

static void bat_render(void)
{
    char buf[40];
    uint32_t color;

    if (!s_lbl_batt) {
        return;
    }
    if (s_bat_mv < 0) {
        if (s_bat_shown_mv != -1) {
            s_bat_shown_mv = -1;
            s_bat_shown_chg = -1;
            s_bat_shown_warn = -1;
            lv_label_set_text(s_lbl_batt, "BAT --.-V");
        }
        return;
    }
    if (s_bat_mv == s_bat_shown_mv && s_bat_chg == s_bat_shown_chg &&
        s_bat_warn == s_bat_shown_warn) {
        return;
    }
    s_bat_shown_mv = s_bat_mv;
    s_bat_shown_chg = s_bat_chg;
    s_bat_shown_warn = s_bat_warn;

    if (s_bat_chg) {
        lv_snprintf(buf, sizeof(buf), "BAT %d.%02dV %d%% CHG",
                    s_bat_mv / 1000, (s_bat_mv % 1000) / 10, s_bat_pct);
    } else if (s_bat_warn >= 2) {
        lv_snprintf(buf, sizeof(buf), "BAT %d.%02dV %d%% LOW",
                    s_bat_mv / 1000, (s_bat_mv % 1000) / 10, s_bat_pct);
    } else {
        lv_snprintf(buf, sizeof(buf), "BAT %d.%02dV %d%%",
                    s_bat_mv / 1000, (s_bat_mv % 1000) / 10, s_bat_pct);
    }
    lv_label_set_text(s_lbl_batt, buf);

    if (s_bat_chg) {
        color = C_GOLD;                          /* 充电中：亮金 */
    } else if (s_bat_warn >= 2) {
        color = 0xFF3B30u;                       /* 极低：红 */
    } else if (s_bat_warn >= 1 || s_bat_pct <= 15) {
        color = C_WARN;                          /* 低电：橙 */
    } else {
        color = C_GOLD_DIM;
    }
    lv_obj_set_style_text_color(s_lbl_batt, lv_color_hex(color), 0);
}

void face_lvgl_set_battery(int mv, int percent)
{
    if (mv >= 0 && percent < 0) {
        percent = 0;
    }
    s_bat_mv = mv;
    s_bat_pct = percent;

    /* 电量变化很小的时候不值得重绘（这一行文字会走完整条刷屏链路） */
    if (mv >= 0 && s_bat_shown_mv >= 0 &&
        mv > s_bat_shown_mv - 8 && mv < s_bat_shown_mv + 8 &&
        s_bat_chg == s_bat_shown_chg && s_bat_warn == s_bat_shown_warn) {
        return;
    }
    bat_render();
}

/*
 * 低电告警等级（2026-10-18）：
 *   0 = 正常
 *   1 = 低电（≤3.45 V）：标签变橙
 *   2 = 极低（≤3.35 V）：标签变红 + 后缀 LOW
 * 充电中一律按 0 显示（CHG 优先）。
 */
void face_lvgl_set_power_warn(int level)
{
    if (level < 0) {
        level = 0;
    }
    if (level > 2) {
        level = 2;
    }
    if (level == s_bat_warn) {
        return;
    }
    s_bat_warn = level;
    bat_render();
}

void face_lvgl_set_charging(int on)
{
    int v = on ? 1 : 0;

    if (v == s_bat_chg) {
        return;
    }
    s_bat_chg = v;
    bat_render();
}
