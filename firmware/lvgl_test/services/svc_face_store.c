/*
 * svc_face_store.c —— 图片表盘存储层（B：表盘可替换，2026-11-08）
 *
 * 契约：V1.6/research/70_表盘可替换_实现方案.md §2/§3.1。
 * 只碰 flash + CRC，不依赖 LVGL；所有接口都允许在 shell 任务或 LVGL 任务调用。
 *
 * 为什么把 image blob 放在 media 分区、XIP 指针直接给 LVGL：
 *   480x480 索引图 230 KB，RAM 放不下；media 区整片被 XIP 映射，
 *   LVGL 的 bin decoder 按行从指针里读（每行解码成 ARGB8888 再混到 RGB565），
 *   和 apps/app_notify.c 的 I1/I2 位图通知是同一套路。
 *
 * 为什么 CRC 用软件 16 项 nibble 表：
 *   zlib CRC32（poly 0xEDB88320）没有硬件外设可直接对齐 Python binascii.crc32；
 *   16 项 nibble 表只占 64 B flash，231 KB 约 2~3 ms（实测待上板确认）。
 */
#include <stdio.h>
#include <string.h>

#include "bflb_flash.h"
#include "bflb_l1c.h"

#include "svc_face_store.h"

/* 编进固件时就固定住两个关键地址：SDK 若改了 XIP 基址，这里直接编译失败，
 * 不要等到上板花屏才发现。 */
#if FACE_STORE_ERASE_LEN < (FACE_STORE_HEADER_SIZE + FACE_IMAGE_MAX)
#error "FACE_STORE_ERASE_LEN 盖不住 header + image"
#endif

/*
 * 2026-11-08 上板发现的大坑：XIP 地址不是 FLASH_XIP_BASE + flash_offset。
 * Bouffalo SDK 的映射是（见 components/fs/romfs/bflb_romfs.c:92）：
 *
 *     XIP = FLASH_XIP_BASE + flash_offset - bflb_flash_get_image_offset()
 *
 * 因为 XIP 窗口把 **应用镜像起点** 映射到 FLASH_XIP_BASE，而应用镜像在
 * flash 里不在 0 地址。image_offset 由 boot2 写进 SF 控制器，运行时才知道
 * （本机是 0x10000 量级），所以不能像原实现那样写死 0xA0379000。
 * 写死的后果：读到了 media 分区 +0x10000 的位置 → 花屏/发白/错位。
 */
static uint32_t face_store_xip_offset(void)
{
    return bflb_flash_get_image_offset();
}

#define FACE_STORE_HDR_LEN  32u            /* header_crc32 覆盖前 32 B */
#define FACE_STORE_CHUNK    256u           /* 回读校验分片 */

