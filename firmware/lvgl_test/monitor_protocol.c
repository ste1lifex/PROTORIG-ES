#include "monitor_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ------------------------------------------------------------------------- */
/* v1.1 title bitmaps (single-focus: slot 0 only)                            */
/*                                                                           */
/* The shell task only fills the staging buffer, checks CRC/hash at bmpend    */
/* and raises a pending flag. The committed buffer and its A8 expansion are   */
/* written only from pm_bitmap_get(), i.e. the LVGL task, so committed pixels  */
/* have a single writer (no tearing while LVGL renders) and pm_link_t only     */
/* carries metadata through the per-cycle stack copy.                         */
/* ------------------------------------------------------------------------- */
static uint8_t  s_bmp_stage[PM_BMP_MAX_BYTES];       /* 750 B  */
static uint8_t  s_bmp_commit[PM_BMP_MAX_BYTES];      /* 750 B  */
static uint8_t  s_bmp_a8[PM_BMP_A8_BYTES];           /* 6000 B */
static uint16_t s_bmp_w, s_bmp_h;
static uint32_t s_bmp_hash;
static bool     s_bmp_present;

/* pending flags are raised by the shell task, applied by pm_bitmap_get() */
static volatile bool s_bmp_pending;
static volatile bool s_bmp_clear_pending;
static uint16_t s_bmp_pending_w, s_bmp_pending_h;
static uint32_t s_bmp_pending_hash;

/* one in-flight 1-bit transfer; PC is stop-and-wait so a single staging state
 * is enough, blocks are written at their i*30 offset (order-independent) */
static struct {
    bool active;
    uint16_t w, n;
    uint32_t hash, last_ms;
    uint32_t mask;              /* bit i = block i received (n <= 25) */
} s_stage;

static uint32_t fnv1a32(const uint8_t *p, size_t n)
{
    uint32_t h = 0x811C9DC5u;
    while(n--) { h ^= *p++; h *= 0x01000193u; }
    return h;
}
static uint16_t crc16_ccitt(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFFu;
    while(n--) {
        c ^= (uint16_t)((uint16_t)*p++ << 8);
        for(int i = 0; i < 8; i++)
            c = (c & 0x8000u) ? (uint16_t)((uint16_t)(c << 1) ^ 0x1021u)
                              : (uint16_t)(c << 1);
    }
    return c;
}
static uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while(n--) {
        c ^= *p++;
        for(int i = 0; i < 8; i++)
            c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}
static size_t bmp_bytes(uint16_t w) { return (size_t)(w / 8u) * PM_BMP_H; }
static size_t bmp_blocks(size_t bytes)
{ return (bytes + PM_BMP_BLOCK_BYTES - 1u) / PM_BMP_BLOCK_BYTES; }
static uint32_t bmp_mask(size_t blocks)
{ return blocks >= 32u ? 0xFFFFFFFFu : ((1u << blocks) - 1u); }

