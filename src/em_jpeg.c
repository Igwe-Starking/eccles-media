// em_jpeg.c - baseline JPEG decoder (streaming MCU rows, 1/1..1/8 scaling) and
// baseline encoder (standard tables, quality scaling). Integer-only, no FPU,
// no tables bigger than ~3KB. Integer IDCT/FDCT are the classic Loeffler
// 13-bit "islow" algorithms (same maths as libjpeg's default).
#include "em_priv.h"
#include <stdlib.h>

static const uint8_t k_zz[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

#define CB 13
#define P1 2
#define F_0_298 2446
#define F_0_390 3196
#define F_0_541 4433
#define F_0_765 6270
#define F_0_899 7373
#define F_1_175 9633
#define F_1_501 12299
#define F_1_847 15137
#define F_1_961 16069
#define F_2_053 16819
#define F_2_562 20995
#define F_3_072 25172
#define DESC(x, n) (((x) + (1 << ((n) - 1))) >> (n))

// ---- inverse DCT (dequantised int32 coefficients, natural order) -> 8x8 bytes -------
static void idct8(const int32_t *in, uint8_t *out, int stride) {
    int32_t ws[64];
    for (int c = 0; c < 8; c++) {
        const int32_t *p = in + c; int32_t *w = ws + c;
        if (!(p[8] | p[16] | p[24] | p[32] | p[40] | p[48] | p[56])) {
            int32_t dc = p[0] << P1;
            for (int r = 0; r < 8; r++) w[r * 8] = dc;
            continue;
        }
        int32_t z2 = p[16], z3 = p[48];
        int32_t z1 = (z2 + z3) * F_0_541;
        int32_t t2 = z1 + z3 * -F_1_847, t3 = z1 + z2 * F_0_765;
        int32_t t0 = (p[0] + p[32]) << CB, t1 = (p[0] - p[32]) << CB;
        int32_t t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
        t0 = p[56]; t1 = p[40]; t2 = p[24]; t3 = p[8];
        z1 = t0 + t3; z2 = t1 + t2; z3 = t0 + t2; int32_t z4 = t1 + t3, z5 = (z3 + z4) * F_1_175;
        t0 *= F_0_298; t1 *= F_2_053; t2 *= F_3_072; t3 *= F_1_501;
        z1 *= -F_0_899; z2 *= -F_2_562; z3 *= -F_1_961; z4 *= -F_0_390;
        z3 += z5; z4 += z5;
        t0 += z1 + z3; t1 += z2 + z4; t2 += z2 + z3; t3 += z1 + z4;
        w[0] = DESC(t10 + t3, CB - P1); w[56] = DESC(t10 - t3, CB - P1);
        w[8] = DESC(t11 + t2, CB - P1); w[48] = DESC(t11 - t2, CB - P1);
        w[16] = DESC(t12 + t1, CB - P1); w[40] = DESC(t12 - t1, CB - P1);
        w[24] = DESC(t13 + t0, CB - P1); w[32] = DESC(t13 - t0, CB - P1);
    }
    for (int r = 0; r < 8; r++) {
        const int32_t *p = ws + r * 8; uint8_t *o = out + r * stride;
        int32_t z2 = p[2], z3 = p[6];
        int32_t z1 = (z2 + z3) * F_0_541;
        int32_t t2 = z1 + z3 * -F_1_847, t3 = z1 + z2 * F_0_765;
        int32_t t0 = (p[0] + p[4]) << CB, t1 = (p[0] - p[4]) << CB;
        int32_t t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
        t0 = p[7]; t1 = p[5]; t2 = p[3]; t3 = p[1];
        z1 = t0 + t3; z2 = t1 + t2; z3 = t0 + t2; int32_t z4 = t1 + t3, z5 = (z3 + z4) * F_1_175;
        t0 *= F_0_298; t1 *= F_2_053; t2 *= F_3_072; t3 *= F_1_501;
        z1 *= -F_0_899; z2 *= -F_2_562; z3 *= -F_1_961; z4 *= -F_0_390;
        z3 += z5; z4 += z5;
        t0 += z1 + z3; t1 += z2 + z4; t2 += z2 + z3; t3 += z1 + z4;
        const int S = CB + P1 + 3;
        o[0] = (uint8_t)clamp8(DESC(t10 + t3, S) + 128); o[7] = (uint8_t)clamp8(DESC(t10 - t3, S) + 128);
        o[1] = (uint8_t)clamp8(DESC(t11 + t2, S) + 128); o[6] = (uint8_t)clamp8(DESC(t11 - t2, S) + 128);
        o[2] = (uint8_t)clamp8(DESC(t12 + t1, S) + 128); o[5] = (uint8_t)clamp8(DESC(t12 - t1, S) + 128);
        o[3] = (uint8_t)clamp8(DESC(t13 + t0, S) + 128); o[4] = (uint8_t)clamp8(DESC(t13 - t0, S) + 128);
    }
}

// ---- decoder ------------------------------------------------------------------------------
typedef struct {
    uint16_t lut[256];          // (len<<8)|sym for codes <= 8 bits
    int32_t  maxcode[18], valoff[17];
    uint8_t  vals[256];
    uint8_t  ok;
} hdec_t;

struct em_jpeg {
    em_io_t *io;
    uint16_t w, h; uint8_t ncomp, subsamp, hmax, vmax;
    uint8_t hs[3], vs[3], tq[3], td[3], ta[3], cid[3];
    uint16_t qt[4][64];         // zigzag order
    uint8_t  qok[4];
    hdec_t dc[4], ac[4];
    uint16_t restart;
    uint32_t scan_off; int decoded_once;
    uint8_t buf[512]; int bpos, blen; uint32_t consumed, base;
    uint32_t bits; int nbits; int marker; int eof;
    uint8_t *plane[3]; uint32_t pstride[3];
    uint16_t *band[2];
    uint8_t *mem; size_t mem_sz;
};

static int jb_next(em_jpeg_t *j) {                 // raw byte, -1 at EOF
    if (j->bpos >= j->blen) {
        int n = j->io->read(j->io->ctx, j->buf, sizeof j->buf);
        if (n <= 0) { j->eof = 1; return -1; }
        j->blen = n; j->bpos = 0;
    }
    j->consumed++;
    return j->buf[j->bpos++];
}
static int jb_u16(em_jpeg_t *j) { int a = jb_next(j), b = jb_next(j); return (a < 0 || b < 0) ? -1 : (a << 8 | b); }

// entropy-coded bit reader (MSB-first, handles FF00 stuffing and markers)
static void bits_fill(em_jpeg_t *j) {
    while (j->nbits <= 24) {
        int b = 0;
        if (!j->marker) {
            b = jb_next(j);
            if (b < 0) { b = 0; j->marker = 0xD9; }
            else if (b == 0xFF) {
                int c;
                do { c = jb_next(j); } while (c == 0xFF);
                if (c == 0) b = 0xFF;
                else { j->marker = c < 0 ? 0xD9 : c; b = 0; }
            }
        }
        j->bits |= (uint32_t)b << (24 - j->nbits);
        j->nbits += 8;
    }
}
static inline int bits_get(em_jpeg_t *j, int n) {
    if (!n) return 0;
    if (j->nbits < n) bits_fill(j);
    int v = (int)(j->bits >> (32 - n));
    j->bits <<= n; j->nbits -= n;
    return v;
}
static inline int huff_sym(em_jpeg_t *j, const hdec_t *h) {
    if (j->nbits < 16) bits_fill(j);
    uint16_t e = h->lut[j->bits >> 24];
    if (e) { int l = e >> 8; j->bits <<= l; j->nbits -= l; return e & 0xFF; }
    uint32_t code16 = j->bits >> 16;
    for (int l = 9; l <= 16; l++) {
        int32_t c = (int32_t)(code16 >> (16 - l));
        if (c <= h->maxcode[l]) { j->bits <<= l; j->nbits -= l; return h->vals[(h->valoff[l] + c) & 255]; }
    }
    return -1;
}
static inline int extend(int v, int s) { return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v; }

static int build_huff(hdec_t *h, const uint8_t *cnt, const uint8_t *vals, int nvals) {
    memset(h, 0, sizeof *h);
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        int n = cnt[l - 1];
        h->valoff[l] = k - code;
        if (n) {
            h->maxcode[l] = code + n - 1;
            if (l <= 8)
                for (int i = 0; i < n; i++) {
                    int c = (code + i) << (8 - l);
                    if (c + (1 << (8 - l)) > 256) return -1;
                    for (int f = 0; f < (1 << (8 - l)); f++) h->lut[c + f] = (uint16_t)(l << 8 | vals[k + i]);
                }
        } else h->maxcode[l] = -1;
        k += n; code += n;
        if (code > (1 << l)) return -1;
        code <<= 1;
    }
    if (k != nvals || k > 256) return -1;
    memcpy(h->vals, vals, (size_t)k);
    h->ok = 1;
    return 0;
}

