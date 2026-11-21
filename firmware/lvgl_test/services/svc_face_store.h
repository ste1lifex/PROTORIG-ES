/*
 * svc_face_store.h —— 图片表盘存储层（B：表盘可替换，2026-11-08）
 *
 * 只做「flash 读写 + 头/CRC 校验」，不碰 LVGL。契约见
 * V1.6/research/70_表盘可替换_实现方案.md §2。
 *
 * media 分区（partition_cfg_4M.toml：flash 0x378000，0x71000 = 452 KB）单槽：
 *
 *   0x0000        header 256 B（下称 H）
 *   0x1000        image blob：1024 B palette + 480x480 索引 = 231,424 B
 *   0x1000+231424 余下 ~227 KB 留给 M4（PWF/双槽）
 *
 * image blob 的 XIP 地址 = 0xA0000000 + 0x378000 + 0x1000 = 0xA0379000，
 * 直接把这个指针交给 LVGL 当 I8 图的 data（不拷进 RAM：230 KB 放不下）。
 *
 * header 布局（小端）：
 *   0   u32 magic 'PFIM'          18  u32 time_color 0xRRGGBB
 *   4   u8  version = 1           22  u32 image_len  = 231,424
 *   5   u8  fmt = 1 (I8)          26  u32 image_crc32（zlib CRC32，覆盖 image blob）
 *   6   u16 width  = 480          30  u8[2] reserved = 0
 *   8   u16 height = 480          32  u32 header_crc32（zlib CRC32，覆盖 H[0..31]）
 *   10  u16 palette_entries = 256 36..255 0xFF
 *   12  i16 time_x
 *   14  i16 time_y
 *   16  u8  font_id（0=UI_FONT_HUGE 1=UI_FONT_CLOCK 2=UI_FONT_HEAD 3=UI_FONT_TITLE）
 *   17  u8  flags（bit0 日期 bit1 电量）
 *
 * CRC32 用标准 zlib（poly 0xEDB88320，init/xorout 0xFFFFFFFF），与 Python
 * binascii.crc32 完全一致；实现用 16 项 nibble 表（64 B flash）省空间。
 */
#ifndef SVC_FACE_STORE_H
#define SVC_FACE_STORE_H

#include <stdint.h>

/* ---- 镜像格式 ---- */
#define FACE_IMAGE_W            480u
#define FACE_IMAGE_H            480u
#define FACE_IMAGE_PALETTE_N    256u
#define FACE_IMAGE_PALETTE_BYTES (FACE_IMAGE_PALETTE_N * 4u)  /* lv_color32_t: B,G,R,A */
#define FACE_IMAGE_MAX          (FACE_IMAGE_PALETTE_BYTES + FACE_IMAGE_W * FACE_IMAGE_H)

/* ---- 物理布局 ---- */
#define FACE_STORE_FLASH_BASE   0x378000u   /* media 分区起始（flash 偏移） */
#define FACE_STORE_HEADER_SIZE  0x1000u     /* 头部独占 1 个 4 KB 扇区 */
#define FACE_STORE_IMAGE_OFF    FACE_STORE_HEADER_SIZE
#define FACE_STORE_IMAGE_SIZE   FACE_IMAGE_MAX
/* 擦除范围：0x378000..0x3B1FFF（header 4 KB + image 231,424 B，向上取整到 4 KB）
 * = 0x3A000 = 58 个 4 KB 扇区，仍在 media 分区（0x71000）内。 */
#define FACE_STORE_ERASE_LEN    0x3A000u
/* image blob 的 flash 偏移；XIP 指针必须运行时换算：
 *   XIP = FLASH_XIP_BASE + FACE_STORE_IMAGE_FLASH_OFF - bflb_flash_get_image_offset()
 * （SDK 把应用镜像起点映射到 FLASH_XIP_BASE，见 romfs 的实现）。
 * 不要写死 0xA0379000：2026-11-08 上板花屏就是写死导致的。 */
#define FACE_STORE_IMAGE_FLASH_OFF (FACE_STORE_FLASH_BASE + FACE_STORE_IMAGE_OFF)
#define FACE_STORE_MAGIC        0x4D494650u   /* 'PFIM' */
#define FACE_STORE_VERSION      1u
#define FACE_STORE_FMT_I8       1u

/* header.flags */
#define FACE_FLAG_SHOW_DATE     0x01u
#define FACE_FLAG_SHOW_BATT     0x02u

/* 返回值：0 成功；负数为具体原因（shell / PC 工具按值打印） */
#define FACE_STORE_OK           0
#define FACE_STORE_ERR_ARGS    (-1)   /* 参数/长度/对齐/偏移越界 */
#define FACE_STORE_ERR_CRC     (-2)   /* image CRC32 不匹配 → 拒装（不写 header） */
#define FACE_STORE_ERR_FLASH   (-3)   /* bflb_flash_* 返回失败 */
#define FACE_STORE_ERR_STATE   (-4)   /* 状态机错（未 begin / 未收齐） */
#define FACE_STORE_ERR_FORMAT  (-5)   /* header 字段非法 */

typedef struct {
    uint16_t width, height;
    int16_t  time_x, time_y;
    uint8_t  font_id, flags;
    uint32_t time_color;
    uint32_t image_len;
    uint32_t image_crc32;
} face_store_info_t;

/* 读 header 并校验 magic/version/fmt/尺寸/长度/header_crc32，填缓存。
 * 返回 0 = 有可用图片表盘；<0 = 无（未安装 / 头部损坏 / 读失败）。
 * 不校验 image_crc32（231 KB 全片扫描，只在 face_store_info(...,1) 里做）。 */
int face_store_init(void);

/* 只读查询。with_crc != 0 时额外把 image blob 读回来重算 CRC：
 * 0 = 有效；FACE_STORE_ERR_CRC = 头部有效但图片 CRC 不符；<0 其他失败。 */
int face_store_info(face_store_info_t *out, int with_crc);

/* 擦 header + image 区（0x378000..0x3B1FFF）。成功后有图状态变"未安装"。 */
int face_store_erase(void);

/* 开始一次安装：校验 expect_len（必须是 FACE_IMAGE_MAX），记录期望 CRC。
 * 如果之前没有 face_store_erase()，这里会先自动擦一次（防止在旧数据上叠加写）。 */
int face_store_begin(uint32_t expect_len, uint32_t expect_crc);

/* 写 image blob（off 相对 blob 起点；off/len 必须 4 字节对齐，len <= 48）。 */
int face_store_write(uint32_t off, const uint8_t *data, uint32_t len);

/* 收齐后收尾：校验长度 → 把 image 读回来重算 CRC（坏 CRC 直接拒装，不写 header）
 * → 擦 header 扇区 → 写 256 B header → 回读比对。
 * layout 里只用 time_x/time_y/font_id/flags/time_color（其余字段由本函数填）。 */
int face_store_finish(const face_store_info_t *layout);

/* image blob 的 XIP 指针，直接给 lv_image_dsc_t.data 用（不拷贝）。 */
const uint8_t *face_store_image_ptr(void);

/* 最近一次 face_store_init() 的结果（0 = 有有效表盘），不重新读 flash。 */
int face_store_is_valid(void);

#endif /* SVC_FACE_STORE_H */
