// em_wav.c - WAV (PCM8/PCM16/IMA-ADPCM) reader + writer, and the IMA ADPCM codec.
#include "em_priv.h"
#include <stdlib.h>

// ---- IMA ADPCM ------------------------------------------------------------------
static const int16_t k_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
    80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
    2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t k_idx[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

typedef struct { int pred, idx; } adpcm_st;

static inline int adpcm_dec(adpcm_st *s, int nib) {
    int step = k_step[s->idx], d = step >> 3;
    if (nib & 1) d += step >> 2;
    if (nib & 2) d += step >> 1;
    if (nib & 4) d += step;
    s->pred += (nib & 8) ? -d : d;
    if (s->pred > 32767) s->pred = 32767; else if (s->pred < -32768) s->pred = -32768;
    s->idx += k_idx[nib & 7];
    if (s->idx < 0) s->idx = 0; else if (s->idx > 88) s->idx = 88;
    return s->pred;
}
static inline int adpcm_enc(adpcm_st *s, int sample) {
    int step = k_step[s->idx], diff = sample - s->pred, nib = 0;
    if (diff < 0) { nib = 8; diff = -diff; }
    int d = step >> 3;
    if (diff >= step) { nib |= 4; diff -= step; d += step; }
    step >>= 1;
    if (diff >= step) { nib |= 2; diff -= step; d += step; }
    step >>= 1;
    if (diff >= step) { nib |= 1; d += step; }
    s->pred += (nib & 8) ? -d : d;
    if (s->pred > 32767) s->pred = 32767; else if (s->pred < -32768) s->pred = -32768;
    s->idx += k_idx[nib & 7];
    if (s->idx < 0) s->idx = 0; else if (s->idx > 88) s->idx = 88;
    return nib;
}

int em_adpcm_block_frames(int ch, int block_bytes) { return (block_bytes - 4 * ch) * 2 / ch + 1; }

void em_adpcm_decode_block(const uint8_t *b, int block_bytes, int ch, int16_t *pcm) {
    adpcm_st st[2];
    for (int c = 0; c < ch; c++) {
        st[c].pred = (int16_t)rd16(b + 4 * c);
        st[c].idx = b[4 * c + 2] > 88 ? 88 : b[4 * c + 2];
        pcm[c] = (int16_t)st[c].pred;
    }
    int frames = em_adpcm_block_frames(ch, block_bytes);
    const uint8_t *p = b + 4 * ch;
    int f = 1;
    while (f < frames) {
        for (int c = 0; c < ch; c++)
            for (int i = 0; i < 4; i++) {
                uint8_t v = p[c * 4 + i];
                int fa = f + i * 2;
                int lo = adpcm_dec(&st[c], v & 15), hi = adpcm_dec(&st[c], v >> 4);
                if (fa < frames) pcm[fa * ch + c] = (int16_t)lo;
                if (fa + 1 < frames) pcm[(fa + 1) * ch + c] = (int16_t)hi;
            }
        p += 4 * ch; f += 8;
    }
}

// Encodes exactly em_adpcm_block_frames(ch, block_bytes) frames (caller pads with zeros).
int em_adpcm_encode_block(const int16_t *pcm, int frames, int ch, uint8_t *blk) {
    int block_bytes = 4 * ch + ((frames - 1) * ch + 1) / 2;
    adpcm_st st[2];
    for (int c = 0; c < ch; c++) {
        st[c].pred = pcm[c]; st[c].idx = 0;
        // Pick a starting step from the first delta so loud blocks don't start at step 7.
        if (frames > 1) {
            int d = pcm[ch + c] - pcm[c]; if (d < 0) d = -d;
            while (st[c].idx < 88 && k_step[st[c].idx] * 2 < d) st[c].idx += 2;
            if (st[c].idx > 88) st[c].idx = 88;
        }
        wr16(blk + 4 * c, (uint16_t)pcm[c]); blk[4 * c + 2] = (uint8_t)st[c].idx; blk[4 * c + 3] = 0;
    }
    uint8_t *p = blk + 4 * ch;
    int f = 1;
    while (f < frames) {
        for (int c = 0; c < ch; c++)
            for (int i = 0; i < 4; i++) {
                int fa = f + i * 2, fb = fa + 1;
                int lo = fa < frames ? adpcm_enc(&st[c], pcm[fa * ch + c]) : 0;
                int hi = fb < frames ? adpcm_enc(&st[c], pcm[fb * ch + c]) : 0;
                p[c * 4 + i] = (uint8_t)(lo | hi << 4);
            }
        p += 4 * ch; f += 8;
    }
    return block_bytes;
}

// ---- WAV reader -------------------------------------------------------------------
#define WAV_ADPCM_BLOCK 512

struct em_wav {
    em_io_t *io;
    int writer;
    uint16_t tag, ch, bits, block_align, spb;   // spb: samples/frames per ADPCM block
    uint32_t rate;
    uint32_t data_off, data_len, pos;           // bytes within data
    uint32_t frames_total;
    // ADPCM state (reader: decoded block cache; writer: accumulation buffer)
    int16_t *pcm_blk; uint8_t *raw_blk; int blk_pos, blk_frames;
    uint32_t frames_written;
};

static int read_at(em_io_t *io, uint32_t off, void *dst, uint32_t n) {
    return io->seek(io->ctx, off) == 0 && io_read_exact(io, dst, n) == 0 ? 0 : -1;
}

em_err_t em_wav_open(em_wav_t **out, em_io_t *io, em_audio_info_t *info) {
    uint8_t h[12];
    if (read_at(io, 0, h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return EM_ERR_FORMAT;
    uint32_t off = 12, fmt_ok = 0, data_off = 0, data_len = 0;
    uint16_t tag = 0, ch = 0, bits = 0, ba = 0; uint32_t rate = 0;
    for (;;) {
        uint8_t ck[8];
        if (read_at(io, off, ck, 8)) break;
        uint32_t sz = rd32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            uint8_t f[40]; uint32_t n = sz < 40 ? sz : 40;
            if (n < 16 || read_at(io, off + 8, f, n)) return EM_ERR_FORMAT;
            tag = rd16(f); ch = rd16(f + 2); rate = rd32(f + 4); ba = rd16(f + 12); bits = rd16(f + 14);
            if (tag == 0xFFFE && n >= 26) tag = rd16(f + 24);
            fmt_ok = 1;
        } else if (!memcmp(ck, "data", 4)) {
            data_off = off + 8; data_len = sz; break;
        }
        off += 8 + sz + (sz & 1);
    }
    if (!fmt_ok || !data_off) return EM_ERR_FORMAT;
    if (ch < 1 || ch > 2) return EM_ERR_UNSUPPORTED;
    if (!((tag == 1 && (bits == 8 || bits == 16)) || (tag == 0x11 && bits == 4))) return EM_ERR_UNSUPPORTED;
    if (tag == 0x11 && (ba < 4 * ch + 4 || ba > 4096)) return EM_ERR_FORMAT;
    if (tag == 1 && ba != ch * bits / 8) ba = ch * bits / 8;
    // Live-recorded files may carry a 0/0xFFFFFFFF data size: then read to EOF.
    int unknown = (data_len == 0 || data_len == 0xFFFFFFFFu);
    em_wav_t *w = em_alloc(sizeof *w + (tag == 0x11 ? (size_t)ba + (size_t)em_adpcm_block_frames(ch, ba) * ch * 2 : 0), EM_MEM_FAST);
    if (!w) return EM_ERR_NOMEM;
    memset(w, 0, sizeof *w);
    w->io = io; w->tag = tag; w->ch = ch; w->bits = bits; w->block_align = ba; w->rate = rate;
    w->data_off = data_off; w->data_len = unknown ? 0xFFFFFFFFu : data_len;
    if (tag == 0x11) {
        w->spb = (uint16_t)em_adpcm_block_frames(ch, ba);
        w->raw_blk = (uint8_t *)(w + 1);
        w->pcm_blk = (int16_t *)(w->raw_blk + ba + (ba & 1));
        w->blk_pos = w->blk_frames = 0;
        w->frames_total = unknown ? 0 : (data_len / ba) * w->spb;
    } else {
        w->frames_total = unknown ? 0 : data_len / ba;
    }
    io->seek(io->ctx, data_off);
    if (info) { info->sample_rate = rate; info->channels = (uint8_t)ch; info->total_frames = w->frames_total; }
    *out = w;
    return EM_OK;
}

static int wav_fill_adpcm(em_wav_t *w) {
    uint32_t left = w->data_len == 0xFFFFFFFFu ? w->block_align : w->data_len - w->pos;
    if (left < w->block_align) return 0;
    int n = w->io->read(w->io->ctx, w->raw_blk, w->block_align);
    if (n != w->block_align) return 0;
    w->pos += w->block_align;
    em_adpcm_decode_block(w->raw_blk, w->block_align, w->ch, w->pcm_blk);
    w->blk_pos = 0; w->blk_frames = w->spb;
    return 1;
}

int em_wav_read(em_wav_t *w, int16_t *pcm, int max_frames) {
    if (!w || w->writer) return -1;
    int got = 0;
    if (w->tag == 0x11) {
        while (got < max_frames) {
            if (w->blk_pos >= w->blk_frames && !wav_fill_adpcm(w)) break;
            int n = w->blk_frames - w->blk_pos; if (n > max_frames - got) n = max_frames - got;
            memcpy(pcm + got * w->ch, w->pcm_blk + w->blk_pos * w->ch, (size_t)n * w->ch * 2);
            w->blk_pos += n; got += n;
        }
        return got;
    }
    uint32_t want = (uint32_t)max_frames * w->block_align;
    if (w->data_len != 0xFFFFFFFFu && want > w->data_len - w->pos) want = w->data_len - w->pos;
    if (w->bits == 16) {
        int n = w->io->read(w->io->ctx, pcm, want);
        if (n < 0) return -1;
        n -= n % w->block_align;
        w->pos += (uint32_t)n;
        return n / w->block_align;
    }
    uint8_t tmp[128];
    uint32_t total = 0;
    while (want) {
        uint32_t c = want < sizeof tmp ? want : sizeof tmp;
        int n = w->io->read(w->io->ctx, tmp, c);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) pcm[total + i] = (int16_t)((tmp[i] - 128) << 8);
        total += (uint32_t)n; want -= (uint32_t)n;
        if ((uint32_t)n < c) break;
    }
    total -= total % w->ch;
    w->pos += total;
    return (int)(total / w->ch);
}

em_err_t em_wav_seek(em_wav_t *w, uint32_t frame) {
    if (!w || w->writer) return EM_ERR_ARG;
    if (w->tag == 0x11) {
        uint32_t blk = frame / w->spb;
        w->pos = blk * w->block_align;
        if (w->io->seek(w->io->ctx, w->data_off + w->pos)) return EM_ERR_IO;
        w->blk_pos = w->blk_frames = 0;
        if (!wav_fill_adpcm(w)) return EM_EOF;
        w->blk_pos = (int)(frame % w->spb);
        return EM_OK;
    }
    w->pos = frame * w->block_align;
    return w->io->seek(w->io->ctx, w->data_off + w->pos) ? EM_ERR_IO : EM_OK;
}

void em_wav_close(em_wav_t *w) { em_free(w); }

// ---- WAV writer ---------------------------------------------------------------------
static uint32_t wav_hdr(em_wav_t *w, uint8_t *h, uint32_t data_len, uint32_t frames) {
    int ad = w->tag == 0x11;
    uint32_t n = 0;
    memcpy(h, "RIFF", 4); memcpy(h + 8, "WAVEfmt ", 8);
    wr32(h + 16, ad ? 20 : 16);
    wr16(h + 20, w->tag); wr16(h + 22, w->ch); wr32(h + 24, w->rate);
    wr32(h + 28, ad ? w->rate / w->spb * w->block_align : w->rate * w->block_align);
    wr16(h + 32, w->block_align); wr16(h + 34, w->bits);
    n = 36;
    if (ad) { wr16(h + 36, 2); wr16(h + 38, w->spb); memcpy(h + 40, "fact", 4); wr32(h + 44, 4); wr32(h + 48, frames); n = 52; }
    memcpy(h + n, "data", 4); wr32(h + n + 4, data_len);
    n += 8;
    wr32(h + 4, n - 8 + data_len);
    return n;
}

em_err_t em_wav_create(em_wav_t **out, em_io_t *io, uint32_t rate, int ch, int adpcm) {
    if (ch < 1 || ch > 2 || !rate) return EM_ERR_ARG;
    size_t extra = adpcm ? (size_t)WAV_ADPCM_BLOCK + (size_t)em_adpcm_block_frames(ch, WAV_ADPCM_BLOCK) * ch * 2 : 0;
    em_wav_t *w = em_alloc(sizeof *w + extra, EM_MEM_FAST);
    if (!w) return EM_ERR_NOMEM;
    memset(w, 0, sizeof *w);
    w->io = io; w->writer = 1; w->ch = (uint16_t)ch; w->rate = rate;
    if (adpcm) {
        w->tag = 0x11; w->bits = 4; w->block_align = WAV_ADPCM_BLOCK;
        w->spb = (uint16_t)em_adpcm_block_frames(ch, WAV_ADPCM_BLOCK);
        w->raw_blk = (uint8_t *)(w + 1);
        w->pcm_blk = (int16_t *)(w->raw_blk + WAV_ADPCM_BLOCK);
    } else { w->tag = 1; w->bits = 16; w->block_align = (uint16_t)(2 * ch); }
    uint8_t h[64] = {0};
    uint32_t n = wav_hdr(w, h, 0, 0);
    if (io_write_all(io, h, n)) { em_free(w); return EM_ERR_IO; }
    w->data_off = n;
    *out = w;
    return EM_OK;
}

static int adpcm_flush(em_wav_t *w, int pad) {
    if (w->blk_pos == 0) return 0;
    if (pad) memset(w->pcm_blk + w->blk_pos * w->ch, 0, (size_t)(w->spb - w->blk_pos) * w->ch * 2);
    em_adpcm_encode_block(w->pcm_blk, w->spb, w->ch, w->raw_blk);
    w->blk_pos = 0;
    if (io_write_all(w->io, w->raw_blk, w->block_align)) return -1;
    w->data_len += w->block_align;
    return 0;
}

int em_wav_write(em_wav_t *w, const int16_t *pcm, int frames) {
    if (!w || !w->writer) return -1;
    if (w->tag == 1) {
        if (io_write_all(w->io, pcm, (uint32_t)frames * w->block_align)) return -1;
        w->data_len += (uint32_t)frames * w->block_align;
        w->frames_written += (uint32_t)frames;
        return frames;
    }
    int done = 0;
    while (done < frames) {
        int n = w->spb - w->blk_pos; if (n > frames - done) n = frames - done;
        memcpy(w->pcm_blk + w->blk_pos * w->ch, pcm + done * w->ch, (size_t)n * w->ch * 2);
        w->blk_pos += n; done += n;
        if (w->blk_pos == w->spb && adpcm_flush(w, 0)) return -1;
    }
    w->frames_written += (uint32_t)frames;
    return frames;
}

em_err_t em_wav_finish(em_wav_t *w) {
    if (!w || !w->writer) return EM_ERR_ARG;
    em_err_t r = EM_OK;
    if (w->tag == 0x11 && adpcm_flush(w, 1)) r = EM_ERR_IO;
    uint8_t h[64] = {0};
    uint32_t n = wav_hdr(w, h, w->data_len, w->frames_written);
    if (w->io->seek(w->io->ctx, 0) || io_write_all(w->io, h, n)) r = EM_ERR_IO;
    w->io->seek(w->io->ctx, w->data_off + w->data_len);
    em_free(w);
    return r;
}