static em_err_t jpeg_parse(em_jpeg_t *j, em_io_t *io, em_img_info_t *info) {
    j->io = io; j->base = io->tell ? io->tell(io->ctx) : 0;
    if (jb_u16(j) != 0xFFD8) return EM_ERR_FORMAT;
    int got_sof = 0, got_sos = 0;
    while (!got_sos) {
        int b = jb_next(j);
        if (b < 0) goto bad;
        if (b != 0xFF) continue;
        int m; do { m = jb_next(j); } while (m == 0xFF);
        if (m < 0) goto bad;
        if (m == 0 || m == 0x01 || (m >= 0xD0 && m <= 0xD8)) continue;
        if (m == 0xD9) goto bad;
        int len = jb_u16(j);
        if (len < 2) goto bad;
        len -= 2;
        uint8_t t[272];
        if (m == 0xDB) {
            while (len > 0) {
                int pq = jb_next(j); if (pq < 0) goto bad; len--;
                int id = pq & 15, prec = pq >> 4;
                if (id > 3 || prec > 1) goto bad;
                for (int i = 0; i < 64; i++) {
                    int v = prec ? jb_u16(j) : jb_next(j);
                    if (v <= 0) goto bad;
                    j->qt[id][i] = (uint16_t)v; len -= prec ? 2 : 1;
                }
                j->qok[id] = 1;
            }
        } else if (m == 0xC4) {
            while (len > 17) {
                int tc = jb_next(j); if (tc < 0) goto bad;
                for (int i = 0; i < 16; i++) { int c = jb_next(j); if (c < 0) goto bad; t[i] = (uint8_t)c; }
                int n = 0; for (int i = 0; i < 16; i++) n += t[i];
                if (n > 256 || n > len - 17) goto bad;
                for (int i = 0; i < n; i++) { int c = jb_next(j); if (c < 0) goto bad; t[16 + i] = (uint8_t)c; }
                len -= 17 + n;
                int id = tc & 3, cls = tc >> 4;
                if ((tc & 0xEC) || cls > 1) goto bad;
                if (build_huff(cls ? &j->ac[id] : &j->dc[id], t, t + 16, n)) goto bad;
            }
            if (len) goto bad;
        } else if (m == 0xC0 || m == 0xC1) {
            if (got_sof || len < 6) goto bad;
            int prec = jb_next(j), hh = jb_u16(j), ww = jb_u16(j), nc = jb_next(j);
            if (prec != 8 || hh <= 0 || ww <= 0 || (nc != 1 && nc != 3) || len != 6 + 3 * nc) goto bad;
            j->w = (uint16_t)ww; j->h = (uint16_t)hh; j->ncomp = (uint8_t)nc;
            for (int i = 0; i < nc; i++) {
                int id = jb_next(j), hv = jb_next(j), q = jb_next(j);
                if (id < 0 || hv < 0 || q < 0 || q > 3) goto bad;
                j->cid[i] = (uint8_t)id; j->hs[i] = (uint8_t)(hv >> 4); j->vs[i] = (uint8_t)(hv & 15); j->tq[i] = (uint8_t)q;
            }
            if (nc == 1) { j->hs[0] = j->vs[0] = 1; }
            int hm = 1, vm = 1;
            for (int c = 0; c < nc; c++) {
                if (j->hs[c] < 1 || j->hs[c] > 2 || j->vs[c] < 1 || j->vs[c] > 2) return EM_ERR_UNSUPPORTED;
                if (j->hs[c] > hm) hm = j->hs[c];
                if (j->vs[c] > vm) vm = j->vs[c];
            }
            for (int c = 0; c < nc; c++) if (hm % j->hs[c] || vm % j->vs[c]) return EM_ERR_UNSUPPORTED;
            j->hmax = (uint8_t)hm; j->vmax = (uint8_t)vm;
            if (nc == 1) j->subsamp = 0;
            else {
                int sx = hm / j->hs[1], sy = vm / j->vs[1];
                j->subsamp = (sx == 1 && sy == 1) ? 1 : (sx == 2 && sy == 1) ? 2 : (sx == 2 && sy == 2) ? 3 : 4;
            }
            got_sof = 1;
        } else if (m == 0xC2 || (m >= 0xC3 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC)) {
            return EM_ERR_UNSUPPORTED;               // progressive / lossless / arithmetic
        } else if (m == 0xDD) {
            if (len != 2) goto bad;
            j->restart = (uint16_t)jb_u16(j);
        } else if (m == 0xDA) {
            if (!got_sof) goto bad;
            int ns = jb_next(j);
            if (ns != j->ncomp || len != 4 + 2 * ns) return EM_ERR_UNSUPPORTED;   // non-interleaved scans
            for (int i = 0; i < ns; i++) {
                int cs = jb_next(j), tt = jb_next(j);
                if (cs < 0 || tt < 0) goto bad;
                int k = -1; for (int c = 0; c < ns; c++) if (j->cid[c] == cs) k = c;
                if (k < 0) goto bad;
                j->td[k] = (uint8_t)(tt >> 4); j->ta[k] = (uint8_t)(tt & 15);
                if (j->td[k] > 3 || j->ta[k] > 3) goto bad;
            }
            jb_next(j); jb_next(j); jb_next(j);                   // Ss Se Ah/Al
            got_sos = 1;
        } else {
            for (int i = 0; i < len; i++) if (jb_next(j) < 0) goto bad;
        }
    }
    for (int c = 0; c < j->ncomp; c++)
        if (!j->qok[j->tq[c]] || !j->dc[j->td[c]].ok || !j->ac[j->ta[c]].ok) goto bad;
    j->scan_off = j->base + j->consumed;
    if (info) { info->width = j->w; info->height = j->h; info->subsamp = j->subsamp; }
    return EM_OK;
bad:
    return EM_ERR_FORMAT;
}

