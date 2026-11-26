// em_png.c - streaming PNG decoder (resumable inflate, 1 scanline of state)
// and low-RAM PNG encoder (filter + LZ77/fixed-Huffman deflate).
#include "em_priv.h"
#include <stdlib.h>

#ifndef EM_PNG_BAND
#define EM_PNG_BAND 8            // rows handed to the callback at once
#endif
#define WIN 32768                // inflate window (spec max; PNGs in the wild use it)

// ---- resumable inflate ----------------------------------------------------------------
typedef struct { uint16_t cnt[16]; uint16_t sym[288]; } huff_t;

typedef struct {
    em_io_t *io;
    // zlib byte source: walks IDAT chunks
    uint32_t chunk_left;
    int      src_done;
    uint8_t  inbuf[256]; uint16_t in_pos, in_len;
    // bit reader
    uint32_t bitbuf; int bitcnt;
    // state
    int mode;                    // 0=need block header, 1=stored, 2=huffman, 3=done
    int final_blk;
    uint32_t stored_left;
    huff_t lit, dist;
    uint32_t copy_len, copy_dist;
    uint8_t *win; uint32_t wpos;
    int err;
} inf_t;

static int inf_byte(inf_t *s) {
    if (s->in_pos >= s->in_len) {
        s->in_pos = s->in_len = 0;
        while (s->chunk_left == 0) {
            uint8_t h[12];
            if (s->src_done) return -1;
            // skip the CRC of the previous IDAT (4 bytes) handled when we read the next header
            if (io_read_exact(s->io, h, 8)) { s->src_done = 1; return -1; }
            uint32_t len = rd32be(h);
            if (!memcmp(h + 4, "IDAT", 4)) { s->chunk_left = len; if (len == 0) { s->io->read(s->io->ctx, h, 4); } }
            else if (!memcmp(h + 4, "IEND", 4)) { s->src_done = 1; return -1; }
            else { // some other chunk between IDATs: skip data + crc
                uint32_t pos = s->io->tell(s->io->ctx);
                if (s->io->seek(s->io->ctx, pos + len + 4)) { s->src_done = 1; return -1; }
            }
        }
        uint32_t n = s->chunk_left < sizeof s->inbuf ? s->chunk_left : sizeof s->inbuf;
        int r = s->io->read(s->io->ctx, s->inbuf, n);
        if (r <= 0) { s->src_done = 1; return -1; }
        s->in_len = (uint16_t)r; s->chunk_left -= (uint32_t)r;
        if (s->chunk_left == 0) {
            uint8_t crc[4]; s->io->read(s->io->ctx, crc, 4);   // IDAT CRC (not verified: speed/RAM)
        }
    }
    return s->inbuf[s->in_pos++];
}

static int inf_bits(inf_t *s, int n) {
    while (s->bitcnt < n) {
        int b = inf_byte(s);
        if (b < 0) { s->err = 1; return 0; }
        s->bitbuf |= (uint32_t)b << s->bitcnt; s->bitcnt += 8;
    }
    int v = (int)(s->bitbuf & ((1u << n) - 1));
    s->bitbuf >>= n; s->bitcnt -= n;
    return v;
}

static void huff_build(huff_t *h, const uint8_t *len, int n) {
    uint16_t offs[16];
    memset(h->cnt, 0, sizeof h->cnt);
    for (int i = 0; i < n; i++) h->cnt[len[i]]++;
    h->cnt[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (uint16_t)(offs[i] + h->cnt[i]);
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (uint16_t)i;
}

static int huff_dec(inf_t *s, const huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= 15; l++) {
        code |= inf_bits(s, 1);
        if (s->err) return -1;
        int c = h->cnt[l];
        if (code - c < first) return h->sym[index + (code - first)];
        index += c; first += c; first <<= 1; code <<= 1;
    }
    return -1;
}

