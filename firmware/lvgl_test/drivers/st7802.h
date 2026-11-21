/**
 * @file  st7802.h
 * @brief ST7802 480x480 AMOLED QSPI 驱动（PROTORIG ES V1.6 / 面板 AM160Q480480LK5）
 *
 * 硬件连线（V1.6 原理图 schematic/SCH_Schematic1_1-P1_2026-10-18.png，与 SDK bl616dk 参考设计一致）：
 *   IO10 -> LCD_CLK   (QSPI_CLK)
 *   IO11 -> LCD_CS
 *   IO12 -> LCD_SDA0  (SI00 / DAT0)
 *   IO13 -> LCD_SDA1  (SI01 / DAT1)
 *   IO14 -> LCD_SDA2  (SI02 / DAT2)
 *   IO15 -> LCD_SDA3  (SI03 / DAT3)
 *   IO27 -> LCD_RST        （屏 pin15）
 *   IO28 -> LCD_TE         （屏 pin16，TE 输出）
 *   IO29 -> LCD_VCI_EN     （屏 pin23，PMIC BV6810 使能）
 *   每条 QSPI 线上串了 22Ω（R5~R11）
 *
 * MSPI(QSPI) 协议（ST7802 手册 8.4 节 + 厂商 esp_lcd_st7802_spi.c）：
 *   写命令:  instruction 0x02 + AD[23:0] = 00 CMD 00 + 参数
 *   写像素:  instruction 0x32 + AD[23:0] = 00 2C 00 + 像素
 *   读:      instruction 0x03 + AD[23:0] = 00 CMD 00 + 读回数据
 *   注意 instruction 永远走 1 线；AD 和 data 可以走 1/2/4/8 线。
 *   本驱动：命令阶段全部 1 线（A1D1），像素阶段地址 1 线 + 数据 4 线（A1D4）。
 */
#ifndef __ST7802_H
#define __ST7802_H

#include <stdint.h>

#define ST7802_WIDTH   480
#define ST7802_HEIGHT  480

/* QSPI 时钟默认值 —— 都是实测出来的，不是照手册抄的。
 *
 * 读：手册 7.5.3 写 "Serial clock cycle(Read) 最小 300ns"（≈3.3MHz），
 *     但实测 1/2/4/6/8/10/15 MHz 全部读回正确的 COLMOD，
 *     20 MHz 开始失败，失效值是正确值右移一位，
 *     说明是采样点晚了一个时钟（面板 TACC 10~120ns 超出 BL616 采样余量），
 *     不是随机错误。所以真实上限约 15MHz，这里取 10MHz 留余量。
 *
 * 写（2026-09-26 实测扫描，480x480 RGB888 整帧）：
 *     15 MHz -> 125 ms
 *     25 MHz ->  82 ms
 *     40 MHz ->  65 ms   （约 15 fps）
 *   手册 7.5.3 写 Serial clock cycle(Write) 最小 20ns -> 上限 50MHz，
 *   所以 40MHz 仍在规格内。再往上提升有限：整帧 = 传输 + 约 30ms 固定开销
 *   （渲染 + 灌 FIFO + 每带的 CASET/RASET），所以瓶颈会转移到 CPU 侧。
 *   默认取 40MHz；想更稳可以 shell 里 `wrclk 25`。
 */
#define ST7802_WRITE_HZ (40 * 1000 * 1000)
#define ST7802_READ_HZ  (10 * 1000 * 1000)

/** 上电时序 + DBI 初始化 + 面板初始化，返回 0 成功 */
int st7802_init(void);

/**
 * @brief 手动配置 DBI（诊断用）
 * @param hz        时钟
 * @param clk_mode  DBI_CLOCK_MODE_0/1/3
 * @param addr_wire DBI_QSPI_WIRE_NUM_1 / _4
 * @param data_wire DBI_QSPI_WIRE_NUM_1 / _4
 */
void st7802_cfg(uint32_t hz, uint8_t clk_mode, uint8_t addr_wire, uint8_t data_wire);

/** 设置/读取时钟（读写各一个）。下一个操作会自动重配 DBI。 */
void st7802_set_read_hz(uint32_t hz);
void st7802_set_write_hz(uint32_t hz);
uint32_t st7802_get_read_hz(void);
uint32_t st7802_get_write_hz(void);