em_err_t em_jpeg_open(em_jpeg_t **out, em_io_t *io, em_img_info_t *info) {
    em_jpeg_t *j = em_alloc(sizeof *j, EM_MEM_FAST);
    if (!j) return EM_ERR_NOMEM;
    memset(j, 0, sizeof *j);
    em_err_t e = jpeg_parse(j, io, info);
    if (e) { em_free(j); return e; }
    *out = j;
    return EM_OK;
}

em_err_t em_jpeg_reset(em_jpeg_t *j, em_io_t *io, em_img_info_t *info) {
    uint8_t *mem = j->mem; size_t sz = j->mem_sz;
    memset(j, 0, sizeof *j);
    j->mem = mem; j->mem_sz = sz;
    return jpeg_parse(j, io, info);
}


void em_jpeg_close(em_jpeg_t *j) { if (!j) return; em_free(j->mem); em_free(j); }

static void restart_sync(em_jpeg_t *j) {
    // discard partial bits, find the RSTn marker, resume
    j->bits = 0; j->nbits = 0;
    if (!j->marker) {
        int b, guard = 0;
        for (;;) {
            b = jb_next(j);
            if (b < 0 || ++guard > 1 << 16) { j->marker = 0xD9; break; }
            if (b == 0xFF) {
                int c; do { c = jb_next(j); } while (c == 0xFF);
                if (c >= 0xD0 && c <= 0xD7) { j->marker = c; break; }
                if (c != 0) { j->marker = c < 0 ? 0xD9 : c; break; }
            }
        }
    }
    if (j->marker >= 0xD0 && j->marker <= 0xD7) j->marker = 0;     // consumed
}