static void bmp_session_reset(void)
{
    memset(s_bmp_stage, 0, sizeof(s_bmp_stage));
    s_stage.active = false;
    s_bmp_pending = false;
    s_bmp_clear_pending = true;
    s_bmp_pending_w = 0;
    s_bmp_pending_h = 0;
    s_bmp_pending_hash = 0;
}
static void bmp_clear_meta(pm_link_t *s)
{
    s->bmp_valid = 0;
    memset(s->bmp_w, 0, sizeof(s->bmp_w));
    memset(s->bmp_h, 0, sizeof(s->bmp_h));
    memset(s->bmp_hash, 0, sizeof(s->bmp_hash));
}
static void bmp_clear_slot0(pm_link_t *s)
{
    s->bmp_valid &= (uint8_t)~1u;
    s->bmp_w[0] = 0;
    s->bmp_h[0] = 0;
    s->bmp_hash[0] = 0;
    s_bmp_pending = false;
    s_bmp_clear_pending = true;
    s_stage.active = false;
}
static void pm_bmp_expand(void)
{
    uint16_t width = s_bmp_w;
    uint16_t height = s_bmp_h;
    uint16_t stride = (uint16_t)(width / 8u);

    for(uint16_t y = 0; y < height; y++) {
        for(uint16_t x = 0; x < width; x++) {
            uint8_t bit = (uint8_t)((s_bmp_commit[(size_t)y * stride + (x >> 3)] >>
                                     (7u - (x & 7u))) & 1u);
            s_bmp_a8[(size_t)y * width + x] = bit ? 255u : 0u;
        }
    }
}
const uint8_t *pm_bitmap_get(int slot, uint16_t *w, uint16_t *h)
{
    if(slot != 0) return NULL;              /* single-focus: slot 1 is never live */

    if(s_bmp_clear_pending) {
        s_bmp_clear_pending = false;
        s_bmp_present = false;
    }
    if(s_bmp_pending) {
        uint16_t width = s_bmp_pending_w;
        uint16_t height = s_bmp_pending_h;
        size_t total = bmp_bytes(width);
        s_bmp_pending = false;
        if(width < 8u || width > PM_BMP_MAX_W || (width % 8u) != 0u ||
           height != PM_BMP_H || total > PM_BMP_MAX_BYTES) return NULL;
        memcpy(s_bmp_commit, s_bmp_stage, total);
        s_bmp_w = width;
        s_bmp_h = height;
        s_bmp_hash = s_bmp_pending_hash;
        s_bmp_present = true;
        pm_bmp_expand();
    }
    if(!s_bmp_present) return NULL;
    if(w) *w = s_bmp_w;
    if(h) *h = s_bmp_h;
    return s_bmp_a8;
}