static const uint16_t k_lbase[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t  k_lext[]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t k_dbase[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t  k_dext[]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static void inf_init(inf_t *s, em_io_t *io, uint8_t *win) {
    memset(s, 0, sizeof *s);
    s->io = io; s->win = win;
    // consume zlib header (2 bytes)
    inf_byte(s); inf_byte(s);
}

static int inf_block_header(inf_t *s) {
    s->final_blk = inf_bits(s, 1);
    int type = inf_bits(s, 2);
    if (s->err) return -1;
    if (type == 0) {
        s->bitbuf = 0; s->bitcnt = 0;                 // align
        int a = inf_byte(s), b = inf_byte(s), c = inf_byte(s), d = inf_byte(s);
        if ((a | b | c | d) < 0) return -1;
        s->stored_left = (uint32_t)(a | b << 8);
        s->mode = 1;
    } else if (type == 1) {
        uint8_t l[288];
        for (int i = 0; i < 144; i++) l[i] = 8;
        for (int i = 144; i < 256; i++) l[i] = 9;
        for (int i = 256; i < 280; i++) l[i] = 7;
        for (int i = 280; i < 288; i++) l[i] = 8;
        huff_build(&s->lit, l, 288);
        uint8_t d[30]; memset(d, 5, sizeof d);
        huff_build(&s->dist, d, 30);
        s->mode = 2;
    } else if (type == 2) {
        int hlit = inf_bits(s, 5) + 257, hdist = inf_bits(s, 5) + 1, hclen = inf_bits(s, 4) + 4;
        static const uint8_t ord[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
        uint8_t lens[320]; huff_t cl;
        memset(lens, 0, 19);
        for (int i = 0; i < hclen; i++) lens[ord[i]] = (uint8_t)inf_bits(s, 3);
        if (s->err) return -1;
        huff_build(&cl, lens, 19);
        uint8_t all[320]; int n = 0;
        while (n < hlit + hdist) {
            int sym = huff_dec(s, &cl);
            if (sym < 0) return -1;
            if (sym < 16) all[n++] = (uint8_t)sym;
            else {
                int rep, val = 0;
                if (sym == 16) { if (!n) return -1; val = all[n - 1]; rep = 3 + inf_bits(s, 2); }
                else if (sym == 17) rep = 3 + inf_bits(s, 3);
                else rep = 11 + inf_bits(s, 7);
                if (n + rep > hlit + hdist) return -1;
                while (rep--) all[n++] = (uint8_t)val;
            }
        }
        huff_build(&s->lit, all, hlit);
        huff_build(&s->dist, all + hlit, hdist);
        s->mode = 2;
    } else return -1;
    return 0;
}

// Produces up to `want` bytes. Returns bytes produced (0 => end of stream or error).
static uint32_t inf_read(inf_t *s, uint8_t *out, uint32_t want) {
    uint32_t n = 0;
    while (n < want && !s->err) {
        if (s->copy_len) {
            uint8_t b = s->win[(s->wpos - s->copy_dist) & (WIN - 1)];
            s->win[s->wpos++ & (WIN - 1)] = b; out[n++] = b; s->copy_len--;
            continue;
        }
        if (s->mode == 3) break;
        if (s->mode == 0) {
            if (inf_block_header(s)) { s->err = 1; break; }
            continue;
        }
        if (s->mode == 1) {
            if (!s->stored_left) { s->mode = s->final_blk ? 3 : 0; continue; }
            int b = inf_byte(s);
            if (b < 0) { s->err = 1; break; }
            s->win[s->wpos++ & (WIN - 1)] = (uint8_t)b; out[n++] = (uint8_t)b; s->stored_left--;
            continue;
        }
        int sym = huff_dec(s, &s->lit);
        if (sym < 0) { s->err = 1; break; }
        if (sym < 256) { s->win[s->wpos++ & (WIN - 1)] = (uint8_t)sym; out[n++] = (uint8_t)sym; }
        else if (sym == 256) { s->mode = s->final_blk ? 3 : 0; }
        else {
            sym -= 257;
            if (sym >= 29) { s->err = 1; break; }
            int len = k_lbase[sym] + inf_bits(s, k_lext[sym]);
            int ds = huff_dec(s, &s->dist);
            if (ds < 0 || ds >= 30) { s->err = 1; break; }
            int dist = k_dbase[ds] + inf_bits(s, k_dext[ds]);
            if (s->err) break;
            s->copy_len = (uint32_t)len; s->copy_dist = (uint32_t)dist;
        }
    }
    return n;
}

// ---- decoder ---------------------------------------------------------------------------------
struct em_png {
    em_io_t *io;
    uint32_t w, h; uint8_t depth, ctype, bpp;   // bpp = bytes per pixel (>=1) for filtering
    uint32_t rowbytes;
    uint32_t idat_pos;                          // file offset of first chunk after IHDR
    uint8_t plte[256][3]; uint8_t trns[256]; int ntrns;
    uint16_t trns_key[3]; int has_key;
    uint8_t *cur, *prev;                        // scanline buffers (rowbytes each)
    uint16_t *band[2];
    inf_t *inf;
    uint8_t *win;
};

em_err_t em_png_open(em_png_t **out, em_io_t *io, em_img_info_t *info) {
    uint8_t sig[8], h[8 + 13 + 4];
    if (io->seek(io->ctx, 0) || io_read_exact(io, sig, 8) || memcmp(sig, "\x89PNG\r\n\x1a\n", 8)) return EM_ERR_FORMAT;
    if (io_read_exact(io, h, sizeof h) || memcmp(h + 4, "IHDR", 4)) return EM_ERR_FORMAT;
    uint32_t w = rd32be(h + 8), hh = rd32be(h + 12);
    uint8_t depth = h[16], ct = h[17], interlace = h[20];
    if (!w || !hh || w > 8192 || hh > 8192) return EM_ERR_UNSUPPORTED;
    if (interlace) return EM_ERR_UNSUPPORTED;   // Adam7 needs the whole image; out of scope for low RAM
    int ch = ct == 0 ? 1 : ct == 2 ? 3 : ct == 3 ? 1 : ct == 4 ? 2 : ct == 6 ? 4 : 0;
    if (!ch || (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)) return EM_ERR_FORMAT;
    if (ct == 3 && depth > 8) return EM_ERR_FORMAT;

    size_t rowbytes = ((size_t)w * ch * depth + 7) / 8;
    size_t band_sz = (size_t)EM_PNG_BAND * w * 2 * 2;
    size_t total = sizeof(em_png_t) + sizeof(inf_t) + 2 * rowbytes + band_sz;
    uint8_t *mem = em_alloc(total, EM_MEM_BIG);
    uint8_t *win = em_alloc(WIN, EM_MEM_BIG);
    if (!mem || !win) { em_free(mem); em_free(win); return EM_ERR_NOMEM; }
    memset(mem, 0, total);
    em_png_t *p = (em_png_t *)mem;
    p->io = io; p->w = w; p->h = hh; p->depth = depth; p->ctype = ct;
    p->bpp = (uint8_t)((ch * depth + 7) / 8); p->rowbytes = (uint32_t)rowbytes;
    p->inf = (inf_t *)(p + 1);
    p->cur = (uint8_t *)(p->inf + 1); p->prev = p->cur + rowbytes;
    p->band[0] = (uint16_t *)(p->prev + rowbytes);          // alignment: rowbytes arbitrary -> fix below
    p->win = win;
    // align band pointers to 2 bytes
    {
        uintptr_t a = ((uintptr_t)p->band[0] + 1) & ~(uintptr_t)1;
        p->band[0] = (uint16_t *)a; p->band[1] = p->band[0] + (size_t)EM_PNG_BAND * w;
    }
    // scan chunks up to first IDAT for PLTE/tRNS
    uint32_t off = 8 + 8 + 13 + 4;
    for (;;) {
        uint8_t ck[8];
        if (io->seek(io->ctx, off) || io_read_exact(io, ck, 8)) { em_png_close(p); return EM_ERR_FORMAT; }
        uint32_t len = rd32be(ck);
        if (!memcmp(ck + 4, "IDAT", 4)) { p->idat_pos = off; break; }
        if (!memcmp(ck + 4, "PLTE", 4) && len <= 768) {
            uint8_t t[768]; if (io_read_exact(io, t, len)) { em_png_close(p); return EM_ERR_FORMAT; }
            for (uint32_t i = 0; i < len / 3; i++) memcpy(p->plte[i], t + i * 3, 3);
        } else if (!memcmp(ck + 4, "tRNS", 4) && len <= 768) {
            uint8_t t[768]; if (io_read_exact(io, t, len)) { em_png_close(p); return EM_ERR_FORMAT; }
            if (ct == 3) { memcpy(p->trns, t, len < 256 ? len : 256); p->ntrns = (int)(len < 256 ? len : 256); }
            else if (len >= 2 * (ct == 2 ? 3u : 1u)) { p->has_key = 1; for (int i = 0; i < (ct == 2 ? 3 : 1); i++) p->trns_key[i] = rd16be(t + i * 2); }
        } else if (!memcmp(ck + 4, "IEND", 4)) { em_png_close(p); return EM_ERR_FORMAT; }
        off += 12 + len;
    }
    if (info) { info->width = (uint16_t)w; info->height = (uint16_t)hh; info->subsamp = 0; }
    *out = p;
    return EM_OK;
}

void em_png_close(em_png_t *p) { if (!p) return; em_free(p->win); em_free(p); }

static inline int paeth(int a, int b, int c) {
    int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

static void unfilter(uint8_t *cur, const uint8_t *prev, uint32_t n, int bpp, int ft) {
    switch (ft) {
    case 1: for (uint32_t i = bpp; i < n; i++) cur[i] = (uint8_t)(cur[i] + cur[i - bpp]); break;
    case 2: for (uint32_t i = 0; i < n; i++) cur[i] = (uint8_t)(cur[i] + prev[i]); break;
    case 3:
        for (uint32_t i = 0; i < (uint32_t)bpp && i < n; i++) cur[i] = (uint8_t)(cur[i] + (prev[i] >> 1));
        for (uint32_t i = bpp; i < n; i++) cur[i] = (uint8_t)(cur[i] + ((cur[i - bpp] + prev[i]) >> 1));
        break;
    case 4:
        for (uint32_t i = 0; i < (uint32_t)bpp && i < n; i++) cur[i] = (uint8_t)(cur[i] + prev[i]);
        for (uint32_t i = bpp; i < n; i++) cur[i] = (uint8_t)(cur[i] + paeth(cur[i - bpp], prev[i], prev[i - bpp]));
        break;
    }
}

static inline int blend(int c, int a, int bg) { return (c * a + bg * (255 - a) + 127) / 255; }

static void row_to_565(const em_png_t *p, const uint8_t *s, uint16_t *d, int bgr, int bgg, int bgb) {
    uint32_t w = p->w;
    for (uint32_t x = 0; x < w; x++) {
        int r, g, b, a = 255;
        switch (p->ctype) {
        case 0: {
            int v;
            if (p->depth == 16) { v = s[x * 2]; if (p->has_key && rd16be(s + x * 2) == p->trns_key[0]) a = 0; }
            else if (p->depth == 8) { v = s[x]; if (p->has_key && v == p->trns_key[0]) a = 0; }
            else {
                int per = 8 / p->depth, sh = (per - 1 - (int)(x % per)) * p->depth, m = (1 << p->depth) - 1;
                int raw = (s[x / per] >> sh) & m;
                if (p->has_key && raw == p->trns_key[0]) a = 0;
                v = raw * 255 / m;
            }
            r = g = b = v; break; }
        case 2:
            if (p->depth == 16) { r = s[x * 6]; g = s[x * 6 + 2]; b = s[x * 6 + 4];
                if (p->has_key && rd16be(s + x * 6) == p->trns_key[0] && rd16be(s + x * 6 + 2) == p->trns_key[1] && rd16be(s + x * 6 + 4) == p->trns_key[2]) a = 0; }
            else { r = s[x * 3]; g = s[x * 3 + 1]; b = s[x * 3 + 2];
                if (p->has_key && r == p->trns_key[0] && g == p->trns_key[1] && b == p->trns_key[2]) a = 0; }
            break;
        case 3: {
            int per = 8 / p->depth, sh = (per - 1 - (int)(x % per)) * p->depth, m = (1 << p->depth) - 1;
            int idx = p->depth == 8 ? s[x] : (s[x / per] >> sh) & m;
            r = p->plte[idx][0]; g = p->plte[idx][1]; b = p->plte[idx][2];
            if (idx < p->ntrns) a = p->trns[idx];
            break; }
        case 4: { int bps = p->depth / 8; r = g = b = s[x * 2 * bps]; a = s[x * 2 * bps + bps]; break; }
        default: { int bps = p->depth / 8; r = s[x * 4 * bps]; g = s[x * 4 * bps + bps]; b = s[x * 4 * bps + 2 * bps]; a = s[x * 4 * bps + 3 * bps]; break; }
        }
        if (a < 255) { r = blend(r, a, bgr); g = blend(g, a, bgg); b = blend(b, a, bgb); }
        d[x] = pack565(r, g, b);
    }
}

em_err_t em_png_decode(em_png_t *p, uint16_t bg565, em_rows_cb cb, void *user) {
    if (!p || !cb) return EM_ERR_ARG;
    int bgr = ((bg565 >> 11) & 31) * 255 / 31, bgg = ((bg565 >> 5) & 63) * 255 / 63, bgb = (bg565 & 31) * 255 / 31;
    if (p->io->seek(p->io->ctx, p->idat_pos)) return EM_ERR_IO;
    inf_init(p->inf, p->io, p->win);
    memset(p->prev, 0, p->rowbytes);
    int which = 0, in_band = 0;
    uint32_t y0 = 0;
    for (uint32_t y = 0; y < p->h; y++) {
        uint8_t ft;
        if (inf_read(p->inf, &ft, 1) != 1) return EM_ERR_FORMAT;
        if (inf_read(p->inf, p->cur, p->rowbytes) != p->rowbytes || ft > 4) return EM_ERR_FORMAT;
        unfilter(p->cur, p->prev, p->rowbytes, p->bpp, ft);
        row_to_565(p, p->cur, p->band[which] + (size_t)in_band * p->w, bgr, bgg, bgb);
        in_band++;
        uint8_t *t = p->cur; p->cur = p->prev; p->prev = t;
        if (in_band == EM_PNG_BAND || y + 1 == p->h) {
            if (cb(user, (int)y0, in_band, p->band[which], (int)p->w)) return EM_ERR_ABORT;
            y0 = y + 1; in_band = 0; which ^= 1;
        }
    }
    return EM_OK;
}

// ---- encoder --------------------------------------------------------------------------------------
#ifndef EM_PNG_ENC_WIN
#define EM_PNG_ENC_WIN 4096      // LZ77 window; bigger = smaller files, more RAM
#endif
#define HBITS 11
#define MAXMATCH 258

static uint32_t crc_upd(uint32_t crc, const uint8_t *p, size_t n) {
    static const uint32_t t[16] = {0, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
                                   0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
    crc = ~crc;
    while (n--) { crc ^= *p++; crc = (crc >> 4) ^ t[crc & 15]; crc = (crc >> 4) ^ t[crc & 15]; }
    return ~crc;
}

typedef struct {
    em_io_t *io; int err;
    uint8_t out[512]; uint32_t olen;
    uint32_t bitbuf; int bitcnt;
    uint32_t crc;                 // CRC of current chunk (type+data)
    uint32_t adler_a, adler_b;
    // LZ77
    uint8_t *ring; uint32_t rmask, rpos, rend;   // absolute positions in `rpos..rend` are pending
    uint32_t *head;               // hash -> absolute position + 1
    uint32_t idat_len;            // bytes buffered in current IDAT chunk body (flushed in `out`)
} enc_t;

// IDAT chunks are emitted in <=4KB pieces; we need the length before the data, so buffer one piece.
#define IDAT_PIECE 2048
typedef struct { uint8_t piece[IDAT_PIECE]; uint32_t plen; } piece_t;

static void chunk_write(enc_t *e, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t h[8]; wr32be(h, len); memcpy(h + 4, type, 4);
    uint32_t crc = crc_upd(0, h + 4, 4);
    crc = crc_upd(crc, data, len);
    uint8_t t[4]; wr32be(t, crc);
    if (io_write_all(e->io, h, 8) || (len && io_write_all(e->io, data, len)) || io_write_all(e->io, t, 4)) e->err = 1;
}


static inline void put_byte(enc_t *e, piece_t *pc, uint8_t b) {
    pc->piece[pc->plen++] = b;
    if (pc->plen == IDAT_PIECE) { chunk_write(e, "IDAT", pc->piece, pc->plen); pc->plen = 0; }
}
static inline void put_bits(enc_t *e, piece_t *pc, uint32_t v, int n) {   // LSB-first
    e->bitbuf |= v << e->bitcnt; e->bitcnt += n;
    while (e->bitcnt >= 8) { put_byte(e, pc, (uint8_t)e->bitbuf); e->bitbuf >>= 8; e->bitcnt -= 8; }
}
static inline uint32_t rev(uint32_t v, int n) { uint32_t r = 0; while (n--) { r = (r << 1) | (v & 1); v >>= 1; } return r; }
static inline void put_huff(enc_t *e, piece_t *pc, uint32_t code, int n) { put_bits(e, pc, rev(code, n), n); }

static void put_lit(enc_t *e, piece_t *pc, int c) {
    if (c < 144) put_huff(e, pc, 0x30 + c, 8);
    else put_huff(e, pc, 0x190 + (c - 144), 9);
}
static void put_match(enc_t *e, piece_t *pc, int len, int dist) {
    int li = 28; while (k_lbase[li] > len) li--;
    int sym = 257 + li;
    if (sym < 280) put_huff(e, pc, sym - 256, 7); else put_huff(e, pc, 0xC0 + (sym - 280), 8);
    if (k_lext[li]) put_bits(e, pc, (uint32_t)(len - k_lbase[li]), k_lext[li]);
    int di = 29; while (k_dbase[di] > dist) di--;
    put_huff(e, pc, (uint32_t)di, 5);
    if (k_dext[di]) put_bits(e, pc, (uint32_t)(dist - k_dbase[di]), k_dext[di]);
}

static inline uint32_t hash3(const uint8_t *r, uint32_t m, uint32_t pos) {
    uint32_t v = r[pos & m] | r[(pos + 1) & m] << 8 | (uint32_t)r[(pos + 2) & m] << 16;
    return (v * 2654435761u) >> (32 - HBITS);
}

// Compress pending bytes. If `flush`, consume everything; otherwise keep MAXMATCH bytes lookahead.
static void lz_run(enc_t *e, piece_t *pc, int flush) {
    uint32_t m = e->rmask;
    while (e->rpos < e->rend) {
        uint32_t avail = e->rend - e->rpos;
        if (!flush && avail < MAXMATCH) break;
        int best = 0; uint32_t bdist = 0;
        if (avail >= 3) {
            uint32_t h = hash3(e->ring, m, e->rpos);
            uint32_t cand = e->head[h];                 // absolute pos + 1
            e->head[h] = e->rpos + 1;
            if (cand) {
                cand--;
                uint32_t dist = e->rpos - cand;
                if (dist > 0 && dist <= EM_PNG_ENC_WIN) {
                    uint32_t maxl = avail < MAXMATCH ? avail : MAXMATCH, l = 0;
                    while (l < maxl && e->ring[(cand + l) & m] == e->ring[(e->rpos + l) & m]) l++;
                    if (l >= 3) { best = (int)l; bdist = dist; }
                }
            }
        }
        if (best) {
            put_match(e, pc, best, (int)bdist);
            for (int i = 1; i < best; i++) {            // keep the hash table fed
                uint32_t pos = e->rpos + (uint32_t)i;
                if (e->rend - pos >= 3) e->head[hash3(e->ring, m, pos)] = pos + 1;
            }
            e->rpos += (uint32_t)best;
        } else { put_lit(e, pc, e->ring[e->rpos & m]); e->rpos++; }
    }
}

static void enc_feed(enc_t *e, piece_t *pc, const uint8_t *d, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        e->ring[e->rend++ & e->rmask] = d[i];
        e->adler_a += d[i]; if (e->adler_a >= 65521) e->adler_a -= 65521;
        e->adler_b += e->adler_a; if (e->adler_b >= 65521) e->adler_b -= 65521;
    }
    lz_run(e, pc, 0);
}

em_err_t em_png_encode(em_io_t *out, int w, int h, em_src_cb src, void *user) {
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || !src) return EM_ERR_ARG;
    uint32_t rowbytes = (uint32_t)w * 3;
    uint32_t rsz = 1; while (rsz < EM_PNG_ENC_WIN + rowbytes + 1 + MAXMATCH + 64) rsz <<= 1;
    size_t total = sizeof(enc_t) + sizeof(piece_t) + rsz + (sizeof(uint32_t) << HBITS) + (size_t)w * 2 + 5 * (size_t)rowbytes + 8;
    uint8_t *mem = em_alloc(total, EM_MEM_BIG);
    if (!mem) return EM_ERR_NOMEM;
    memset(mem, 0, sizeof(enc_t) + sizeof(piece_t));
    enc_t *e = (enc_t *)mem; piece_t *pc = (piece_t *)(e + 1);
    e->io = out; e->adler_a = 1; e->adler_b = 0; e->rmask = rsz - 1;
    uint8_t *q = (uint8_t *)(pc + 1);
    e->ring = q; q += rsz;
    e->head = (uint32_t *)(((uintptr_t)q + 3) & ~(uintptr_t)3); q = (uint8_t *)(e->head + (1u << HBITS));
    memset(e->head, 0, sizeof(uint32_t) << HBITS);
    uint16_t *line = (uint16_t *)(((uintptr_t)q + 1) & ~(uintptr_t)1); q = (uint8_t *)(line + w);
    uint8_t *raw = q, *prev = raw + rowbytes, *cand = prev + rowbytes;   // cand: 3 filtered candidates, rowbytes each
    memset(prev, 0, rowbytes);

    em_err_t rc = EM_OK;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    if (io_write_all(out, sig, 8)) { rc = EM_ERR_IO; goto done; }
    uint8_t ihdr[13]; wr32be(ihdr, (uint32_t)w); wr32be(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk_write(e, "IHDR", ihdr, 13);
    // zlib header + one fixed-Huffman deflate stream (BFINAL=1, BTYPE=01)
    put_byte(e, pc, 0x78); put_byte(e, pc, 0x01);
    put_bits(e, pc, 1, 1); put_bits(e, pc, 1, 2);

    for (int y = 0; y < h; y++) {
        if (src(user, y, 1, line)) { rc = EM_ERR_ABORT; goto done; }
        for (int x = 0; x < w; x++) {
            uint16_t v = line[x];
            raw[x * 3] = (uint8_t)(((v >> 11) & 31) << 3 | ((v >> 11) & 31) >> 2);
            raw[x * 3 + 1] = (uint8_t)(((v >> 5) & 63) << 2 | ((v >> 5) & 63) >> 4);
            raw[x * 3 + 2] = (uint8_t)((v & 31) << 3 | (v & 31) >> 2);
        }
        // choose the cheapest of None / Sub / Up by sum of absolute signed values
        uint8_t *sub = cand, *up = cand + rowbytes;
        uint32_t s0 = 0, s1 = 0, s2 = 0;
        for (uint32_t i = 0; i < rowbytes; i++) {
            int a = i >= 3 ? raw[i - 3] : 0;
            sub[i] = (uint8_t)(raw[i] - a); up[i] = (uint8_t)(raw[i] - prev[i]);
            s0 += (uint32_t)abs((int8_t)raw[i]); s1 += (uint32_t)abs((int8_t)sub[i]); s2 += (uint32_t)abs((int8_t)up[i]);
        }
        uint8_t ft = 0; const uint8_t *best = raw;
        if (s1 < s0 && s1 <= s2) { ft = 1; best = sub; } else if (s2 < s0) { ft = 2; best = up; }
        enc_feed(e, pc, &ft, 1);
        enc_feed(e, pc, best, rowbytes);
        memcpy(prev, raw, rowbytes);
        if (e->err) { rc = EM_ERR_IO; goto done; }
    }
    lz_run(e, pc, 1);
    put_huff(e, pc, 0, 7);                                   // end-of-block (256)
    if (e->bitcnt) put_bits(e, pc, 0, 8 - e->bitcnt);
    {
        uint32_t ad = (e->adler_b << 16) | e->adler_a;
        put_byte(e, pc, (uint8_t)(ad >> 24)); put_byte(e, pc, (uint8_t)(ad >> 16));
        put_byte(e, pc, (uint8_t)(ad >> 8)); put_byte(e, pc, (uint8_t)ad);
    }
    if (pc->plen) chunk_write(e, "IDAT", pc->piece, pc->plen);
    chunk_write(e, "IEND", NULL, 0);
    if (e->err) rc = EM_ERR_IO;
done:
    em_free(mem);
    return rc;
}