static int dec_block(em_jpeg_t *j, int c, int *pred, int32_t *coef, int *maxk) {
    const hdec_t *dc = &j->dc[j->td[c]], *ac = &j->ac[j->ta[c]];
    const uint16_t *q = j->qt[j->tq[c]];
    memset(coef, 0, 64 * sizeof(int32_t));
    int s = huff_sym(j, dc);
    if (s < 0 || s > 11) return -1;
    int diff = s ? extend(bits_get(j, s), s) : 0;
    *pred += diff;
    coef[0] = *pred * q[0];
    int k = 1;
    while (k < 64) {
        int rs = huff_sym(j, ac);
        if (rs < 0) return -1;
        int r = rs >> 4; s = rs & 15;
        if (!s) { if (r == 15) { k += 16; continue; } break; }
        k += r;
        if (k > 63) return -1;
        coef[k_zz[k]] = extend(bits_get(j, s), s) * q[k];
        k++;
    }
    *maxk = k;
    return 0;
}

em_err_t em_jpeg_decode(em_jpeg_t *j, int sh, em_rows_cb cb, void *user) {
    if (!j || !cb || sh < 0 || sh > 3) return EM_ERR_ARG;
    int bs = 8 >> sh;
    int mcu_w = 8 * j->hmax, mcu_h = 8 * j->vmax;
    int mcus_x = (j->w + mcu_w - 1) / mcu_w, mcus_y = (j->h + mcu_h - 1) / mcu_h;
    int out_w = (j->w + (1 << sh) - 1) >> sh, out_h = (j->h + (1 << sh) - 1) >> sh;
    int band_lines = (mcu_h) >> sh;
    if (j->decoded_once) {
        if (!j->io->seek || j->io->seek(j->io->ctx, j->scan_off)) return EM_ERR_IO;
        j->bpos = j->blen = 0;
    }
    j->decoded_once = 1;
    j->bits = 0; j->nbits = 0; j->marker = 0; j->eof = 0;

    // one allocation: planes + two RGB565 bands
    size_t psz[3] = {0, 0, 0};
    for (int c = 0; c < j->ncomp; c++) {
        j->pstride[c] = (uint32_t)(mcus_x * j->hs[c] * bs);
        psz[c] = (size_t)j->pstride[c] * (size_t)(j->vs[c] * bs);
    }
    size_t band_sz = (size_t)out_w * band_lines * 2;
    size_t need = psz[0] + psz[1] + psz[2] + 2 * band_sz + 8;
    if (!j->mem || j->mem_sz < need) {
        em_free(j->mem); j->mem = em_alloc(need, EM_MEM_BIG); j->mem_sz = need;
        if (!j->mem) { j->mem_sz = 0; return EM_ERR_NOMEM; }
    }
    uint8_t *q = j->mem;
    for (int c = 0; c < 3; c++) { j->plane[c] = psz[c] ? q : NULL; q += psz[c]; }
    q = (uint8_t *)(((uintptr_t)q + 1) & ~(uintptr_t)1);
    j->band[0] = (uint16_t *)q; j->band[1] = (uint16_t *)(q + band_sz);

    int32_t coef[64]; uint8_t tmp[64];
    int pred[3] = {0, 0, 0};
    int rst_count = 0, which = 0;
    int xsh[3], ysh[3];
    for (int c = 0; c < 3; c++) { xsh[c] = j->hs[c] && j->hmax / j->hs[c] == 2; ysh[c] = j->vs[c] && j->vmax / j->vs[c] == 2; }
    for (int my = 0; my < mcus_y; my++) {
        for (int mx = 0; mx < mcus_x; mx++) {
            if (j->restart && rst_count == j->restart) { restart_sync(j); pred[0] = pred[1] = pred[2] = 0; rst_count = 0; }
            rst_count++;
            for (int c = 0; c < j->ncomp; c++)
                for (int by = 0; by < j->vs[c]; by++)
                    for (int bx = 0; bx < j->hs[c]; bx++) {
                        int maxk = 0;
                        if (dec_block(j, c, &pred[c], coef, &maxk)) return EM_ERR_FORMAT;
                        uint8_t *dst = j->plane[c] + (size_t)(by * bs) * j->pstride[c] + (size_t)(mx * j->hs[c] + bx) * bs;
                        uint32_t st = j->pstride[c];
                        if (sh == 3) { dst[0] = (uint8_t)clamp8(DESC(coef[0], 3) + 128); continue; }
                        if (maxk <= 1) {
                            uint8_t v = (uint8_t)clamp8(DESC(coef[0], 3) + 128);
                            for (int r = 0; r < bs; r++) memset(dst + r * st, v, (size_t)bs);
                            continue;
                        }
                        if (sh == 0) { idct8(coef, dst, (int)st); continue; }
                        idct8(coef, tmp, 8);
                        int f = 1 << sh, area = f * f;
                        for (int r = 0; r < bs; r++)
                            for (int x = 0; x < bs; x++) {
                                int s = 0;
                                for (int yy = 0; yy < f; yy++) for (int xx = 0; xx < f; xx++) s += tmp[(r * f + yy) * 8 + x * f + xx];
                                dst[r * st + x] = (uint8_t)((s + area / 2) / area);
                            }
                    }
        }
        if (j->eof && j->marker == 0xD9 && my < mcus_y - 1 && 0) return EM_ERR_FORMAT;
        // ---- colour convert this MCU row into the current band ----
        int y0 = my * band_lines;
        int lines = band_lines; if (y0 + lines > out_h) lines = out_h - y0;
        uint16_t *bnd = j->band[which];
        for (int ly = 0; ly < lines; ly++) {
            const uint8_t *py = j->plane[0] + (size_t)(ysh[0] ? ly >> 1 : ly) * j->pstride[0];
            uint16_t *o = bnd + (size_t)ly * out_w;
            if (j->ncomp == 1) {
                for (int x = 0; x < out_w; x++) { int v = py[x]; o[x] = pack565(v, v, v); }
            } else {
                const uint8_t *pcb = j->plane[1] + (size_t)(ysh[1] ? ly >> 1 : ly) * j->pstride[1];
                const uint8_t *pcr = j->plane[2] + (size_t)(ysh[2] ? ly >> 1 : ly) * j->pstride[2];
                for (int x = 0; x < out_w; x++) {
                    int Y = py[xsh[0] ? x >> 1 : x], cb = pcb[xsh[1] ? x >> 1 : x] - 128, cr = pcr[xsh[2] ? x >> 1 : x] - 128;
                    int r = Y + ((91881 * cr + 32768) >> 16);
                    int g = Y - ((22554 * cb + 46802 * cr + 32768) >> 16);
                    int b = Y + ((116130 * cb + 32768) >> 16);
                    o[x] = pack565(clamp8(r), clamp8(g), clamp8(b));
                }
            }
        }
        if (cb(user, y0, lines, bnd, out_w)) return EM_ERR_ABORT;
        which ^= 1;
    }
    return EM_OK;
}