static int hex(char c)
{ return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : -1; }
static bool number(const char *s, int64_t lo, int64_t hi, int64_t *out)
{
    char *end;
    if (!s || !*s || (*s != '-' && (*s < '0' || *s > '9'))) return false;
    errno=0;
    long long v=strtoll(s,&end,10);
    if (errno || *end || v<lo || v>hi) return false;
    *out=v; return true;
}
static bool hex_u32(const char *s, uint32_t *out)
{
    if(!s || strlen(s) != 8) return false;
    uint32_t v = 0;
    for(int i = 0; i < 8; i++) {
        int h = hex(s[i]);
        if(h < 0) return false;
        v = (v << 4) | (uint32_t)h;
    }
    *out = v;
    return true;
}
static bool hex_u16(const char *s, uint16_t *out)
{
    if(!s || strlen(s) != 4) return false;
    unsigned v = 0;
    for(int i = 0; i < 4; i++) {
        int h = hex(s[i]);
        if(h < 0) return false;
        v = (v << 4) | (unsigned)h;
    }
    *out = (uint16_t)v;
    return true;
}
static bool hex_bytes(const char *s, size_t len, uint8_t *out)
{
    if(!s || strlen(s) != len * 2u) return false;
    for(size_t i = 0; i < len; i++) {
        int a = hex(s[i * 2u]), b = hex(s[i * 2u + 1u]);
        if(a < 0 || b < 0) return false;
        out[i] = (uint8_t)(a * 16 + b);
    }
    return true;
}
static bool tool_token(const char *s, char out[9])
{
    size_t n = strlen(s);
    if(n < 1 || n > 8) return false;
    for(size_t i = 0; i < n; i++)
        if((unsigned char)s[i] < 32u || (unsigned char)s[i] > 126u) return false;
    memcpy(out, s, n + 1u);
    return true;
}
static bool host_decode(const char *hextext, char *out)
{
    size_t n=strlen(hextext);
    if (!n || n%2 || n>PM_HOST_BYTES*2) return false;
    for (size_t i=0;i<n/2;i++) {
        int a=hex(hextext[i*2]), b=hex(hextext[i*2+1]);
        if(a<0 || b<0) return false;
        unsigned c=(unsigned)(a*16+b);
        if(c<32 || c==127) return false;
        out[i]=(char)c;
    }
    out[n/2]=0;
    /* Reject malformed UTF-8 rather than passing arbitrary bytes to LVGL. */
    for (size_t i=0;i<n/2;) {
        unsigned c=(unsigned char)out[i++], cp; int count;
        if(c<128) continue;
        if(c>=0xC2 && c<=0xDF) { cp=c&31;count=1; }
        else if(c>=0xE0 && c<=0xEF) { cp=c&15;count=2; }
        else if(c>=0xF0 && c<=0xF4) { cp=c&7;count=3; }
        else return false;
        int original=count;
        while(count--) {
            if(i>=n/2 || ((unsigned char)out[i]&0xC0)!=0x80) return false;
            cp=(cp<<6)|((unsigned char)out[i++]&63);
        }
        if((original==2 && cp<0x800) || (original==3 && cp<0x10000) ||
           (cp>=0xD800 && cp<=0xDFFF) || cp>0x10FFFF) return false;
    }
    return true;
}
bool pm_poll(pm_link_t *s, uint32_t now, bool configured)
{
    if(s->session && (!configured || (uint32_t)(now-s->last_ms)>=PM_TIMEOUT_MS)) {
        s->session=false;s->frame=false;s->tick=false;s->vram=-1;
        bmp_session_reset();bmp_clear_meta(s);
        s->revision++;return true;
    }
    return false;
}
bool pm_receive(pm_link_t *s, int argc, char **argv, uint32_t now,
                bool configured, char reply[160])
{
    reply[0]=0;pm_poll(s,now,configured);
    if(!configured || argc<3) return false;
    if(!strcmp(argv[1],"hello") && argc==3) {
        if(strlen(argv[2])!=16) return false;
        for(int i=0;i<16;i++) if(hex(argv[2][i])<0) return false;
        uint32_t rev=s->revision+1;
        memset(s,0,sizeof(*s));s->revision=rev;s->vram=-1;
        s->session=true;s->last_ms=now;strcpy(s->nonce,argv[2]);
        strcpy(s->host,"COMPUTER");
        bmp_session_reset();
        snprintf(reply,160,"@PM1 HELLO %s PROTORIG_ES 1 480 480",s->nonce);
        return true;
    }
    if(!s->session || strcmp(s->nonce,argv[2])) return false;
    if(!strcmp(argv[1],"bye") && argc==3) {
        s->session=false;s->frame=false;s->tick=false;s->vram=-1;
        bmp_session_reset();bmp_clear_meta(s);
        s->revision++;
        snprintf(reply,160,"@PM1 BYE %s",s->nonce);return true;
    }
    if(!strcmp(argv[1],"host") && argc==4) {
        char host[PM_HOST_BYTES+1];
        if(!host_decode(argv[3],host)) return false;
        strcpy(s->host,host);s->last_ms=now;s->revision++;
        snprintf(reply,160,"@PM1 HOST %s",s->nonce);return true;
    }
    if(!strcmp(argv[1],"frame") && argc==12) {
        int64_t v[8];
        const int64_t lo[]={1,-1,-1,-1,-1,-1,32,0};
        const int64_t hi[]={2147483647,1000,1000,1000,4294967294LL,4294967294LL,255,1};
        for(int i=0;i<8;i++) if(!number(argv[i+3],lo[i],hi[i],&v[i])) return false;
        /* Last field is protocol revision; reserved for wire compatibility. */
        if(strcmp(argv[11],"1") || (s->frame && (uint32_t)v[0]<=s->seq)) return false;
        s->seq=(uint32_t)v[0];s->cpu=v[1];s->gpu=v[2];s->ram=v[3];
        s->rx=v[4];s->tx=v[5];s->brightness=v[6];s->always_on=v[7];
        s->frame=true;s->last_ms=now;s->revision++;return true;
    }

    /* ---- protocol v1.1 (only sent after `cap`; v1 clients keep using frame) ---- */
    if(!strcmp(argv[1],"cap") && argc==3) {
        s->last_ms=now;s->revision++;
        snprintf(reply,160,"@PM1 CAP %s %u",s->nonce,(unsigned)PM_CAP_MASK);
        return true;
    }
    if(!strcmp(argv[1],"bmpq") && argc==4) {
        int64_t seq;
        if(!number(argv[3],1,4294967295LL,&seq)) return false;
        s->seq=(uint32_t)seq;s->last_ms=now;s->revision++;
        snprintf(reply,160,"@PM1 BMPQ %s %08lx %u %08lx %u",s->nonce,
            (unsigned long)s->bmp_hash[0],(unsigned)s->bmp_w[0],
            (unsigned long)s->bmp_hash[1],(unsigned)s->bmp_w[1]);
        return true;
    }
    /* Separate capability-negotiated metric: don't lengthen the 128-byte tick.
     * Like tick, retransmission of this idempotent update is safe after lost ACK. */
    if(!strcmp(argv[1],"vram") && argc==6) {
        int64_t seq,value,brand;
        if(!number(argv[3],1,4294967295LL,&seq) || !number(argv[4],-1,1000,&value) ||
           !number(argv[5],0,3,&brand)) return false;
        if((uint32_t)seq<s->seq) return false;
        if((uint32_t)seq==s->seq && (s->vram_seq!=s->seq || s->vram!=value || s->gpu_brand!=brand)) return false;
        s->seq=(uint32_t)seq;s->vram_seq=s->seq;s->vram=(int)value;s->gpu_brand=(int)brand;
        s->last_ms=now;s->revision++;
        return true;
    }
    if(!strcmp(argv[1],"tick") && argc==21) {
        int64_t seq,ao,need,run,s0,s1,step0,step1,ctx;
        const int64_t mlo[]={-1,-1,-1,-1,-1,32};
        const int64_t mhi[]={1000,1000,1000,4294967294LL,4294967294LL,255};
        int64_t m[6];
        char tool0[9],tool1[9];

        if(!number(argv[3],1,4294967295LL,&seq)) return false;
        for(int i=0;i<6;i++) if(!number(argv[4+i],mlo[i],mhi[i],&m[i])) return false;
        if(!number(argv[10],0,1,&ao)) return false;
        if(strcmp(argv[11],"1")) return false;
        if(!number(argv[12],0,999,&need)) return false;
        if(!number(argv[13],0,999,&run)) return false;
        if(!number(argv[14],PM_STATE_IDLE,PM_STATE_FAILED,&s0)) return false;
        if(!tool_token(argv[15],tool0)) return false;
        if(!number(argv[16],0,99999,&step0)) return false;
        if(!number(argv[17],PM_STATE_IDLE,PM_STATE_FAILED,&s1)) return false;
        if(!tool_token(argv[18],tool1)) return false;
        if(!number(argv[19],0,99999,&step1)) return false;
        if(!number(argv[20],-1,100,&ctx)) return false;
        if(s->frame && (uint32_t)seq<=s->seq) return false;

        s->seq=(uint32_t)seq;s->cpu=(int)m[0];s->gpu=(int)m[1];s->ram=(int)m[2];
        s->rx=m[3];s->tx=m[4];s->brightness=(int)m[5];s->always_on=(int)ao;
        s->need=(int)need;s->run=(int)run;
        s->lane_state[0]=(int)s0;strcpy(s->lane_tool[0],tool0);s->lane_step[0]=(int)step0;
        s->lane_state[1]=(int)s1;strcpy(s->lane_tool[1],tool1);s->lane_step[1]=(int)step1;
        s->ctx=(int)ctx;
        s->tick=true;s->frame=true;s->last_ms=now;s->revision++;
        return true;
    }
    if(!strcmp(argv[1],"bmp") && argc==11) {
        int64_t seq,slot,w,idx,n;
        uint32_t hash;
        uint16_t crc16v;
        uint8_t payload[PM_BMP_BLOCK_BYTES];

        if(!number(argv[3],1,4294967295LL,&seq)) return false;
        /* single-focus: only slot 0 carries a bitmap; slot1 data blocks are rejected
         * (the PC clears the unused slot with an all-zero bmpend, which is a no-op) */
        if(!number(argv[4],0,0,&slot)) return false;
        if(!number(argv[5],8,PM_BMP_MAX_W,&w) || (w % 8) != 0) return false;
        if(!number(argv[6],0,PM_BMP_MAX_BLOCKS-1,&idx)) return false;
        if(!number(argv[7],1,PM_BMP_MAX_BLOCKS,&n)) return false;
        if(idx>=n) return false;
        if(!hex_u32(argv[8],&hash)) return false;
        if(!hex_u16(argv[9],&crc16v)) return false;

        size_t total=bmp_bytes((uint16_t)w);
        if((size_t)n!=bmp_blocks(total)) return false;
        size_t off=(size_t)idx*PM_BMP_BLOCK_BYTES;
        size_t len=total-off;
        if(len>PM_BMP_BLOCK_BYTES) len=PM_BMP_BLOCK_BYTES;
        if(!hex_bytes(argv[10],len,payload)) return false;
        if(crc16_ccitt(payload,len)!=crc16v) return false;

        if(!s_stage.active || s_stage.w!=(uint16_t)w ||
           s_stage.hash!=hash || s_stage.n!=(uint16_t)n ||
           (uint32_t)(now-s_stage.last_ms)>=PM_BMP_STAGE_TIMEOUT_MS) {
            memset(s_bmp_stage,0,sizeof(s_bmp_stage));
            s_stage.active=true;s_stage.w=(uint16_t)w;
            s_stage.n=(uint16_t)n;s_stage.hash=hash;s_stage.mask=0;
            s_stage.last_ms=now;
        }
        memcpy(&s_bmp_stage[off],payload,len);
        s_stage.mask|=(uint32_t)(1u<<(unsigned)idx);
        s_stage.last_ms=now;
        s->seq=(uint32_t)seq;s->last_ms=now;s->revision++;
        return true;
    }
    if(!strcmp(argv[1],"bmpend") && argc==9) {
        int64_t seq,slot,w,h;
        uint32_t hash,crc;

        if(!number(argv[3],1,4294967295LL,&seq)) return false;
        if(!number(argv[4],0,PM_BMP_SLOTS-1,&slot)) return false;
        if(!number(argv[5],0,PM_BMP_MAX_W,&w)) return false;
        if(!number(argv[6],0,PM_BMP_H,&h)) return false;
        if(!hex_u32(argv[7],&hash)) return false;
        if(!hex_u32(argv[8],&crc)) return false;

        if(slot != 0) {
            /* Unused second slot: only the all-zero clear form is legal. Treat it
             * as a no-op that consumes seq and produces an ACK, otherwise the PC
             * would wait forever for an ACK it considers mandatory (stop-and-wait). */
            if(w==0 && h==0 && hash==0 && crc==0) {
                s->seq=(uint32_t)seq;s->last_ms=now;s->revision++;
                return true;
            }
            return false;
        }
        if(w==0 || h==0 || hash==0 || crc==0) {
            /* clear-slot form: pcmon bmpend <nonce> <seq> 0 0 0 00000000 00000000 */
            if(w!=0 || h!=0 || hash!=0 || crc!=0) return false;
            bmp_clear_slot0(s);
            s->seq=(uint32_t)seq;s->last_ms=now;s->revision++;
            return true;
        }
        if(w<8 || w>PM_BMP_MAX_W || (w % 8)!=0 || h!=PM_BMP_H) return false;
        size_t total=bmp_bytes((uint16_t)w);
        size_t n=bmp_blocks(total);
        if(n>PM_BMP_MAX_BLOCKS) return false;
        if(!s_stage.active || s_stage.w!=(uint16_t)w ||
           s_stage.hash!=hash || s_stage.n!=(uint16_t)n ||
           s_stage.mask!=bmp_mask(n) ||
           (uint32_t)(now-s_stage.last_ms)>=PM_BMP_STAGE_TIMEOUT_MS) return false;
        if(crc32_ieee(s_bmp_stage,total)!=crc) return false;
        if(fnv1a32(s_bmp_stage,total)!=hash) return false;

        s_bmp_pending_w=(uint16_t)w;
        s_bmp_pending_h=(uint16_t)h;
        s_bmp_pending_hash=hash;
        s_bmp_pending=true;
        s_stage.active=false;
        s->bmp_w[0]=(uint16_t)w;s->bmp_h[0]=(uint16_t)h;
        s->bmp_hash[0]=hash;s->bmp_valid|=(uint8_t)1u;
        s->bmp_w[1]=0;s->bmp_h[1]=0;s->bmp_hash[1]=0;
        s->seq=(uint32_t)seq;s->last_ms=now;s->revision++;
        return true;
    }
    return false;
}
