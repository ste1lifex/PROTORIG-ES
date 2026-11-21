/*
 * st7802_verify.c —— 主机侧"探针自证"
 *
 * 为什么需要它（docs/minute_digit_artifact_20261015.md 第 11.1 节第 3 条）：
 *   上一轮 `s_push_tc` 忘了每块清零，探针本身骗了我们一整轮。
 *   所以本轮所有新增探针 —— CRC32、pxpat 编码、gramrd 的 dummy 偏移分析、
 *   整分取证状态机 —— 都在主机上用**同一份源码** assert 一遍。
 *
 * 做法：-DST7802_HOST_VERIFY 直接 include lvgl_test/st7802.c。
 *   寄存器/面板那几处换成内存模型（见 st7802.c 末尾的 host 段），
 *   其余逻辑一字不改。
 *
 * 编译运行见 lvgl_test/tools/host_verify_st7802.ps1
 */
#ifndef ST7802_HOST_VERIFY
#define ST7802_HOST_VERIFY      /* 直接 include st7802.c 时由源码内定义，这里兜底 */
#endif
#include "st7802.c"

#include <assert.h>
#include <string.h>

static int fails;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("  !! FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            fails++;                                                      \
        }                                                                 \
    } while (0)

/* st7802_pxpat 的分段发射改成写模拟 GRAM 之后，"一行"由 st7802_pxpat_line()
 * 提供；这里直接调 st7802_pxpat()，它会通过 pxpat_emit -> st7802_sim_store
 * 真正落进模拟 GRAM。 */

static void test_crc(void)
{
    static const uint8_t v1[] = "123456789";
    printf("[crc32] 内建向量\n");
    CHECK(st7802_crc32(v1, 9) == 0xCBF43926u);
    CHECK(st7802_crc32("a", 1) == 0xE8B7BE43u);
    CHECK(st7802_crc32("a", 0) == 0x00000000u);

    /* 单比特翻转必须改 CRC（探针要能发现"数据不一样"） */
    {
        uint8_t b[64];
        uint32_t c0;
        for (int i = 0; i < 64; i++) b[i] = (uint8_t)(i * 11 + 5);
        c0 = st7802_crc32(b, 64);
        for (int bit = 0; bit < 64; bit++) {
            b[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            CHECK(st7802_crc32(b, 64) != c0);
            b[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        }
        printf("[crc32] 64 个单比特翻转全部被检出\n");
    }
    CHECK(st7802_crc_selfcheck() == 0);
}

/* pxpat 编码：R5=x&31, G6=y&63, B5=0x0A（低 11 位） */
static void test_pxpat_encode(void)
{
    uint32_t px;

    printf("[pxpat] 编码 + 跨 band 边界分段\n");
    /*
     * 写一个横跨 y=64 边界的区域（y1=40..y2=100）。
     * 实现是**逐行**一次真实 blit565（每行的绿色通道编的是自己的 y），
     * 所以段数 = 行数 = 61；关键不是段数，而是 y=64 这个 band 边界
     * 被直接编进了图案里（见下面 y=64/65 的取值检查）。
     */
    px = st7802_pxpat(100, 40, 199, 100, 1);
    CHECK(px == 100u * 61u);
    CHECK(st7802_pxpat_blocks() == 61);
    CHECK(st7802_pxpat_pixels() == 100u * 61u);

    for (uint32_t y = 40; y <= 100; y++) {
        for (uint32_t x = 100; x <= 199; x++) {
            uint16_t want = (uint16_t)(((x & 0x1Fu) << 11) | ((y & 0x3Fu) << 5) | 0x0Au);
            CHECK(st7802_sim_load(x, y) == want);
        }
    }
    /* 每 32 列红通道回卷：x=100 与 x=132 必须同值 */
    CHECK(st7802_sim_load(100, 50) == st7802_sim_load(132, 50));
    /* band 边界两侧（y=63 与 y=64）G6 必须不同 —— 边界是被编码出来的 */
    CHECK((st7802_sim_load(150, 63) & 0x07E0) != (st7802_sim_load(150, 64) & 0x07E0));
    /* 蓝色常量在位 */
    CHECK((st7802_sim_load(150, 70) & 0x1F) == 0x0A);

    /* off 必须真的抹掉（写 0x0000） */
    px = st7802_pxpat(100, 40, 199, 100, 0);
    CHECK(px == 100u * 61u);
    CHECK(st7802_sim_load(150, 70) == 0x0000);

    /* 边界检查 */
    CHECK(st7802_pxpat(0, 0, 480, 10, 1) == 0);   /* x2 越界 */
    CHECK(st7802_pxpat(200, 100, 100, 40, 1) == 0); /* 反序 */
}

/*
 * gramrd 的 dummy 偏移分析：核心是"回读字节整体移了 N 字节也要能认出来"。
 * 这里用模拟 GRAM 造三种情况：
 *   ① 指针正确      -> best_off 应为 0
 *   ② 指针晚 1 个像素（模拟多了 2 字节 dummy）-> best_off 应为 2
 *   ③ 面板不支持（回读恒 0xFF）-> 命中应为 0，且必须报"全 FF"
 */
static void test_gram_decode(void)
{
    uint32_t best_off;
    int best_hit, seen;
    static uint8_t rb[16];
    const uint32_t X = 100, Y = 40, W = 8;

    printf("[gramrd] dummy 偏移分析\n");

    /* 造图案（整屏，保证任意窗口都在图案里） */
    st7802_pxpat(0, 0, 479, 479, 1);

    /* 字节序必须能判出来：回读 (120,104) 的 0xC50A 时，
     * 小端解释 [0A C5] 命中、大端解释 [C5 0A] 不命中。 */
    {
        uint16_t v = st7802_sim_load(120, 104);
        uint8_t b0 = (uint8_t)(v & 0xFF), b1 = (uint8_t)(v >> 8);
        CHECK(st7802_gram_pixel_ok(b0, b1, 0, 120, 104) == 1); /* 小端 */
        CHECK(st7802_gram_pixel_ok(b0, b1, 1, 120, 104) == 0); /* 大端 */
    }

    /* ① 起点正确（小端字节流） */
    for (uint32_t i = 0; i < 16; i += 2) {
        uint32_t k = i / 2;
        uint16_t v = st7802_sim_load(X + k, Y);
        rb[i] = (uint8_t)(v & 0xFF);
        rb[i + 1] = (uint8_t)(v >> 8);
    }
    /* 先自证：这条字节流按小端逐像素解出来的就是 (100..107, 40) */
    for (uint32_t k = 0; k < 8; k++) {
        CHECK(st7802_gram_pixel_ok(rb[2 * k], rb[2 * k + 1], 0, X + k, Y) == 1);
    }
    st7802_gram_decode_pat(rb, 16, X, Y, W, &best_off, &best_hit, &seen);
    printf("  -> 正确对齐时 best_off=%lu best_hit=%d\n",
           (unsigned long)best_off, best_hit);
    CHECK(best_off == 0 && best_hit == 8);

    /*
     * ② 已知局限（必须有测试守着，否则将来会有人以为它什么都能测）：
     *    R5 = x & 31，所以在**同一行**里 x 从 100 连续走 8 个像素时
     *    R5 恒为 4 —— 横向错位 1..31 像素在单行内**原理上测不出来**
     *    （p(x,y) == p(x+32,y)，而且 p(x,y) 的 R5 在本行内不变）。
     *    能可靠测出的是：
     *      · 纵向错位（G6 直接变）      —— 见 ②b
     *      · 横向错 32 的整数倍          —— 见 ②c
     *    更细的横向错位要靠蓝色通道另编一维，本轮**没做**（见文档的局限一节）。
     */
    {
        uint32_t hits = 0;
        for (uint32_t d = 1; d < 8; d++) {
            uint16_t v = st7802_sim_load(X + d, Y);
            hits += st7802_gram_pixel_ok((uint8_t)(v & 0xFF), (uint8_t)(v >> 8), 0, X, Y);
        }
        printf("  -> 横向 1..7 像素错位可测出的个数 = %lu（设计如此，应为 7=全部\"测不出\"）\n",
               (unsigned long)hits);
        CHECK(hits == 7);   /* 全部误命中 = 横向细错位测不出，是已知局限 */
    }

    /* ②b 行错位（y 差 1）必须被测出来：G6 直接变了 */
    {
        static uint8_t rb4[16];
        for (uint32_t i = 0; i < 16; i += 2) {
            uint32_t k = i / 2;
            uint16_t v = st7802_sim_load(X + k, Y + 1);
            rb4[i] = (uint8_t)(v & 0xFF);
            rb4[i + 1] = (uint8_t)(v >> 8);
        }
        CHECK(st7802_gram_pixel_ok(rb4[0], rb4[1], 0, X, Y) == 0);
        st7802_gram_decode_pat(rb4, 16, X, Y, W, &best_off, &best_hit, &seen);
        printf("  -> 纵向错 1 行时 best_hit=%d（应为 0）\n", best_hit);
        CHECK(best_hit == 0);
    }

    /* ②c 横向错 32 像素（= 64 字节）：仍然测不出来 ——
     *    x 与 x+32 编出来的是同一个值（R5 周期 32），整个图案在
     *    "横向平移 32 的倍数"下是不变的。这条必须写死在测试里，
     *    免得将来有人以为 pxpat 能测横向错位。 */
    {
        static uint8_t rb3[16];
        for (uint32_t i = 0; i < 16; i += 2) {
            uint32_t k = i / 2 + 32;
            uint16_t v = st7802_sim_load(X + k, Y);
            rb3[i] = (uint8_t)(v & 0xFF);
            rb3[i + 1] = (uint8_t)(v >> 8);
        }
        CHECK(st7802_gram_pixel_ok(rb3[0], rb3[1], 0, X, Y) == 1); /* 误命中 = 局限 */
        st7802_gram_decode_pat(rb3, 16, X, Y, W, &best_off, &best_hit, &seen);
        printf("  -> 横向错 32 像素时 best_hit=%d（同样误命中；横向错位测不出的已确认）\n",
               best_hit);
        CHECK(best_hit == 0 || best_hit == 8);
    }

    /* ③ 全 FF（面板不支持回读）-> 没有任何偏移能命中 */
    memset(rb, 0xFF, sizeof(rb));
    st7802_gram_decode_pat(rb, 16, X, Y, W, &best_off, &best_hit, &seen);
    printf("  -> 全 FF 时 best_hit=%d（应为 0）\n", best_hit);
    CHECK(best_hit == 0);

    /* ④ 走完整 gramrd 路径（含窗口设置、读回、打印） */
    printf("[gramrd] 完整路径（模拟面板支持 RAMRD）\n");
    st7802_sim_const(0, 0);
    st7802_sim_pointer_reset(0);
    CHECK(st7802_gramrd(100, 40, 4, 2, 0x2E, 8, 1) == 16);

    /* ⑤ 模拟"面板不支持 2Eh"：回读恒 0xFF，判定必须是"全 FF" */
    printf("[gramrd] 完整路径（模拟不支持 2Eh）\n");
    st7802_sim_const(1, 0xFF);
    CHECK(st7802_gramrd(100, 40, 4, 2, 0x2E, 8, 0) == 16);
    st7802_sim_const(0, 0);

    /* ⑥ 模拟"读指针每个事务复位"：整段回读必然出现重复段。
     *    真机上遇到这个现象就说明"不能靠多次 8 字节事务拼整窗口"。 */
    printf("[gramrd] 模拟读指针每 8 字节事务复位\n");
    st7802_sim_pointer_reset(1);
    {
        static uint8_t rb2[32];
        uint32_t got = st7802_hw_read(0x2E, rb2, 32, 8);
        CHECK(got == 32);
        CHECK(memcmp(rb2, rb2 + 8, 8) == 0);   /* 第二块和第一块一模一样 */
        CHECK(memcmp(rb2, rb2 + 16, 8) == 0);
        printf("  -> 事务间复位时，第 2/3/4 块与第 1 块完全相同（真机看到这个=不能整窗口回读）\n");
    }
    st7802_sim_pointer_reset(0);
}

/*
 * 整分取证状态机：这条最容易"看起来对"，所以全部状态都 assert。
 * 序列：blit 0..6 -> armed -> 分钟变化时 arm() -> 同帧再 take() 两次 -> dump。
 */
static void test_min_capture(void)
{
    static uint16_t line[8];
    uint32_t i;

    printf("[mincap] 整分取证状态机\n");

    st7802_flush_diag_reset();
    st7802_min_capture_reset();
    CHECK(st7802_min_capture_ready() == 0);
    CHECK(st7802_min_capture_armed() == 0);

    /* 没有 armed 时 arm() 必须什么都不做（不能误抓） */
    st7802_min_capture_arm(2200);
    CHECK(st7802_min_capture_ready() == 0);

    for (i = 0; i < 8; i++) {
        for (uint32_t k = 0; k < 8; k++) line[k] = (uint16_t)(i * 16 + k);
        st7802_blit565(0, (uint16_t)i, 7, (uint16_t)i, line);
    }
    CHECK(st7802_blit_count() == 8);
    CHECK(st7802_min_capture_ready() == 0);

    st7802_min_capture_arm_request(0);
    CHECK(st7802_min_capture_armed() == 1);
    CHECK(st7802_min_capture_ready() == 0);

    /* 分钟变化那一刻：同一帧再发一块（模拟整分帧的第一块），然后 arm() */
    for (uint32_t k = 0; k < 8; k++) line[k] = 0x1234;
    st7802_blit565(0, 100, 7, 100, line);
    st7802_min_capture_arm(2200);
    CHECK(st7802_min_capture_ready() == 1);
    CHECK(s_min_pre_n == 6);          /* 之前留 6 块 */
    CHECK(s_min_post_n == 1);         /* 整分帧第一块 */

    /* 同一帧剩下的带继续收（每块内容都不同，CRC 才该不同） */
    for (uint32_t k = 0; k < 8; k++) line[k] = 0x5678;
    st7802_blit565(0, 101, 7, 101, line);
    st7802_min_capture_take();
    for (uint32_t k = 0; k < 8; k++) line[k] = 0x9ABC;
    st7802_blit565(0, 102, 7, 102, line);
    st7802_min_capture_take();
    CHECK(s_min_post_n == 3);         /* post[0] 被覆盖 + 2 个新块 */

    /* pre 的最后一块和 post[0] 必须是**同一块**（同一个 #nsent），
     * 因为 post[0] 就是"触发 arm 的那一块"；判据是拿它跟更早的同窗口块比。 */
    CHECK(s_min_pre[5].nsent == s_min_post[0].nsent);
    CHECK(s_min_pre[5].crc == s_min_post[0].crc);
    printf("  -> post[0] 就是触发 arm 的那一块 #%lu crc=%08lX\n",
           (unsigned long)s_min_post[0].nsent, (unsigned long)s_min_post[0].crc);

    /* 真正的判据：同一窗口、内容不同的两块，CRC 必须不同 */
    CHECK(s_min_post[1].crc != s_min_post[0].crc);
    CHECK(s_min_post[2].crc != s_min_post[1].crc);
    /* 同一窗口，跨帧：pre 里最后那个"同窗口"块 vs post[1] */
    CHECK(s_min_pre[5].x1 == s_min_post[1].x1);
    printf("  -> 同窗口跨块：post[0]=%08lX post[1]=%08lX pre[5]=%08lX\n",
           (unsigned long)s_min_post[0].crc, (unsigned long)s_min_post[1].crc,
           (unsigned long)s_min_pre[5].crc);

    /* 同样内容的块，CRC 必须一样（否则指纹本身不可信） */
    st7802_blit565(0, 100, 7, 100, line);
    {
        uint32_t c1 = s_blit_log[(s_blit_log_i - 1) % BLIT_LOG_N].crc;
        st7802_blit565(0, 100, 7, 100, line);
        {
            uint32_t c2 = s_blit_log[(s_blit_log_i - 1) % BLIT_LOG_N].crc;
            CHECK(c1 == c2);
        }
    }

    /* 超过 MINCAP_POST 之后不能再写（防越界） */
    for (i = 0; i < 30; i++) {
        st7802_min_capture_take();
    }
    CHECK(s_min_post_n <= 12);

    st7802_min_capture_dump();
    CHECK(st7802_min_capture_panel_check(0) == 0);
    CHECK(st7802_min_capture_panel_check(999) == -1);

    st7802_min_capture_reset();
    CHECK(st7802_min_capture_ready() == 0);
    CHECK(st7802_min_capture_armed() == 0);
}

/* 每块的诊断计数必须每块清零（第 11.1 节栽的就是这里） */
static void test_per_block_reset(void)
{
    static uint16_t line[8];

    printf("[flushdiag] 每块计数字段必须独立\n");
    st7802_flush_diag_reset();
    for (uint32_t k = 0; k < 8; k++) line[k] = (uint16_t)k;
    for (uint32_t i = 0; i < 4; i++) {
        st7802_blit565(0, (uint16_t)i, 7, (uint16_t)i, line);
    }
    for (uint32_t i = 0; i < 4; i++) {
        blit_log_t *e = &s_blit_log[(s_blit_log_i - 4 + i) % BLIT_LOG_N];
        CHECK(e->npix == 8);
        /* 主机桩里这些都是 0；关键是"没有累计" */
        CHECK(e->iters == 0 && e->zero == 0 && e->tc_spin == 0);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    test_crc();
    test_pxpat_encode();
    test_gram_decode();
    test_min_capture();
    test_per_block_reset();

    if (fails) {
        printf("\nST7802 VERIFY FAILED: %d 项\n", fails);
        return 1;
    }
    printf("\nST7802 VERIFY PASS: crc32, pxpat encode/segments, gram dummy-offset, mincap state, per-block reset\n");
    return 0;
}
