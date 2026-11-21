#ifndef MONITOR_PROTOCOL_H
#define MONITOR_PROTOCOL_H
#include <stdbool.h>
#include <stdint.h>

#define PM_TIMEOUT_MS 5000u
#define PM_HOST_BYTES 32

/* v1.1 1-bit title bitmaps: buffers stay in monitor_protocol.c (8-bit A8 expansion
 * included); pm_link_t only carries metadata so the per-cycle stack copy in
 * pc_monitor_service stays small.
 *
 * Single-focus layout: only slot 0 is live (one active session, big title).
 * The wire still has two slots for backward compatibility: bmpq always reports
 * slot 1 as empty, a slot1 data block is rejected, and an all-zero bmpend for
 * slot 1 is accepted as a no-op so the PC gets its ACK. */
#define PM_BMP_SLOTS           2
#define PM_BMP_H               20
#define PM_BMP_MAX_W           300
/* Worst-case budget requested for the 20px title: 300 x 20 / 8 = 750 B of 1-bit
 * rows and 300 x 20 = 6000 B of A8. The wire keeps the byte-aligned "w is a
 * multiple of 8" rule, so the widest legal bitmap is 296 px (740 B); the extra
 * headroom keeps the RAM accounting aligned with the 300px spec. */
#define PM_BMP_MAX_BYTES       750
#define PM_BMP_A8_BYTES        6000
#define PM_BMP_BLOCK_BYTES     30
#define PM_BMP_MAX_BLOCKS      25                               /* ceil(750 / 30) */
#define PM_BMP_STAGE_TIMEOUT_MS 3000u

/* cap reply mask: bit0 tick, bit1 bmp, bit2 dual slot, bit3 bmpq, bit4 VRAM */
#define PM_CAP_MASK 0x1Fu

/* lane_state codes carried by tick s0 (s1 is parsed but ignored by the UI) */
#define PM_STATE_IDLE    0
#define PM_STATE_RUNNING 1
#define PM_STATE_NEEDS   2
#define PM_STATE_DONE    3
#define PM_STATE_FAILED  4

/* ASCII shell transport, bounded below the existing 128-byte command buffer.
 * Values: tenths of percent, bytes/sec; -1 means unavailable. No LVGL/USB calls. */
typedef struct {
    bool session, frame;
    bool tick;                  /* v1.1 tick seen (also sets frame=true) */
    char nonce[17], host[PM_HOST_BYTES + 1];
    uint32_t last_ms, seq, revision;
    int cpu, gpu, ram, brightness, always_on;
    int vram;                   /* tenths of percent; -1 unavailable */
    int gpu_brand;              /* 0 default, 1 NVIDIA, 2 AMD, 3 Intel */
    uint32_t vram_seq;          /* retry acknowledgement for the last VRAM update */
    int64_t rx, tx;

    /* v1.1 tick payload: focus state + tool/steps. lane1 fields are kept on the
     * wire for compatibility but the single-focus UI ignores them. */
    int need, run;
    int lane_state[PM_BMP_SLOTS];       /* PM_STATE_* (0..4) */
    char lane_tool[PM_BMP_SLOTS][9];    /* <= 8 printable ASCII chars */
    int lane_step[PM_BMP_SLOTS];
    int ctx;                            /* -1 = unavailable, else 0..100 */

    /* v1.1 committed bitmap metadata; pixels live in monitor_protocol.c.
     * Slot 1 metadata stays zero (single-focus UI only uses slot 0). */
    uint16_t bmp_w[PM_BMP_SLOTS];
    uint16_t bmp_h[PM_BMP_SLOTS];
    uint32_t bmp_hash[PM_BMP_SLOTS];
    uint8_t bmp_valid;                  /* bit N = slot N committed */
} pm_link_t;

bool pm_poll(pm_link_t *s, uint32_t now, bool configured);
bool pm_receive(pm_link_t *s, int argc, char **argv, uint32_t now,
                bool configured, char reply[160]);

/* Expand the committed slot-0 bitmap to A8 (0/255, stride = width) in
 * monitor_protocol.c's static buffer and return it. Call only from the LVGL task.
 * Returns NULL when there is no committed bitmap (or slot != 0). */
const uint8_t *pm_bitmap_get(int slot, uint16_t *w, uint16_t *h);
#endif