// ---- encoder -------------------------------------------------------------------------------------
static const uint8_t k_dc_l_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t k_dc_c_bits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t k_dc_vals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t k_ac_l_bits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t k_ac_l_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
static const uint8_t k_ac_c_bits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const uint8_t k_ac_c_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
static const uint8_t k_q_lum[64] = {
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
static const uint8_t k_q_chr[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

typedef struct { uint16_t code[256]; uint8_t len[256]; } henc_t;

static void build_henc(henc_t *h, const uint8_t *bits, const uint8_t *vals) {
    memset(h, 0, sizeof *h);
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < bits[l - 1]; i++, k++) { h->code[vals[k]] = (uint16_t)code++; h->len[vals[k]] = (uint8_t)l; }
        code <<= 1;
    }
}

typedef struct { em_io_t *io; uint8_t buf[512]; int n; uint32_t acc; int nb; int err; } jw_t;
static void jw_flush(jw_t *w) { if (w->n && io_write_all(w->io, w->buf, (uint32_t)w->n)) w->err = 1; w->n = 0; }
static void jw_byte(jw_t *w, int b) { if (w->n == (int)sizeof w->buf) jw_flush(w); w->buf[w->n++] = (uint8_t)b; }
static void jw_bits(jw_t *w, int v, int n) {
    if (!n) return;
    w->acc = (w->acc << n) | ((uint32_t)v & ((1u << n) - 1)); w->nb += n;
    while (w->nb >= 8) { int b = (int)((w->acc >> (w->nb - 8)) & 0xFF); jw_byte(w, b); if (b == 0xFF) jw_byte(w, 0); w->nb -= 8; }
}
static void jw_align(jw_t *w) { if (w->nb) jw_bits(w, 0x7F, 8 - w->nb); }
static void jw_u16(jw_t *w, int v) { jw_byte(w, v >> 8); jw_byte(w, v & 255); }

