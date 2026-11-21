/*
 * svc_face_panel.h —— 默认表盘下半区面板的存储层（76 §2.1，2026-11-08）
 *
 * 只做「flash 读写 + 头/CRC 校验」，不碰 LVGL（和 svc_face_store 同一分工）。
 * 契约见 V1.6/research/76_表盘精简与下半区面板_2026-11-08.md §2.1。
 *
 * media 分区（flash 0x378000 起，0x71000 = 452 KB）里紧跟着 face 3 槽的第二个槽：
 *
 *   0x3B2000        header 256 B（下称 H，独占 1 个 4 KB 扇区）
 *   0x3B3000        image blob：1024 B palette + 480x232 索引 = 112,384 B
 *   0x3CE700        image 结束；擦除区到 0x3CF000（整片 0x1D000，仍在 media 分区内）
 *
 * image blob 的 XIP 地址必须运行时算（同 face 3，别写死）：
 *   XIP = FLASH_XIP_BASE + 0x3B3000 - bflb_flash_get_image_offset()
 * 写死的后果见 research/71 §11.1（第一次上板花屏就是它）。
 *
 * header 布局（小端，与 research/76 §2.1 逐字节一致）：
 *   0   u32 magic 'PFPN' = 0x4E504650    12  u32 image_len  = 112,384
 *   4   u8  version = 1                  16  u32 image_crc32（zlib CRC32，覆盖 image blob）
 *   5   u8  fmt = 1 (I8)                 20  u32 header_crc32（zlib CRC32，覆盖 H[0..19]）
 *   6   u16 width  = 480                 24..255 0xFF
 *   8   u16 height = 232
 *   10  u16 y      = 248                 ← 面板在 480x480 屏幕上的 y（= watch_photo 的位置）
 *
 * 注意 header_crc32 **只覆盖前 20 B**（和 face 3 覆盖 32 B 不同，别照抄）。
 * CRC32 用标准 zlib（poly 0xEDB88320，init/xorout 0xFFFFFFFF），与 Python
 * binascii.crc32 完全一致 —— 和 `faceimg` 同一张 16 项 nibble 表。
 */
#ifndef SVC_FACE_PANEL_H
#define SVC_FACE_PANEL_H

#include <stdint.h>

/* ---- 面板图格式（I8）---- */
#define FACE_PANEL_W            480u
#define FACE_PANEL_H            232u
#define FACE_PANEL_Y            248u
#define FACE_PANEL_PALETTE_N    256u
#define FACE_PANEL_PALETTE_BYTES (FACE_PANEL_PALETTE_N * 4u)  /* lv_color32_t: B,G,R,A */
/* 1024 + 480*232 = 112,384 B（0x1B700） */
#define FACE_PANEL_IMAGE_LEN    (FACE_PANEL_PALETTE_BYTES + FACE_PANEL_W * FACE_PANEL_H)

/* ---- 物理布局（flash 偏移）---- */
#define FACE_PANEL_FLASH_BASE   0x3B2000u   /* header 起点（face 3 槽擦除区之后） */
#define FACE_PANEL_HEADER_SIZE  0x1000u     /* header 独占 1 个 4 KB 扇区 */
#define FACE_PANEL_IMAGE_OFF    FACE_PANEL_HEADER_SIZE
#define FACE_PANEL_IMAGE_FLASH_OFF (FACE_PANEL_FLASH_BASE + FACE_PANEL_IMAGE_OFF)
/* 擦除范围 0x3B2000..0x3CEFFF（header 4 KB + image 112,384 B，向上取整到 4 KB），
 * 仍在 media 分区（0x378000 + 0x71000 = 0x3E9000）之前。 */
#define FACE_PANEL_ERASE_LEN    0x1D000u

#define FACE_PANEL_MAGIC        0x4E504650u  /* 'PFPN' */
#define FACE_PANEL_VERSION      1u
#define FACE_PANEL_FMT_I8       1u

/* 返回值：0 成功；负数为具体原因（shell / PC 工具按值打印） */
#define FACE_PANEL_OK           0
#define FACE_PANEL_ERR_ARGS    (-1)   /* 参数/长度/对齐/偏移越界 */
#define FACE_PANEL_ERR_CRC     (-2)   /* image CRC32 不匹配 → 拒装（不写 header） */
#define FACE_PANEL_ERR_FLASH   (-3)   /* bflb_flash_* 返回失败 */
#define FACE_PANEL_ERR_STATE   (-4)   /* 状态机错（未 begin / 未收齐） */
#define FACE_PANEL_ERR_FORMAT  (-5)   /* header 字段非法 */

typedef struct {
    uint16_t width, height;     /* 480 / 232 */
    uint16_t y;                 /* 248：面板在屏幕上的 y（= watch_photo 的位置） */
    uint32_t image_len;         /* 112,384 */
    uint32_t image_crc32;
} face_panel_info_t;

/* 读 header 并校验 magic/version/fmt/尺寸/长度/header_crc32，填缓存。
 * 返回 0 = 有可安装的面板；<0 = 无（未安装 / 头部损坏 / 读失败）。
 * 不校验 image_crc32（112 KB 全片扫描，只在 face_panel_info(...,1) 里做）。 */
int face_panel_init(void);

/* 只读查询。with_crc != 0 时额外把 image blob 读回来重算 CRC：
 * 0 = 有效；FACE_PANEL_ERR_CRC = 头部有效但图片 CRC 不符；<0 其他失败。 */
int face_panel_info(face_panel_info_t *out, int with_crc);

/* 1 = 当前有有效面板（懒加载：第一次调用会读 header，之后只看缓存）。
 * face_lvgl_create() 每次重建表盘都会问一次，所以这里不能刷屏。 */
int face_panel_available(void);

/* image blob 的 XIP 指针，直接给 lv_image_dsc_t.data 用（不拷贝）。 */
const uint8_t *face_panel_image_ptr(void);

/* 擦整个槽（0x3B2000..0x3CEFFF，29 个 4 KB 扇区）。成功后状态变"未安装"。 */
int face_panel_erase(void);

/* 只擦 header 扇区（4 KB）：面板立即变"未安装"，face 0 回内置 watch photo。
 * 图片数据留在 flash 里，下次 begin()/erase() 时再整片擦（省 28 个扇区的擦除时间）。 */
int face_panel_clear(void);

/* 开始一次安装：校验 expect_len（必须是 FACE_PANEL_IMAGE_LEN），记录期望 CRC。
 * 如果之前没有 face_panel_erase()/clear() 之后的整片擦，这里会先自动擦一次
 * （防止在旧数据上叠加写：NOR 只能把 1 写成 0）。 */
int face_panel_begin(uint32_t expect_len, uint32_t expect_crc);

/* 写 image blob（off 相对 blob 起点；off/len 必须 4 字节对齐，len <= 48）。 */
int face_panel_write(uint32_t off, const uint8_t *data, uint32_t len);

/* 收齐后收尾：校验长度 → 把 image 读回来重算 CRC（坏 CRC 直接拒装，不写 header）
 * → 擦 header 扇区 → 写 256 B header → 回读比对。
 * 面板的几何/位置是固定的（480x232 @ y=248），所以没有 layout 参数。 */
int face_panel_finish(void);

#endif /* SVC_FACE_PANEL_H */