/**
 * 送屏（blit）通路诊断：按"块"记录 DBI 状态位，只写 RAM，事后 dump。
 * 查"整分时大数字那块发花"用 —— 现有代码只等 TC，从不看 FIFO_ERR。
 */
void st7802_flush_diag_dump(void);
void st7802_flush_diag_reset(void);

/**
 * @brief 读时钟扫描：把读时钟从低到高扫一遍，每次读 RDDCOLMOD(0Ch)。
 *        哪个频率开始读不到 0x77，就是读路径的真实上限。
 *        （第一次测出"30MHz 读不到"的时候接线是错的，所以要用这个重新定。）
 */
void st7802_read_clock_sweep(void);

/**
 * @brief 写时钟扫描：把写时钟从低到高扫一遍，每次用该频率写一个 COLMOD 值，
 *        再用 2MHz 读回来比对。读得回来就说明那个写频率是可靠的。
 */
void st7802_write_clock_sweep(void);

/** 按当前配置写命令 */
int st7802_write_cmd(uint8_t cmd, const uint8_t *data, uint32_t len);

/** 按当前配置读命令（调用前请先用 st7802_cfg 把时钟降到 2MHz 左右）
 *
 *  ⚠️ 上限 8 字节 —— 这是 **BL616 DBI 硬件**的限制，不是本驱动的：
 *     lhal 只有 DBI_RDATA_0/1 两个 32 位回读寄存器
 *     （`DBI_READ_DATA_BYTE_MAX 8`，`bflb_dbi_send_cmd_read_data()` 里
 *      也只从这两个寄存器搬 4+4 字节）。要读更多只能拆成多次 8 字节事务，
 *     见下面的 st7802_read_cmd_n()。寄存器回读继续走这个函数，不要动它。 */
int st7802_read_cmd(uint8_t cmd, uint8_t *data, uint32_t len);

/**
 * @brief 分块长读：把一次"读 N 字节"拆成多次 4/8 字节的读事务。
 *
 * 每一块都是"instruction 0x03 + AD[23:0]=00 CMD 00 + 读 len_i 字节"，
 * 也就是**每条命令都带自己的地址相位**，与 SDK 的 bflb_dbi_send_cmd_read_data
 * 完全同一条路径，只是重复多次。面板是否会把读指针在事务之间继续推进
 * **未经验证**（见 local/docs/history/minute_digit_artifact_20261015.md 第 13.2 节）：
 * 如果每块读回同一组字节，说明读指针在事务间复位，这个函数就不能拿来做
 * 整窗口回读。
 *
 * @param cmd      读命令（0x04=RDDID…；0x2E=RAMRD 本手册未列，2026-10-16 实测读不到 GRAM，别用）
 * @param buf      输出缓冲
 * @param len      总字节数
 * @param max_chunk 每次事务最多读几字节（1..8，0 按 8）
 * @return 实际读回的字节数
 */
uint32_t st7802_read_cmd_n(uint8_t cmd, uint8_t *buf, uint32_t len, uint8_t max_chunk);

/**
 * @brief GRAM/寄存器回读探测：定窗口 -> 读命令 -> 读回并打十六进制。
 *
 * ⚠️ 2026-10-16 结论（别再走）：**本面板没有 GRAM 读命令。**
 *    手册 v0.3 的 UCS 表里没有 2Eh(RAMRD)；可读命令只有
 *    04/05/09/0A/0B/0C/0D/0E/0F/45/52/54/56/59/5B/5D/A1/A8/AA/AF/DA~DD。
 *    所以 `cmd=0x2E`（本函数的默认）读到的不是 GRAM，和图案对不上是必然的。
 *    要读**寄存器**请传 cmd=0x0A 这种真实存在的读命令，并让 w*h==1（1 字节事务）。
 *
 * @param x,y,w,h   读窗口（内部换算 CASET/RASET，和 blit565 同一套写法）
 * @param cmd       读命令，0 = 默认 0x2E(RAMRD，本手册未列 —— 见上面的警告)
 * @param max_chunk 每次事务的最大读长度 1..8（0 = 8）
 * @param pat_check 非 0：把回读值当 pxpat 编码图案解释，
 *                  自动扫 0..2 字节整体偏移并按地址规律打分
 * @return 实际读回的字节数，<0 出错
 */
