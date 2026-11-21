/* [历史/未编译] terrain565.c —— 旧"地形"表盘的背景生成器；2026-11 起表盘不再用它，
 * CMake 也不编译。保留仅作参考，别看它判断当前渲染路径（表圈/刻度由 face_lvgl.c 自绘）。 */
/* Seeded plan-view topography. Isolines of ONE 2-D height field, not stacked waves.
 * A sampling grid is built once; bilinear interpolation and gradient-normalized
 * coverage produce consistent, anti-aliased contour widths at different slopes.
 *
 * ⚠️ 2026-10-11 晚 · 内存修正（这块直接导致过"上电黑屏"）：
 *   原来是 `static float height[121][121]` = 57.6 KB，加上 217 KB 的地形画布，
 *   把静态 RAM 顶到 89%，剩余堆不够 LVGL 建 EEZ 那 44 个控件 →
 *   lv_malloc 失败 → 设备端 LV_USE_ASSERT_MALLOC=1 命中断言 → 默认处理器是
 *   `while(1)` → LVGL 任务死循环 → **屏幕永远不亮，但 USB 还活着**。
 *   （主机预览发现不了：PC 内存管够。）
 *
 *   现在：高度场改 int16_t（数值范围约 -50..1400，int16 足够），
 *   采样步长 4→8。占用从 57.6 KB 降到 7.4 KB。
 */
#include "terrain565.h"
#include <math.h>
#include <string.h>

#define GRID 61
#define STEP 8
#define CONTOUR_INTERVAL 160.0f
static int16_t height[GRID][GRID];
static uint32_t seed;
static uint8_t growth;
static int dirty=1;
static uint32_t hash(uint32_t x)
{ x^=x>>16;x*=0x7feb352dU;x^=x>>15;x*=0x846ca68bU;return x^(x>>16); }
static float random01(unsigned n) { return (hash(seed+n*0x9e3779b9U)&65535)/65535.0f; }
static float mix(float a,float b,float t) { return a+(b-a)*t; }
static float noise(float x,float y,unsigned salt)
{
    int ix=(int)floorf(x),iy=(int)floorf(y);
    float u=x-ix,v=y-iy;u=u*u*(3-2*u);v=v*v*(3-2*v);
    float a=(hash(seed+salt+(uint32_t)ix*73856093U+(uint32_t)iy*19349663U)&65535)/65535.0f;
    float b=(hash(seed+salt+(uint32_t)(ix+1)*73856093U+(uint32_t)iy*19349663U)&65535)/65535.0f;
    float c=(hash(seed+salt+(uint32_t)ix*73856093U+(uint32_t)(iy+1)*19349663U)&65535)/65535.0f;
    float d=(hash(seed+salt+(uint32_t)(ix+1)*73856093U+(uint32_t)(iy+1)*19349663U)&65535)/65535.0f;
    return mix(mix(a,b,u),mix(c,d,u),v);
}
void terrain565_init(uint32_t s) { seed=s;dirty=1; }
uint32_t terrain565_seed(void) { return seed; }
void terrain565_set_growth(uint8_t g) { if(g!=growth) {growth=g;dirty=1;} }
uint8_t terrain565_get_growth(void) { return growth; }
void terrain565_begin_frame(uint32_t ms)
{
    (void)ms; /* Immutable terrain: no random drift, no per-second recomputation. */
    if(!dirty) return;
    float cx[6],cy[6],rx[6],ry[6],amp[6];
    for(int k=0;k<6;k++) {
        cx[k]=45+random01(k*5+1)*390;cy[k]=245+random01(k*5+2)*200;
        rx[k]=58+random01(k*5+3)*72;ry[k]=42+random01(k*5+4)*55;
        amp[k]=(k==5?-400:600+random01(k*5+5)*650)*(1+growth/800.0f);
    }
    for(int y=0;y<GRID;y++) for(int x=0;x<GRID;x++) {
        float px=x*STEP,py=y*STEP;
        float wx=px+25*(noise(px/93,py/93,17)-0.5f);
        float wy=py+22*(noise(px/77,py/77,73)-0.5f);
        float z=90*noise(px/60,py/60,121)+18*noise(px/27,py/27,819);
        for(int k=0;k<6;k++) {
            float dx=(wx-cx[k])/rx[k],dy=(wy-cy[k])/ry[k];
            float q=1+dx*dx+dy*dy;
            z+=amp[k]/(q*q);
        }
        height[y][x]=(int16_t)lrintf(z);
    }
    dirty=0;
}
static float clamp(float x) { return x<0?0:x>1?1:x; }
void terrain565_render_band(tf565_t *g)
{
    if(!g || !g->px || g->band_h<=0) return;
    terrain565_begin_frame(0);
    for(int by=0;by<g->band_h;by++) {
        int y=g->band_y+by;
        if(y<0 || y>=480) continue;
        int iy=y/STEP;float v=(y%STEP)/(float)STEP;
        for(int x=0;x<480;x++) {
            float rr=(x-240)*(x-240)+(y-240)*(y-240);
            if(rr>=207*207 || y<248) continue;
            int ix=x/STEP;float u=(x%STEP)/(float)STEP;
            float a=height[iy][ix],b=height[iy][ix+1],c=height[iy+1][ix],d=height[iy+1][ix+1];
            float z=mix(mix(a,b,u),mix(c,d,u),v);
            float dx=mix(b-a,d-c,v)/STEP,dy=mix(c-a,d-b,u)/STEP;
            float grad=sqrtf(dx*dx+dy*dy)+0.12f;
            int level=(int)floorf(z/CONTOUR_INTERVAL+0.5f);
            int major=level%4==0;
            float dist=fabsf(z-level*CONTOUR_INTERVAL)/grad;
            float ink=clamp((major?1.3f:0.95f)-dist);
            float fade=clamp((y-248)/22.0f)*clamp((207*207-rr)/2600.0f);
            float light=clamp(0.4f+(-dx-dy)*0.028f);
            /* 配色：中性偏暖暗底 + 金琥珀等高线。
             * 原来细线是 (68,89,88) 青绿、底色 b>r 偏蓝，整体发青蓝，
             * 丢了"黑底金线"的仪器感。细线=暗金，主线=亮金(GOLD_TRACE 196,164,48)。 */
            float r=6+light*6,gc=7+light*6,bc=4+light*4;
            r=mix(r,major?196:112,ink);gc=mix(gc,major?164:96,ink);bc=mix(bc,major?48:44,ink);
            g->px[by*480+x]=TF565_PACK((int)(r*fade),(int)(gc*fade),(int)(bc*fade));
        }
    }
}
