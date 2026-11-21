/*
 * svc_face_panel.c —— 默认表盘下半区面板的存储层（76 §2.1/§2.2，2026-11-08）
 *
 * 契约：V1.6/research/76_表盘精简与下半区面板_2026-11-08.md §2.1。
 * 只碰 flash + CRC，不依赖 LVGL；所有接口都允许在 shell 任务或 LVGL 任务调用。
 *
 * 为什么把面板图放在 media 分区、XIP 指针直接给 LVGL：
 *   480x232 索引图 112 KB，RAM 放不下（而且没必要）；media 区整片被 XIP 映射，
 *   LVGL 的 bin decoder 按行从指针里读，和 face 3（svc_face_store.c）完全同一套路。
 *
 * 为什么和 face 3 分成两个模块而不是"通用槽"：
 *   face 3 的 header 有叠层参数（time_x/time_y/font/flags/color）且 480x480；
 *   面板的几何是固定的（480x232 @ y=248）、header 只有 6 个字段。
 *   合并要为一个固定表付一层参数结构，反而更容易在 header 偏移上出错；
 *   两个槽的 flash 逻辑几乎相同，各自独立更好读、也更好按 76 §2.1 审计。
 *
 * 为什么 CRC 用软件 16 项 nibble 表（和 svc_face_store.c 同一张）：
 *   zlib CRC32（poly 0xEDB88320）没有硬件外设可直接对齐 Python binascii.crc32；
 *   16 项表只占 64 B flash，112 KB 约 1 ms（实测待上板确认）。
 *   这里刻意**没有**去 include svc_face_store 的私有实现，两处各留一份表：
 *   升级 face 3 的 CRC 实现时不该悄悄改变面板的校验口径。
 */
#include <stdio.h>
#include <string.h>

#include "bflb_flash.h"
#include "bflb_l1c.h"

#include "svc_face_panel.h"
/* 只为了下面这两条编译期断言，运行时零依赖（header 里只有宏和类型）。 */
#include "svc_face_store.h"

/* 编进固件时就固定住关键地址：SDK 若改了 flash 布局/擦除粒度，这里直接编译失败，
 * 不要等到上板花屏才发现。 */
#if FACE_PANEL_ERASE_LEN < (FACE_PANEL_HEADER_SIZE + FACE_PANEL_IMAGE_LEN)
#error "FACE_PANEL_ERASE_LEN 盖不住 header + image"
#endif
/* 面板槽必须从 face 3 槽（0x378000..0x3B1FFF）之后开始，否则会覆盖图片表盘。 */
#if FACE_PANEL_FLASH_BASE < (FACE_STORE_FLASH_BASE + FACE_STORE_ERASE_LEN)
#error "面板槽与 face 3 槽重叠"
#endif

/*
 * XIP 地址不是 FLASH_XIP_BASE + flash_offset（2026-11-08 上板踩过，见 research/71 §11.1）。
 * Bouffalo SDK 的映射是（components/fs/romfs/bflb_romfs.c）：
 *
 *     XIP = FLASH_XIP_BASE + flash_offset - bflb_flash_get_image_offset()
 *
 * 因为 XIP 窗口把 **应用镜像起点** 映射到 FLASH_XIP_BASE，而应用镜像在 flash 里
 * 不在 0 地址。image_offset 由 boot2 写进 SF 控制器，运行时才知道（本机 0x11000）。
 */
static uint32_t face_panel_xip_offset(void)
{
    return bflb_flash_get_image_offset();
}

#define FACE_PANEL_HDR_CRC_LEN  20u            /* header_crc32 覆盖前 20 B */
#define FACE_PANEL_CHUNK        256u           /* 回读校验分片 */