int st7802_gramrd(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                  uint8_t cmd, uint8_t max_chunk, int pat_check);

/**
 * @brief 面板状态自证：读 RDDPM(0Ah)/RDDST(09h)/RDCOLMOD(0Ch) 等寄存器并解码。
 *
 * 真机实测（用户肉眼同时确认）：RDDPM 屏亮=0x9C、息屏=0x98，DISON 是 0x08 位。
 * 这是**唯一**能证明"面板真的在显示"的软件判据 —— 固件自己的 screen 标志不算。
 *
 * @return 1 = 面板在显示（DISON=1）；0 = 面板没在显示；-1 = 读通路自证失败
 */
int st7802_panel_probe(void);

/** RDDPM 位域的可读描述（返回静态缓冲，别存指针） */
const char *st7802_panel_power_str(uint8_t rddpm);

/**
 * @brief 「收尾排空等待」实验开关（默认 0 = 关，生产路径零影响）。
 *
 *   0 = 收尾只等 TC（原行为）
 *   1 = 收尾额外等 DBI_INTSTS_TX_FIFO（FIFO 空）
 *
 * 用来验证交接文档 §4 的首选假设："背靠背连续 blit 时上一块还没出完，
 * 下一块的 CASET/RASET 就把它打断"。注意：**不能**用
 * `DBI_CMD_GET_TX_FIFO_CNT == 0` 来等（那返回的是剩余空间，2026-10-15 就是这么错的）。
 */
void st7802_set_drain_experiment(int on);
int  st7802_get_drain_experiment(void);
/** drain 统计：返回最长自旋次数，*out_blocks = 真的多等了的块数 */
uint32_t st7802_drain_stats(uint32_t *out_blocks);
void st7802_drain_stats_reset(void);

/**
 * @brief 块间"收窗"保护时间（微秒，默认 0 = 关）。shell `winguard <us>`。
 *
 * 针对的假设：真机整分那一帧是两块**背靠背**的大 blit，分界 y=180/181 正好横穿
 * 大数字中部；两块的像素字节与主机预测逐字节相同、排空等待也无差别（drain 实测
 * 0 块），但真实面板上四个数字都在同一高度出现横向错位，且"整屏重绘即恢复"。
 * 唯一剩下的候选是"上一块的像素发完后，面板侧窗口相位还没落定，紧接着的
 * CASET/RASET 被并进上一块的窗口设置"。这不是等 FIFO 空（已否掉），而是等一个
 * 固定量级的落定时间。
 *
 * ⚠️ 这只是**候选修复**，用来在真机上做单变量 A/B；默认 0，对生产路径零影响。
 */
void st7802_set_win_guard_us(uint32_t us);
uint32_t st7802_get_win_guard_us(void);

/**
 * @brief 判断回读的两个字节按给定字节序是否就是 pxpat 在 (px,py) 应写的值。
 *        只比低 11 位（R5/G6）—— 专门用来判字节序。
 * @param big_endian 0 = 低字节先（小端），1 = 高字节先
 * @return 1 = 命中
 */
int st7802_gram_pixel_ok(uint8_t b0, uint8_t b1, int big_endian,
                         uint32_t px, uint32_t py);

/**
 * @brief 把回读字节流当 pxpat 编码解释，扫 0..2 字节整体偏移（dummy 周期量级），
 *        报告命中最多的是哪个偏移。拆出来是为了能在主机上单独 assert。
 */
int st7802_gram_decode_pat(const uint8_t *rb, uint32_t rb_n, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t *out_best_off,
                           int *out_best_hit, int *out_seen);

/** CRC32 自检（内建向量 + 送屏路径成本估算），返回 0 = 全部通过 */
int st7802_crc_selfcheck(void);

/**
 * @brief 编码自证图案：把 x 低位编码进红色、y 低位编码进绿色，蓝色常量。
 *
 *   R5 = x & 0x1F,  G6 = y & 0x3F,  B5 = 0x0A
 *
 * 走**真实** st7802_blit565()，不经 LVGL。按 LVGL 的 band 高度（64 行）
 * 自动分段，所以图案天然横跨 band 边界；分段数打印出来可核对。
 * on=0 时换成固定 0x0000（关掉图案）。
 *
 * @return 实际写下去的像素数
 */