static void fdct8(const uint8_t *in, int stride, int32_t *out) {
    int32_t ws[64];
    for (int r = 0; r < 8; r++) {
        const uint8_t *p = in + r * stride; int32_t *o = ws + r * 8;
        int32_t d0 = p[0] - 128, d1 = p[1] - 128, d2 = p[2] - 128, d3 = p[3] - 128, d4 = p[4] - 128, d5 = p[5] - 128, d6 = p[6] - 128, d7 = p[7] - 128;
        int32_t t0 = d0 + d7, t7 = d0 - d7, t1 = d1 + d6, t6 = d1 - d6, t2 = d2 + d5, t5 = d2 - d5, t3 = d3 + d4, t4 = d3 - d4;
        int32_t t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
        o[0] = (t10 + t11) << P1; o[4] = (t10 - t11) << P1;
        int32_t z1 = (t12 + t13) * F_0_541;
        o[2] = DESC(z1 + t13 * F_0_765, CB - P1); o[6] = DESC(z1 + t12 * -F_1_847, CB - P1);
        z1 = t4 + t7; int32_t z2 = t5 + t6, z3 = t4 + t6, z4 = t5 + t7, z5 = (z3 + z4) * F_1_175;
        t4 *= F_0_298; t5 *= F_2_053; t6 *= F_3_072; t7 *= F_1_501;
        z1 *= -F_0_899; z2 *= -F_2_562; z3 *= -F_1_961; z4 *= -F_0_390;
        z3 += z5; z4 += z5;
        o[7] = DESC(t4 + z1 + z3, CB - P1); o[5] = DESC(t5 + z2 + z4, CB - P1);
        o[3] = DESC(t6 + z2 + z3, CB - P1); o[1] = DESC(t7 + z1 + z4, CB - P1);
    }
    for (int c = 0; c < 8; c++) {
        const int32_t *p = ws + c; int32_t *o = out + c;
        int32_t t0 = p[0] + p[56], t7 = p[0] - p[56], t1 = p[8] + p[48], t6 = p[8] - p[48], t2 = p[16] + p[40], t5 = p[16] - p[40], t3 = p[24] + p[32], t4 = p[24] - p[32];
        int32_t t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
        o[0] = DESC(t10 + t11, P1); o[32] = DESC(t10 - t11, P1);
        int32_t z1 = (t12 + t13) * F_0_541;
        o[16] = DESC(z1 + t13 * F_0_765, CB + P1); o[48] = DESC(z1 + t12 * -F_1_847, CB + P1);
        z1 = t4 + t7; int32_t z2 = t5 + t6, z3 = t4 + t6, z4 = t5 + t7, z5 = (z3 + z4) * F_1_175;
        t4 *= F_0_298; t5 *= F_2_053; t6 *= F_3_072; t7 *= F_1_501;
        z1 *= -F_0_899; z2 *= -F_2_562; z3 *= -F_1_961; z4 *= -F_0_390;
        z3 += z5; z4 += z5;
        o[56] = DESC(t4 + z1 + z3, CB + P1); o[40] = DESC(t5 + z2 + z4, CB + P1);
        o[24] = DESC(t6 + z2 + z3, CB + P1); o[8] = DESC(t7 + z1 + z4, CB + P1);
    }
}