/* zlib CRC32 的 16 项 nibble 表（标准值，等价于 256 项表的每 4 bit 一步） */
static const uint32_t s_crc32_tab[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

static int      s_inited;       /* face_panel_init() 至少跑过一次（懒加载标志） */
static int      s_valid;        /* header 有效（未校验 image_crc32） */
static int      s_begun;        /* face_panel_begin() 之后、finish() 之前 */
static int      s_erased;       /* 本次上电后已整片擦过槽（避免 begin 再擦一次） */
static uint32_t s_expect_crc;   /* 本轮安装的期望 image CRC32 */
static uint32_t s_hi;           /* 已写入区间的最高末端（相对 blob） */
static uint32_t s_recv;         /* 累计收到的字节数（仅诊断；重发会偏大） */
static int      s_logged_state = -1;  /* 上次打印过的 valid 值（只在变化时打一行） */
static face_panel_info_t s_info;

static uint8_t s_hdr[256];      /* header 收发共用缓冲（256 B，别放栈上：shell 栈 1 KB） */
static uint8_t s_crc_buf[FACE_PANEL_CHUNK];

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t crc32_feed(uint32_t crc, const uint8_t *p, uint32_t len)
{
    while (len-- > 0) {
        crc ^= *p++;
        crc = (crc >> 4) ^ s_crc32_tab[crc & 0x0Fu];
        crc = (crc >> 4) ^ s_crc32_tab[crc & 0x0Fu];
    }
    return crc;
}

/* 读 flash（走 SBUS，不经过 XIP cache）并算 image blob 的 CRC32 */
static int image_crc_readback(uint32_t *out)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t off = 0;

    while (off < FACE_PANEL_IMAGE_LEN) {
        uint32_t n = FACE_PANEL_IMAGE_LEN - off;
        if (n > FACE_PANEL_CHUNK) {
            n = FACE_PANEL_CHUNK;
        }
        if (bflb_flash_read(FACE_PANEL_IMAGE_FLASH_OFF + off, s_crc_buf, n) != 0) {
            return FACE_PANEL_ERR_FLASH;
        }
        crc = crc32_feed(crc, s_crc_buf, n);
        off += n;
    }
    *out = crc ^ 0xFFFFFFFFu;
    return FACE_PANEL_OK;
}

/* 写完 flash 后让 XIP 读路径看到新数据。BL616 的写/擦 API 走 ROM，
 * 不保证替我们失效 L1C；这里只 invalidate（flash 只读，没有脏行要回写）。 */
static void face_panel_cache_invalidate(void)
{
    bflb_l1c_dcache_invalidate_range((void *)face_panel_image_ptr(),
                                     FACE_PANEL_IMAGE_LEN);
}

/* ------------------------------------------------------------------ */
/* header 解析 / 生成                                                  */
/* ------------------------------------------------------------------ */

static int header_parse(const uint8_t *hdr, face_panel_info_t *info)
{
    uint32_t want, got;

    if (rd_u32(hdr + 0) != FACE_PANEL_MAGIC) {
        return FACE_PANEL_ERR_FORMAT;
    }
    if (hdr[4] != FACE_PANEL_VERSION || hdr[5] != FACE_PANEL_FMT_I8) {
        return FACE_PANEL_ERR_FORMAT;
    }
    if (rd_u16(hdr + 6) != FACE_PANEL_W || rd_u16(hdr + 8) != FACE_PANEL_H ||
        rd_u16(hdr + 10) != FACE_PANEL_Y ||
        rd_u32(hdr + 12) != FACE_PANEL_IMAGE_LEN) {
        return FACE_PANEL_ERR_FORMAT;
    }
    /* 先验 header CRC，再信任里面任何字段（CRC 覆盖 0..19，字段 0..19 全在范围内） */
    want = rd_u32(hdr + 20);
    got = crc32_feed(0xFFFFFFFFu, hdr, FACE_PANEL_HDR_CRC_LEN) ^ 0xFFFFFFFFu;
    if (want != got) {
        return FACE_PANEL_ERR_FORMAT;
    }

    if (info != NULL) {
        memset(info, 0, sizeof(*info));
        info->width = rd_u16(hdr + 6);
        info->height = rd_u16(hdr + 8);
        info->y = rd_u16(hdr + 10);
        info->image_len = rd_u32(hdr + 12);
        info->image_crc32 = rd_u32(hdr + 16);
    }
    return FACE_PANEL_OK;
}

static void header_build(uint32_t image_crc)
{
    memset(s_hdr, 0xFF, sizeof(s_hdr));

    wr_u32(s_hdr + 0, FACE_PANEL_MAGIC);
    s_hdr[4] = FACE_PANEL_VERSION;
    s_hdr[5] = FACE_PANEL_FMT_I8;
    wr_u16(s_hdr + 6, FACE_PANEL_W);
    wr_u16(s_hdr + 8, FACE_PANEL_H);
    wr_u16(s_hdr + 10, FACE_PANEL_Y);
    wr_u32(s_hdr + 12, FACE_PANEL_IMAGE_LEN);
    wr_u32(s_hdr + 16, image_crc);
    /* header_crc32 覆盖 0..19，自己（20..23）不在覆盖范围内；先置 0 再算，
     * 保证同一份内容每次算出的值一样，也和 PC 侧 build_header 的做法对得上。 */
    wr_u32(s_hdr + 20, 0);
    wr_u32(s_hdr + 20, crc32_feed(0xFFFFFFFFu, s_hdr, FACE_PANEL_HDR_CRC_LEN) ^
                       0xFFFFFFFFu);
}