/* zlib CRC32 的 16 项 nibble 表（标准值，等价于 256 项表的每 4 bit 一步） */
static const uint32_t s_crc32_tab[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

static int      s_valid;        /* header 有效（未校验 image_crc32） */
static int      s_begun;        /* face_store_begin() 之后、finish() 之前 */
static int      s_erased;       /* 本次上电后已擦过槽（避免 begin 再擦一次） */
static uint32_t s_expect_crc;   /* 本轮安装的期望 image CRC32 */
static uint32_t s_hi;           /* 已写入区间的最高末端（相对 blob） */
static uint32_t s_recv;         /* 累计收到的字节数（仅诊断；重发会偏大） */
static face_store_info_t s_info;

static uint8_t s_hdr[256];      /* header 收发共用缓冲（256 B，别放栈上：shell 栈 1 KB） */
static uint8_t s_crc_buf[FACE_STORE_CHUNK];

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int16_t rd_i16(const uint8_t *p)
{
    return (int16_t)rd_u16(p);
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

    while (off < FACE_IMAGE_MAX) {
        uint32_t n = FACE_IMAGE_MAX - off;
        if (n > FACE_STORE_CHUNK) {
            n = FACE_STORE_CHUNK;
        }
        if (bflb_flash_read(FACE_STORE_FLASH_BASE + FACE_STORE_IMAGE_OFF + off,
                            s_crc_buf, n) != 0) {
            return FACE_STORE_ERR_FLASH;
        }
        crc = crc32_feed(crc, s_crc_buf, n);
        off += n;
    }
    *out = crc ^ 0xFFFFFFFFu;
    return FACE_STORE_OK;
}

/* 写完 flash 后让 XIP 读路径看到新数据。BL616 的写/擦 API 走 ROM，
 * 不保证替我们失效 L1C；这里只 invalidate（flash 只读，没有脏行要回写）。 */
static void face_store_cache_invalidate(void)
{
    bflb_l1c_dcache_invalidate_range((void *)face_store_image_ptr(),
                                     FACE_IMAGE_MAX);
}

/* ------------------------------------------------------------------ */
/* header 解析 / 生成                                                  */
/* ------------------------------------------------------------------ */

static int header_parse(const uint8_t *hdr, face_store_info_t *info)
{
    uint32_t want, got;

    if (rd_u32(hdr + 0) != FACE_STORE_MAGIC) {
        return FACE_STORE_ERR_FORMAT;
    }
    if (hdr[4] != FACE_STORE_VERSION || hdr[5] != FACE_STORE_FMT_I8) {
        return FACE_STORE_ERR_FORMAT;
    }
    if (rd_u16(hdr + 6) != FACE_IMAGE_W || rd_u16(hdr + 8) != FACE_IMAGE_H ||
        rd_u16(hdr + 10) != FACE_IMAGE_PALETTE_N ||
        rd_u32(hdr + 22) != FACE_IMAGE_MAX) {
        return FACE_STORE_ERR_FORMAT;
    }
    /* 先验 header CRC，再信任里面任何字段 */
    want = rd_u32(hdr + 32);
    got = crc32_feed(0xFFFFFFFFu, hdr, FACE_STORE_HDR_LEN) ^ 0xFFFFFFFFu;
    if (want != got) {
        return FACE_STORE_ERR_FORMAT;
    }
    if (hdr[16] > 3u || (hdr[17] & (uint8_t)~0x03u) != 0u) {
        return FACE_STORE_ERR_FORMAT;
    }

    if (info != NULL) {
        memset(info, 0, sizeof(*info));
        info->width = rd_u16(hdr + 6);
        info->height = rd_u16(hdr + 8);
        info->time_x = rd_i16(hdr + 12);
        info->time_y = rd_i16(hdr + 14);
        info->font_id = hdr[16];
        info->flags = hdr[17];
        info->time_color = rd_u32(hdr + 18);
        info->image_len = rd_u32(hdr + 22);
        info->image_crc32 = rd_u32(hdr + 26);
    }
    return FACE_STORE_OK;
}

static void header_build(const face_store_info_t *layout, uint32_t image_crc)
{
    memset(s_hdr, 0xFF, sizeof(s_hdr));

    wr_u32(s_hdr + 0, FACE_STORE_MAGIC);
    s_hdr[4] = FACE_STORE_VERSION;
    s_hdr[5] = FACE_STORE_FMT_I8;
    wr_u16(s_hdr + 6, FACE_IMAGE_W);
    wr_u16(s_hdr + 8, FACE_IMAGE_H);
    wr_u16(s_hdr + 10, FACE_IMAGE_PALETTE_N);
    wr_u16(s_hdr + 12, (uint16_t)layout->time_x);
    wr_u16(s_hdr + 14, (uint16_t)layout->time_y);
    s_hdr[16] = layout->font_id;
    s_hdr[17] = (uint8_t)(layout->flags & 0x03u);
    wr_u32(s_hdr + 18, layout->time_color & 0x00FFFFFFu);
    wr_u32(s_hdr + 22, FACE_IMAGE_MAX);
    wr_u32(s_hdr + 26, image_crc);
    s_hdr[30] = 0;
    s_hdr[31] = 0;
    wr_u32(s_hdr + 32, crc32_feed(0xFFFFFFFFu, s_hdr, FACE_STORE_HDR_LEN) ^ 0xFFFFFFFFu);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

int face_store_init(void)
{
    face_store_info_t info;
    int rc;
    static int s_logged_invalid;   /* 只在第一次失败时打一行，避免 face 切换刷屏 */

    s_valid = 0;
    if (bflb_flash_read(FACE_STORE_FLASH_BASE, s_hdr, sizeof(s_hdr)) != 0) {
        return FACE_STORE_ERR_FLASH;
    }
    rc = header_parse(s_hdr, &info);
    if (rc != FACE_STORE_OK) {
        if (!s_logged_invalid) {
            s_logged_invalid = 1;
            printf("[face] store: no valid header (magic=%08lX) -> 未安装\r\n",
                   (unsigned long)rd_u32(s_hdr));
        }
        return rc;
    }
    s_logged_invalid = 0;
    s_info = info;
    s_valid = 1;
    printf("[face] image xip=%p (img_off=0x%lX)\r\n",
           (const void *)face_store_image_ptr(),
           (unsigned long)face_store_xip_offset());
    return FACE_STORE_OK;
}

int face_store_info(face_store_info_t *out, int with_crc)
{
    int rc;

    if (!s_valid) {
        rc = face_store_init();
        if (rc != FACE_STORE_OK) {
            return rc;
        }
    }
    if (with_crc) {
        uint32_t crc = 0;
        rc = image_crc_readback(&crc);
        if (rc != FACE_STORE_OK) {
            return rc;
        }
        if (crc != s_info.image_crc32) {
            return FACE_STORE_ERR_CRC;
        }
    }
    if (out != NULL) {
        *out = s_info;
    }
    return FACE_STORE_OK;
}

int face_store_erase(void)
{
    int rc;

    rc = bflb_flash_erase(FACE_STORE_FLASH_BASE, FACE_STORE_ERASE_LEN);
    if (rc != 0) {
        printf("[face] erase 0x%lX+0x%lX failed rc=%d\r\n",
               (unsigned long)FACE_STORE_FLASH_BASE,
               (unsigned long)FACE_STORE_ERASE_LEN, rc);
        return FACE_STORE_ERR_FLASH;
    }
    face_store_cache_invalidate();
    memset(&s_info, 0, sizeof(s_info));
    s_valid = 0;
    s_begun = 0;
    s_erased = 1;
    return FACE_STORE_OK;
}

int face_store_begin(uint32_t expect_len, uint32_t expect_crc)
{
    int rc;

    if (expect_len != FACE_IMAGE_MAX) {
        return FACE_STORE_ERR_ARGS;
    }
    if (!s_erased) {
        /* 直接在旧数据上叠加写会得到"旧值 AND 新值"的乱码；没擦过就先擦。 */
        rc = face_store_erase();
        if (rc != FACE_STORE_OK) {
            return rc;
        }
    }
    s_expect_crc = expect_crc;
    s_hi = 0;
    s_recv = 0;
    s_begun = 1;
    return FACE_STORE_OK;
}

int face_store_write(uint32_t off, const uint8_t *data, uint32_t len)
{
    int rc;

    if (!s_begun) {
        return FACE_STORE_ERR_STATE;
    }
    if (data == NULL || len == 0 || len > 48u) {
        return FACE_STORE_ERR_ARGS;
    }
    /* bflb_flash_write 按 4 B 粒度编程；shell 工具按 48 B 分片正好对齐 */
    if ((off & 0x03u) != 0 || (len & 0x03u) != 0) {
        return FACE_STORE_ERR_ARGS;
    }
    if (off > FACE_IMAGE_MAX || len > FACE_IMAGE_MAX - off) {
        return FACE_STORE_ERR_ARGS;
    }
    rc = bflb_flash_write(FACE_STORE_FLASH_BASE + FACE_STORE_IMAGE_OFF + off,
                          (uint8_t *)data, len);
    if (rc != 0) {
        printf("[face] write off=%lu len=%lu failed rc=%d\r\n",
               (unsigned long)off, (unsigned long)len, rc);
        return FACE_STORE_ERR_FLASH;
    }
    if (off + len > s_hi) {
        s_hi = off + len;
    }
    s_recv += len;
    /* 只要写过 image 区，这个槽就不再是"干净已擦"状态：失败重来时 begin 会补擦，
     * 避免在已编程数据上叠加写（NOR 只能把 1 写成 0）。 */
    s_erased = 0;
    return FACE_STORE_OK;
}

int face_store_finish(const face_store_info_t *layout)
{
    face_store_info_t info;
    uint32_t crc = 0;
    int rc;

    if (!s_begun) {
        return FACE_STORE_ERR_STATE;
    }
    if (layout == NULL) {
        return FACE_STORE_ERR_ARGS;
    }
    if (s_hi != FACE_IMAGE_MAX) {
        printf("[face] finish: 未收齐 hi=%lu/%lu\r\n",
               (unsigned long)s_hi, (unsigned long)FACE_IMAGE_MAX);
        return FACE_STORE_ERR_STATE;
    }

    /* 关键：坏 CRC 直接拒装，绝不写 header（旧表盘最多变成"未安装"，不会变成花屏表盘） */
    face_store_cache_invalidate();
    rc = image_crc_readback(&crc);
    if (rc != FACE_STORE_OK) {
        return rc;
    }
    if (crc != s_expect_crc) {
        s_begun = 0;
        printf("[face] finish: CRC mismatch got=%08lX expect=%08lX -> 拒装\r\n",
               (unsigned long)crc, (unsigned long)s_expect_crc);
        return FACE_STORE_ERR_CRC;
    }

    header_build(layout, crc);

    rc = bflb_flash_erase(FACE_STORE_FLASH_BASE, FACE_STORE_HEADER_SIZE);
    if (rc != 0) {
        printf("[face] header erase failed rc=%d\r\n", rc);
        return FACE_STORE_ERR_FLASH;
    }
    rc = bflb_flash_write(FACE_STORE_FLASH_BASE, s_hdr, 256);
    if (rc != 0) {
        printf("[face] header write failed rc=%d\r\n", rc);
        return FACE_STORE_ERR_FLASH;
    }

    /* 回读比对：写没写进去以 flash 为准，不看返回值 */
    memset(s_crc_buf, 0, FACE_STORE_CHUNK);
    if (bflb_flash_read(FACE_STORE_FLASH_BASE, s_crc_buf, 256) != 0 ||
        memcmp(s_crc_buf, s_hdr, 256) != 0) {
        printf("[face] header verify failed\r\n");
        return FACE_STORE_ERR_FLASH;
    }
    if (header_parse(s_crc_buf, &info) != FACE_STORE_OK) {
        return FACE_STORE_ERR_FORMAT;
    }

    s_info = info;
    s_valid = 1;
    s_begun = 0;
    s_erased = 0;   /* 下次 begin 前必须重新擦 image 区 */
    printf("[face] install ok crc=%08lX recv=%lu B\r\n",
           (unsigned long)info.image_crc32, (unsigned long)s_recv);
    return FACE_STORE_OK;
}

const uint8_t *face_store_image_ptr(void)
{
    /* 见文件头注释：XIP = FLASH_XIP_BASE + flash_offset - image_offset */
    return (const uint8_t *)(uintptr_t)(FLASH_XIP_BASE + FACE_STORE_IMAGE_FLASH_OFF -
                                        face_store_xip_offset());
}

int face_store_is_valid(void)
{
    return s_valid;
}