static void enc_block(jw_t *w, const uint8_t *src, int stride, const uint16_t *qz, int *pred, const henc_t *dc, const henc_t *ac) {
    int32_t f[64];
    fdct8(src, stride, f);
    int zz[64];
    for (int k = 0; k < 64; k++) {
        int v = f[k_zz[k]], d = qz[k] << 3;
        zz[k] = v >= 0 ? (v + d / 2) / d : -((-v + d / 2) / d);
    }
    int diff = zz[0] - *pred; *pred = zz[0];
    int a = diff < 0 ? -diff : diff, s = 0; while (a >> s) s++;
    jw_bits(w, dc->code[s], dc->len[s]);
    if (s) jw_bits(w, diff < 0 ? diff - 1 : diff, s);
    int run = 0;
    for (int k = 1; k < 64; k++) {
        int v = zz[k];
        if (!v) { run++; continue; }
        while (run > 15) { jw_bits(w, ac->code[0xF0], ac->len[0xF0]); run -= 16; }
        a = v < 0 ? -v : v; s = 0; while (a >> s) s++;
        int sym = run << 4 | s;
        jw_bits(w, ac->code[sym], ac->len[sym]);
        jw_bits(w, v < 0 ? v - 1 : v, s);
        run = 0;
    }
    if (run) jw_bits(w, ac->code[0], ac->len[0]);
}

static void put_dqt(jw_t *w, int id, const uint16_t *qz) {
    jw_byte(w, 0xFF); jw_byte(w, 0xDB); jw_u16(w, 67); jw_byte(w, id);
    for (int k = 0; k < 64; k++) jw_byte(w, qz[k]);
}
static void put_dht(jw_t *w, int tc_id, const uint8_t *bits, const uint8_t *vals, int n) {
    jw_byte(w, 0xFF); jw_byte(w, 0xC4); jw_u16(w, 19 + n); jw_byte(w, tc_id);
    for (int i = 0; i < 16; i++) jw_byte(w, bits[i]);
    for (int i = 0; i < n; i++) jw_byte(w, vals[i]);
}