uint32_t st7802_pxpat(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, int on);

/** 上一次 pxpat 被切成几段（= 横跨几个 band 边界），供 shell 打印核对 */
uint32_t st7802_pxpat_blocks(void);
uint32_t st7802_pxpat_pixels(void);

/**
 * @brief 算 CRC32（IEEE 802.3，反射，初值/异或全 1）。用于发送缓冲指纹。
 *        多项式 0xEDB88320，和 zlib.crc32 同族，可用主机脚本独立验证。
 */
uint32_t st7802_crc32(const void *buf, uint32_t len);

/* ------------------------------------------------------------------ */
/* 诊断：发送侧 CRC / 每块窗口（只写 RAM，事后 dump）                    */
/* ------------------------------------------------------------------ */

/** 每块发送缓冲的最新 CRC 与窗口（st7802_blit565 里记录，不打印） */
typedef struct {
    uint16_t x1, y1, x2, y2; /* 窗口 */
    uint32_t npix;           /* 像素数 */
    uint32_t crc;            /* 发送侧数据 CRC32（含边框补的重复半字） */
    uint32_t nsent;          /* 已经推了多少块（编号，用来对齐发送/回读） */
    uint32_t addr;           /* 源缓冲地址 —— 判断是不是 flash/XIP 那一块 */
} st7802_blk_t;

/**
 * @brief 整分取证：把"分钟刚跳变"这一刻前后各若干块发送记录存进 RAM。
 *
 * 调用方（main.c 的 flush 钩子）在检测到"分钟数字刚变"时调用一次。
 * 本函数会把环形缓冲里"该分钟之前的最后几块"与"之后的第一块"分开存，
 * 这样事后可以直接比 **同一个窗口在分钟变化前后发出去的字节是否不同**。
 */
void st7802_min_capture_arm(uint32_t minute);

/** flush 钩子每块都调（arm 之后继续收块，最多 MINCAP_POST 块） */
void st7802_min_capture_take(void);

/** shell 侧请求：armed，等下一次分钟跳变 */
void st7802_min_capture_arm_request(uint32_t minute);

/** 1 = 已 armed 或已抓到（shell 用它决定要不要继续挂钩子） */
int st7802_min_capture_armed(void);

/** 是否已经抓到（1 = 可以 dump） */
int st7802_min_capture_ready(void);

/** dump 抓到的发送侧记录（只 printf，不碰 DBI） */
void st7802_min_capture_dump(void);
void st7802_min_capture_reset(void);

/** 把抓到的第 i 块（0=分钟变化后第一块）的发送数据与面板回读对拍。
 *  只有 st7802_min_capture_ready() 时才有效。i 从 0 开始。
 *  @return 0 成功，<0 出错 */
int st7802_min_capture_panel_check(int i);

/** 读回自检，打印 RDDID/RDDPM/RDDCOLMOD 等 */
void st7802_read_id_report(void);

/** 扫描多种 DBI 配置，打印读回结果矩阵 */
void st7802_read_sweep(void);

/** 只做硬件复位（RST 低 10ms -> 高） */
void st7802_reset(void);

/**
 * 面板电源（VCI_EN = IO29 → BV6810 PMIC）。
 * on=0 彻底断掉面板电源（息屏省电用）；再开需要重新 st7802_init() 才能恢复显示。
 * 2026-10-18：查续航时发现这个脚在 init 之后从来没被关过。
 */
void st7802_vci_power(int on);

/* ------------------------------------------------------------------ */
/* 面板深睡实验（W1，2026-10-28，给用户上板量电流用）                   */
/*                                                                    */
/*   st7802_panel_sleep(level):                                       */
/*     1 = WRDISBV(0x51)=0 + DISPOFF(0x28)                            */
/*     2 = 再加 SLPIN(0x10)，等 120 ms                                 */
/*     3 = 再加 DSTBON(0x4F,0x01) + QSPI 六线改 GPIO 高阻/下拉         */
/*         + VCI_EN(IO29) 低（面板 PMIC 断电）                         */
/*                                                                    */
/*   st7802_panel_wake():                                             */
/*     VCI_EN 高 -> 5 ms -> LCD_RST 低脉冲 10 ms -> 恢复 QSPI 引脚     */
/*     -> SLPOUT(0x11) -> 120 ms -> st7802_init()（完整上电/初始化）    */
/*     注意：唤醒后面板 GRAM 已丢，调用者必须整屏重画。                 */
/*                                                                    */
/*   level 0 = 唤醒。返回 0 成功，<0 失败。                             */
/*   这是实验命令，不改变 board_power.c 的默认息屏路径。                */
/* ------------------------------------------------------------------ */
int st7802_panel_sleep(int level);
int st7802_panel_wake(void);
/** 当前 panelsleep 层级（0 = 正常显示） */
int st7802_panel_sleep_level(void);

