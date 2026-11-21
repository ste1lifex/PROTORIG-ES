/* [历史/未编译] terrain565.h —— 旧"地形"表盘接口；CMake 不编译，保留仅作参考。
 * 别看它判断当前渲染路径。 */
/* Seeded 2-D topography with true height isolines, RGB565 band output.
 * Hand-maintained: do NOT overwrite with the old fork_terrain565.py. */
#ifndef _TERRAIN565_H_
#define _TERRAIN565_H_

#include <stdint.h>

#define TF565_W      480
#define TF565_BAND_H 256      /* 条带缓冲最大行数，必须 >= 画布高 */

/* RGB888 -> RGB565 */
#define TF565_PACK(r, g, b) \
    ((uint16_t)((((uint32_t)(r) & 0xF8u) << 8) | (((uint32_t)(g) & 0xFCu) << 3) | ((uint32_t)(b) >> 3)))

typedef struct {
    uint16_t *px;      /* TF565_W * band_h，RGB565 */
    int band_y;        /* 条带起始行（设备坐标） */
    int band_h;        /* 条带行数 */
} tf565_t;

/* 建查找表 + 按 seed 生成地貌（seed = 管理员编号） */
void terrain565_init(uint32_t seed);
uint32_t terrain565_seed(void);

/* 「相遇」越多地形越长高（每 8 次一档，封顶 200） */
void terrain565_set_growth(uint8_t g);
uint8_t terrain565_get_growth(void);

/* Build cached height field only when seed/growth changes. t_ms does not animate it. */
void terrain565_begin_frame(uint32_t t_ms);

/* 把地形画进给定条带（调用前条带需已清零为黑） */
void terrain565_render_band(tf565_t *g);

#endif /* _TERRAIN565_H_ */