em_err_t em_jpeg_encode(em_io_t *out, int W, int H, int quality, int subsamp, em_src_cb src, void *user) {
    if (W <= 0 || H <= 0 || W > 65535 || H > 65535 || !src || (subsamp != 0 && subsamp != 1 && subsamp != 3)) return EM_ERR_ARG;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    int hs = subsamp == 3 ? 2 : 1, vs = subsamp == 3 ? 2 : 1;
    int mcu_w = 8 * hs, mcu_h = 8 * vs;
    int mcus_x = (W + mcu_w - 1) / mcu_w, mcus_y = (H + mcu_h - 1) / mcu_h;
    int pw = mcus_x * mcu_w;                                   // padded width
    size_t line_sz = (size_t)W * mcu_h * 2;
    size_t ysz = (size_t)pw * mcu_h, csz = subsamp ? (size_t)(pw / hs) * 8 : 0;
    size_t total = sizeof(jw_t) + 2 * sizeof(henc_t) * 2 + line_sz + ysz + 2 * csz + 16;
    uint8_t *mem = em_alloc(total, EM_MEM_BIG);
    if (!mem) return EM_ERR_NOMEM;
    jw_t *w = (jw_t *)mem; memset(w, 0, sizeof *w); w->io = out;
    henc_t *dcl = (henc_t *)(w + 1), *dcc = dcl + 1, *acl = dcc + 1, *acc = acl + 1;
    uint16_t *line = (uint16_t *)(acc + 1);
    uint8_t *Y = (uint8_t *)line + line_sz, *Cb = Y + ysz, *Cr = Cb + csz;
    build_henc(dcl, k_dc_l_bits, k_dc_vals); build_henc(dcc, k_dc_c_bits, k_dc_vals);
    build_henc(acl, k_ac_l_bits, k_ac_l_vals); build_henc(acc, k_ac_c_bits, k_ac_c_vals);
    uint16_t ql[64], qc[64];
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for (int k = 0; k < 64; k++) {
        int a = (k_q_lum[k] * scale + 50) / 100, b = (k_q_chr[k] * scale + 50) / 100;
        ql[k] = (uint16_t)(a < 1 ? 1 : a > 255 ? 255 : a); qc[k] = (uint16_t)(b < 1 ? 1 : b > 255 ? 255 : b);
    }
    // headers (tables are in zigzag order in the file; k_q_* are natural order, so permute)
    uint16_t qlz[64], qcz[64];
    for (int k = 0; k < 64; k++) { qlz[k] = ql[k_zz[k]]; qcz[k] = qc[k_zz[k]]; }
    jw_byte(w, 0xFF); jw_byte(w, 0xD8);
    jw_byte(w, 0xFF); jw_byte(w, 0xE0); jw_u16(w, 16); jw_byte(w, 'J'); jw_byte(w, 'F'); jw_byte(w, 'I'); jw_byte(w, 'F'); jw_byte(w, 0);
    jw_byte(w, 1); jw_byte(w, 1); jw_byte(w, 0); jw_u16(w, 1); jw_u16(w, 1); jw_byte(w, 0); jw_byte(w, 0);
    put_dqt(w, 0, qlz); if (subsamp) put_dqt(w, 1, qcz);
    jw_byte(w, 0xFF); jw_byte(w, 0xC0); jw_u16(w, subsamp ? 17 : 11); jw_byte(w, 8); jw_u16(w, H); jw_u16(w, W); jw_byte(w, subsamp ? 3 : 1);
    jw_byte(w, 1); jw_byte(w, hs << 4 | vs); jw_byte(w, 0);
    if (subsamp) { jw_byte(w, 2); jw_byte(w, 0x11); jw_byte(w, 1); jw_byte(w, 3); jw_byte(w, 0x11); jw_byte(w, 1); }
    put_dht(w, 0x00, k_dc_l_bits, k_dc_vals, 12); put_dht(w, 0x10, k_ac_l_bits, k_ac_l_vals, 162);
    if (subsamp) { put_dht(w, 0x01, k_dc_c_bits, k_dc_vals, 12); put_dht(w, 0x11, k_ac_c_bits, k_ac_c_vals, 162); }
    jw_byte(w, 0xFF); jw_byte(w, 0xDA); jw_u16(w, subsamp ? 12 : 8); jw_byte(w, subsamp ? 3 : 1);
    jw_byte(w, 1); jw_byte(w, 0x00);
    if (subsamp) { jw_byte(w, 2); jw_byte(w, 0x11); jw_byte(w, 3); jw_byte(w, 0x11); }
    jw_byte(w, 0); jw_byte(w, 63); jw_byte(w, 0);

    int pred[3] = {0, 0, 0};
    em_err_t rc = EM_OK;
    for (int my = 0; my < mcus_y; my++) {
        int y0 = my * mcu_h, n = mcu_h; if (y0 + n > H) n = H - y0;
        if (src(user, y0, n, line)) { rc = EM_ERR_ABORT; goto done; }
        for (int ly = 0; ly < mcu_h; ly++) {
            const uint16_t *s = line + (size_t)(ly < n ? ly : n - 1) * W;       // replicate bottom edge
            uint8_t *yp = Y + (size_t)ly * pw;
            for (int x = 0; x < pw; x++) {
                uint16_t v = s[x < W ? x : W - 1];
                int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                r = r << 3 | r >> 2; g = g << 2 | g >> 4; b = b << 3 | b >> 2;
                yp[x] = (uint8_t)((19595 * r + 38470 * g + 7471 * b + 32768) >> 16);
                if (subsamp == 1) {
                    Cb[(size_t)ly * pw + x] = (uint8_t)clamp8(((-11059 * r - 21709 * g + 32768 * b + 32768) >> 16) + 128);
                    Cr[(size_t)ly * pw + x] = (uint8_t)clamp8(((32768 * r - 27439 * g - 5329 * b + 32768) >> 16) + 128);
                }
            }
        }
        if (subsamp == 3) {          // 4:2:0 : average 2x2 of the converted pixels
            for (int cy = 0; cy < 8; cy++)
                for (int cx = 0; cx < pw / 2; cx++) {
                    int sr = 0, sg = 0, sb = 0;
                    for (int dy = 0; dy < 2; dy++) {
                        int ly = cy * 2 + dy;
                        const uint16_t *s = line + (size_t)(ly < n ? ly : n - 1) * W;
                        for (int dx = 0; dx < 2; dx++) {
                            int x = cx * 2 + dx; uint16_t v = s[x < W ? x : W - 1];
                            int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                            sr += r << 3 | r >> 2; sg += g << 2 | g >> 4; sb += b << 3 | b >> 2;
                        }
                    }
                    int r = (sr + 2) >> 2, g = (sg + 2) >> 2, b = (sb + 2) >> 2;
                    Cb[cy * (pw / 2) + cx] = (uint8_t)clamp8(((-11059 * r - 21709 * g + 32768 * b + 32768) >> 16) + 128);
                    Cr[cy * (pw / 2) + cx] = (uint8_t)clamp8(((32768 * r - 27439 * g - 5329 * b + 32768) >> 16) + 128);
                }
        }
        for (int mx = 0; mx < mcus_x; mx++) {
            for (int by = 0; by < vs; by++)
                for (int bx = 0; bx < hs; bx++)
                    enc_block(w, Y + (size_t)(by * 8) * pw + (mx * hs + bx) * 8, pw, qlz, &pred[0], dcl, acl);
            if (subsamp) {
                int cs = subsamp == 3 ? pw / 2 : pw;
                enc_block(w, Cb + mx * 8, cs, qcz, &pred[1], dcc, acc);
                enc_block(w, Cr + mx * 8, cs, qcz, &pred[2], dcc, acc);
            }
        }
        if (w->err) { rc = EM_ERR_IO; goto done; }
    }
    jw_align(w); jw_byte(w, 0xFF); jw_byte(w, 0xD9); jw_flush(w);
    if (w->err) rc = EM_ERR_IO;
done:
    em_free(mem);
    return rc;
}