/**
 * TE（IO28）同步：面板每帧一个跳变沿。
 * 部分刷新落在扫描途中会撕裂，把一帧的送屏对齐到 V-Blank 即可 ——
 * 注意要**每帧等一次**（LV_EVENT_RENDER_START），不是在每个 flush 里等。
 * @return 0 = 等到跳变沿，-1 = 超时（没接/极性不对，靠它兜底不卡死）
 */
int st7802_te_wait(uint32_t timeout_us);

/** 当前 TE 电平（诊断用，-1 = GPIO 未初始化） */
int st7802_te_level(void);

/** 设置刷屏窗口 */
void st7802_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);

/**
 * @brief 切换像素数据相位（ST7802 支持两种写法，手册 8.4.2 的色彩格式表里
 *        A1D1 和 A1D4 都支持 RGB888）
 * @param mode 0 = A1D4（指令 0x32，地址 1 线，像素 4 线）
 *             1 = A1D1（指令 0x02，地址 1 线，像素 1 线）
 */
void st7802_set_pixel_mode(int mode);

/** 当前像素模式 */
int st7802_get_pixel_mode(void);

/**
 * 纯色填充（走 FIFO 手动填充，不需要大缓冲）
 * @param color 24bit RGB，按 0xRRGGBB 传（内部转 RGB565）
 */
void st7802_fill(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint32_t color);

/**
 * 从缓冲刷图 —— **RGB888 路径（默认，实测能跑通）**
 * buf 是 w*h 个 uint32，每像素 0x00RRGGBB（DBI 侧走 NRGB_8888 输入格式）
 */
void st7802_blit888(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, const uint32_t *buf);

/**
 * 从缓冲刷图 —— **RGB565 路径**
 *
 * buf 是 w*h 个 uint16，bit15..11=R, bit10..5=G, bit4..0=B。
 * DBI 的 FIFO 是 32 位宽，16bpp 输入格式下**两个像素打一个字**
 * （SDK: bflb_dbi_get_words_cnt_form_pixel -> words = (n+1)/2）。
 *
 * ⚠️ 2026-09-26 实测：这条路走不通 —— 面板 RDDCOLMOD 读回来一直是 0x77，
 *    也就是 0x3A=0x55 写不进去，面板还在按 24 位收数据，
 *    结果是花屏 + 只画了大约四分之一屏。切之前先用 st7802_probe_colmod()
 *    确认面板接受哪个 COLMOD 值。
 */
void st7802_blit565(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, const uint16_t *buf);

/** 当前是否 RGB565 模式（0 = RGB888） */
int st7802_get_fmt565(void);

/** 切像素格式并回读校验；返回 0 表示面板确认接受 */
int st7802_set_fmt565(int on);

/** 依次写几个常见 COLMOD 值再读回来，打印面板到底接受哪些（诊断用） */
void st7802_probe_colmod(void);

/**
 * @brief 565 打包时高低半字的顺序（现场 A/B 用）
 *
 * 一个 32 位字装两个像素，谁在高半字。
 *   0 = 先画的像素在高半字：word = (px[0] << 16) | px[1]
 *       ★ 默认值，正确顺序（2026-10-11 晚修正：原来写反了，导致真机上
 *         "细笔画全花、粗笔画正常"。依据是 SDK 里
 *         soln_utils/display/display_mcu/soln_display_flush.c 的
 *         16bpp 逐字节参考实现。）
 *   1 = 反过来，留给换面板/换线序时的现场开关
 */
void st7802_set_px_swap(int swap);
int st7802_get_px_swap(void);

#endif /* __ST7802_H */