/* 只在 valid 变化时打一行：face 0 每次重建都会问 face_panel_available()，
 * 无条件 printf 会把串口刷满（face 3 的 face_store_init 也有同样的处理）。 */
static void log_state(void)
{
    if (s_logged_state == s_valid) {
        return;
    }
    s_logged_state = s_valid;
    if (s_valid) {
        printf("[face] panel: installed %ux%u y=%u crc=%08lX xip=%p (img_off=0x%lX)\r\n",
               (unsigned)s_info.width, (unsigned)s_info.height, (unsigned)s_info.y,
               (unsigned long)s_info.image_crc32,
               (const void *)face_panel_image_ptr(),
               (unsigned long)face_panel_xip_offset());
    } else {
        printf("[face] panel: none -> 内置 watch photo\r\n");
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

int face_panel_init(void)
{
    face_panel_info_t info;
    int rc;

    if (bflb_flash_read(FACE_PANEL_FLASH_BASE, s_hdr, sizeof(s_hdr)) != 0) {
        /* 读失败不置 s_inited：下次 available() 还会再试一次，
         * 不会因为一次短暂的 SBUS 失败就"整个上电周期都没有面板"。 */
        memset(&s_info, 0, sizeof(s_info));
        s_valid = 0;
        log_state();
        return FACE_PANEL_ERR_FLASH;
    }
    s_inited = 1;
    rc = header_parse(s_hdr, &info);
    if (rc != FACE_PANEL_OK) {
        memset(&s_info, 0, sizeof(s_info));
        s_valid = 0;
        log_state();
        return rc;
    }
    s_info = info;
    s_valid = 1;
    log_state();
    return FACE_PANEL_OK;
}

int face_panel_available(void)
{
    if (!s_inited) {
        (void)face_panel_init();
    }
    return s_valid;
}

int face_panel_info(face_panel_info_t *out, int with_crc)
{
    int rc;

    if (!s_inited) {
        rc = face_panel_init();
        if (rc != FACE_PANEL_OK) {
            return rc;
        }
    }
    if (!s_valid) {
        return FACE_PANEL_ERR_FORMAT;
    }
    if (with_crc) {
        uint32_t crc = 0;
        rc = image_crc_readback(&crc);
        if (rc != FACE_PANEL_OK) {
            return rc;
        }
        if (crc != s_info.image_crc32) {
            return FACE_PANEL_ERR_CRC;
        }
    }
    if (out != NULL) {
        *out = s_info;
    }
    return FACE_PANEL_OK;
}

const uint8_t *face_panel_image_ptr(void)
{
    /* 见文件头注释：XIP = FLASH_XIP_BASE + flash_offset - image_offset */
    return (const uint8_t *)(uintptr_t)(FLASH_XIP_BASE + FACE_PANEL_IMAGE_FLASH_OFF -
                                        face_panel_xip_offset());
}

int face_panel_erase(void)
{
    int rc;

    rc = bflb_flash_erase(FACE_PANEL_FLASH_BASE, FACE_PANEL_ERASE_LEN);
    if (rc != 0) {
        printf("[face] panel erase 0x%lX+0x%lX failed rc=%d\r\n",
               (unsigned long)FACE_PANEL_FLASH_BASE,
               (unsigned long)FACE_PANEL_ERASE_LEN, rc);
        return FACE_PANEL_ERR_FLASH;
    }
    face_panel_cache_invalidate();
    memset(&s_info, 0, sizeof(s_info));
    s_valid = 0;
    s_begun = 0;
    s_erased = 1;       /* 整片都干净了：下一次 begin 不用再擦 */
    log_state();
    return FACE_PANEL_OK;
}

int face_panel_clear(void)
{
    int rc;

    /* 只擦 header 扇区：面板立刻变"未安装"，face 0 回内置 watch photo。
     * 不擦 image 区（省 28 个扇区的擦除时间），但也不能算"已擦干净"，
     * 所以 s_erased 必须清 0 —— 否则下次 begin 会在旧数据上叠加写（NOR 只能 1->0）。 */
    rc = bflb_flash_erase(FACE_PANEL_FLASH_BASE, FACE_PANEL_HEADER_SIZE);
    if (rc != 0) {
        printf("[face] panel clear header failed rc=%d\r\n", rc);
        return FACE_PANEL_ERR_FLASH;
    }
    memset(&s_info, 0, sizeof(s_info));
    s_valid = 0;
    s_begun = 0;
    s_erased = 0;
    log_state();
    return FACE_PANEL_OK;
}

int face_panel_begin(uint32_t expect_len, uint32_t expect_crc)
{
    int rc;

    if (expect_len != FACE_PANEL_IMAGE_LEN) {
        return FACE_PANEL_ERR_ARGS;
    }
    if (!s_erased) {
        /* 直接在旧数据上叠加写会得到"旧值 AND 新值"的乱码；没整片擦过就先擦。 */
        rc = face_panel_erase();
        if (rc != FACE_PANEL_OK) {
            return rc;
        }
    }
    s_expect_crc = expect_crc;
    s_hi = 0;
    s_recv = 0;
    s_begun = 1;
    return FACE_PANEL_OK;
}

int face_panel_write(uint32_t off, const uint8_t *data, uint32_t len)
{
    int rc;

    if (!s_begun) {
        return FACE_PANEL_ERR_STATE;
    }
    if (data == NULL || len == 0 || len > 48u) {
        return FACE_PANEL_ERR_ARGS;
    }
    /* bflb_flash_write 按 4 B 粒度编程；shell 工具按 48 B 分片正好对齐 */
    if ((off & 0x03u) != 0 || (len & 0x03u) != 0) {
        return FACE_PANEL_ERR_ARGS;
    }
    if (off > FACE_PANEL_IMAGE_LEN || len > FACE_PANEL_IMAGE_LEN - off) {
        return FACE_PANEL_ERR_ARGS;
    }
    rc = bflb_flash_write(FACE_PANEL_IMAGE_FLASH_OFF + off, (uint8_t *)data, len);
    if (rc != 0) {
        printf("[face] panel write off=%lu len=%lu failed rc=%d\r\n",
               (unsigned long)off, (unsigned long)len, rc);
        return FACE_PANEL_ERR_FLASH;
    }
    if (off + len > s_hi) {
        s_hi = off + len;
    }
    s_recv += len;
    /* 只要写过 image 区，这个槽就不再是"干净已擦"状态：失败重来时 begin 会补擦。 */
    s_erased = 0;
    return FACE_PANEL_OK;
}

int face_panel_finish(void)
{
    face_panel_info_t info;
    uint32_t crc = 0;
    int rc;

    if (!s_begun) {
        return FACE_PANEL_ERR_STATE;
    }
    if (s_hi != FACE_PANEL_IMAGE_LEN) {
        printf("[facepanel] finish: 未收齐 hi=%lu/%lu\r\n",
               (unsigned long)s_hi, (unsigned long)FACE_PANEL_IMAGE_LEN);
        return FACE_PANEL_ERR_STATE;
    }

    /* 关键：坏 CRC 直接拒装，绝不写 header（旧面板最多变成"未安装"，
     * 不会变成一张花屏面板）。和 faceimg 的行为完全一致。 */
    face_panel_cache_invalidate();
    rc = image_crc_readback(&crc);
    if (rc != FACE_PANEL_OK) {
        return rc;
    }
    if (crc != s_expect_crc) {
        s_begun = 0;
        printf("[facepanel] finish: CRC mismatch got=%08lX expect=%08lX -> 拒装\r\n",
               (unsigned long)crc, (unsigned long)s_expect_crc);
        return FACE_PANEL_ERR_CRC;
    }

    header_build(crc);

    rc = bflb_flash_erase(FACE_PANEL_FLASH_BASE, FACE_PANEL_HEADER_SIZE);
    if (rc != 0) {
        printf("[facepanel] header erase failed rc=%d\r\n", rc);
        return FACE_PANEL_ERR_FLASH;
    }
    rc = bflb_flash_write(FACE_PANEL_FLASH_BASE, s_hdr, 256);
    if (rc != 0) {
        printf("[facepanel] header write failed rc=%d\r\n", rc);
        return FACE_PANEL_ERR_FLASH;
    }

    /* 回读比对：写没写进去以 flash 为准，不看返回值 */
    memset(s_crc_buf, 0, FACE_PANEL_CHUNK);
    if (bflb_flash_read(FACE_PANEL_FLASH_BASE, s_crc_buf, 256) != 0 ||
        memcmp(s_crc_buf, s_hdr, 256) != 0) {
        printf("[facepanel] header verify failed\r\n");
        return FACE_PANEL_ERR_FLASH;
    }
    if (header_parse(s_crc_buf, &info) != FACE_PANEL_OK) {
        return FACE_PANEL_ERR_FORMAT;
    }

    s_info = info;
    s_valid = 1;
    s_begun = 0;
    s_erased = 0;       /* 下次 begin 前必须重新整片擦 */
    log_state();
    printf("[facepanel] install ok crc=%08lX recv=%lu B\r\n",
           (unsigned long)info.image_crc32, (unsigned long)s_recv);
    return FACE_PANEL_OK;
}
