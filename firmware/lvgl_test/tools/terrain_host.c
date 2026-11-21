/*
 * terrain_host.c —— 在 PC 上跑 terrain565.c：既做性能剖析，也导出烘焙图
 *
 * 编译（msys2 mingw64 gcc）：
 *     gcc -O2 -o terrain_host.exe terrain_host.c ../terrain565.c -lm
 *
 * 它做三件事：
 *   1. 分别计时 begin_frame / render_band 三趟（等高环 / 辉光 / 扫描线）
 *   2. 导出 PPM（可转 PNG 直接看效果）
 *   3. 导出 RGB565 的 C 数组（如果决定把地形烘焙进 flash）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../terrain565.h"

#define TER_H 232
#define TER_PX (TF565_W * TER_H)

static uint16_t canvas[TER_PX] __attribute__((aligned(64)));
static volatile uint32_t g_sink;      /* 防止渲染结果被当成死代码优化掉 */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void dump_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    int i;

    fprintf(f, "P6\n%d %d\n255\n", TF565_W, TER_H);
    for (i = 0; i < TER_PX; i++) {
        uint16_t c = canvas[i];
        unsigned char rgb[3];
        rgb[0] = (unsigned char)(((c >> 11) & 0x1F) * 255 / 31);
        rgb[1] = (unsigned char)(((c >> 5) & 0x3F) * 255 / 63);
        rgb[2] = (unsigned char)((c & 0x1F) * 255 / 31);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("  写出 %s\n", path);
}

static void dump_c(const char *path, const char *name)
{
    FILE *f = fopen(path, "w");
    int i;

    fprintf(f, "/* 由 tools/terrain_host.c 生成 —— 地形烘焙图 %dx%d RGB565 */\n",
            TF565_W, TER_H);
    fprintf(f, "#include <stdint.h>\n\n");
    fprintf(f, "const uint16_t %s[%d] = {\n", name, TER_PX);
    for (i = 0; i < TER_PX; i++) {
        if (i % 12 == 0) fprintf(f, "    ");
        fprintf(f, "0x%04X,", canvas[i]);
        if (i % 12 == 11) fprintf(f, "\n");
    }
    fprintf(f, "\n};\n");
    fclose(f);
    printf("  写出 %s (%.1f KB)\n", path, (double)i * 2 / 1024.0);
}

int main(int argc, char **argv)
{
    uint32_t seed = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 0) : 0x015762u;
    int frames = (argc > 2) ? atoi(argv[2]) : 200;
    tf565_t g;
    double t0, t_init, t_begin = 0, t_render = 0, t_memset = 0;
    int i;

    printf("=== terrain565 主机端剖析 ===\n");
    printf("seed=0x%06X  frames=%d  canvas=%dx%d (%.1f KB RGB565)\n\n",
           seed, frames, TF565_W, TER_H, (double)TER_PX * 2 / 1024.0);

    t0 = now_ms();
    terrain565_init(seed);
    t_init = now_ms() - t0;
    terrain565_set_growth(24);
    printf("terrain565_init (建 LUT + 一次剖面) : %8.2f ms\n", t_init);

    g.px = canvas;
    g.band_y = 0;
    g.band_h = TER_H;

    /* 预热 */
    terrain565_begin_frame(0);
    memset(canvas, 0, sizeof(canvas));
    terrain565_render_band(&g);

    for (i = 0; i < frames; i++) {
        double a, b, c;

        a = now_ms();
        terrain565_begin_frame((uint32_t)(i * 33));      /* 33ms 一帧 = 30fps */
        /* ⚠️ 内存屏障：不加的话 gcc 会认定"每帧结果都被下一帧覆盖"是死代码，
         *    把整个 render_band 优化掉，测出来 0.02ms 这种假数字 */
        __asm__ __volatile__("" ::: "memory");
        b = now_ms();
        memset(canvas, 0, sizeof(canvas));
        c = now_ms();
        terrain565_render_band(&g);
        /* 全量求和：逼编译器把每个像素的写入真正落地（否则整趟会被删掉） */
        {
            uint32_t sum = 0;
            int j;
            for (j = 0; j < TER_PX; j++) {
                sum += canvas[j];
            }
            g_sink = sum;
        }
        t_begin += b - a;
        t_memset += c - b;
        t_render += now_ms() - c;
    }

    printf("\n--- 每帧平均（%d 帧）---\n", frames);
    printf("  begin_frame  (按列算剖面)  : %8.3f ms\n", t_begin / frames);
    printf("  memset 清画布 (217 KB)     : %8.3f ms\n", t_memset / frames);
    printf("  render_band  (逐像素三趟)  : %8.3f ms\n", t_render / frames);
    printf("  -------------------------------------\n");
    printf("  合计                       : %8.3f ms  -> 上限 %.1f fps\n",
           (t_begin + t_memset + t_render) / frames,
           1000.0 / ((t_begin + t_memset + t_render) / frames));

    /* 导出最后一帧 */
    printf("\n--- 导出 ---\n");
    memset(canvas, 0, sizeof(canvas));
    terrain565_begin_frame(1234);
    terrain565_render_band(&g);
    dump_ppm("terrain_host.ppm");
    if (argc > 3) {
        dump_c(argv[3], "terrain_baked");
    }
    return 0;
}
