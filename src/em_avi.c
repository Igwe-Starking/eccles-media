// em_avi.c - AVI (RIFF) demuxer and muxer. Video: MJPEG. Audio: PCM16 / IMA ADPCM.
#include "em_container.h"
#include <stdlib.h>

#define HDR_SIZE_V   (12 + 12 + 64 + 124)      // RIFF + LIST hdrl header + avih chunk + video strl
#define AUD_STRL     100
// ---- demux -------------------------------------------------------------------------------------
typedef struct {
    struct em_demux base;
    uint32_t pos, movi_end;
    int vstream, astream;                 // chunk stream numbers (-1 none)
    uint32_t fps_x1000, vframe;
    uint32_t arate; uint16_t ach, ablock, aspb, atag; uint32_t aframes;
} avi_t;

static int rd_at(em_io_t *io, uint32_t off, void *d, uint32_t n) { return io->seek(io->ctx, off) == 0 && io_read_exact(io, d, n) == 0 ? 0 : -1; }

static em_err_t avi_next(struct em_demux *b, em_packet_t *pkt) {
    avi_t *a = (avi_t *)b;
    uint32_t pos = b->pay_off + b->pay_left;
    if (pos > a->pos) a->pos = pos;
    for (;;) {
        uint32_t p = a->pos;
        if (p & 1) p++;                       // chunks are word aligned
        if (p + 8 > a->movi_end) return EM_EOF;
        uint8_t h[12];
        if (rd_at(b->io, p, h, 8)) return EM_EOF;
        uint32_t sz = rd32(h + 4);
        if (!memcmp(h, "LIST", 4)) { a->pos = p + 12; continue; }        // 'rec ' lists: descend
        if (!memcmp(h, "idx1", 4)) return EM_EOF;
        a->pos = p + 8 + sz;
        if (a->pos > a->movi_end + 8 && a->movi_end) { /* truncated file: clamp below */ }
        int s0 = h[0] - '0', s1 = h[1] - '0';
        if (s0 < 0 || s0 > 9 || s1 < 0 || s1 > 9) continue;              // JUNK etc
        int stream = s0 * 10 + s1;
        if (stream == a->vstream && (h[2] == 'd') && (h[3] == 'c' || h[3] == 'b')) {
            pkt->kind = EM_KIND_VIDEO; pkt->size = sz;
            pkt->pts_ms = a->fps_x1000 ? (uint32_t)((uint64_t)a->vframe * 1000000u / a->fps_x1000) : 0;
            a->vframe++;
        } else if (stream == a->astream && h[2] == 'w' && h[3] == 'b') {
            pkt->kind = EM_KIND_AUDIO; pkt->size = sz;
            pkt->pts_ms = a->arate ? (uint32_t)((uint64_t)a->aframes * 1000u / a->arate) : 0;
            a->aframes += a->atag == 0x11 && a->ablock ? (sz / a->ablock) * a->aspb : sz / (a->ablock ? a->ablock : 1);
        } else continue;
        b->pay_off = p + 8; b->pay_left = sz; b->pay_seeked = 0;
        return EM_OK;
    }
}
static void avi_destroy(struct em_demux *b) { em_free(b); }

em_err_t em_avi_open(struct em_demux **out, em_io_t *io, em_media_info_t *info) {
    uint8_t h[12];
    if (rd_at(io, 0, h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "AVI ", 4)) return EM_ERR_FORMAT;
    avi_t *a = em_alloc(sizeof *a, EM_MEM_FAST);
    if (!a) return EM_ERR_NOMEM;
    memset(a, 0, sizeof *a);
    a->base.io = io; a->base.next = avi_next; a->base.destroy = avi_destroy;
    a->vstream = a->astream = -1;
    memset(info, 0, sizeof *info);
    uint32_t pos = 12, vlen = 0, alen = 0; int nstream = 0, cur = -1, curtype = 0;
    int have_movi = 0;
    // walk top level; descend into hdrl and strl lists
    uint32_t end_of_hdrl = 0;
    while (!have_movi) {
        uint8_t ck[12];
        if (rd_at(io, pos, ck, 8)) break;
        uint32_t sz = rd32(ck + 4);
        if (!memcmp(ck, "LIST", 4)) {
            if (rd_at(io, pos + 8, ck + 8, 4)) break;
            if (!memcmp(ck + 8, "movi", 4)) {
                a->pos = pos + 12; a->movi_end = pos + 8 + sz; have_movi = 1; break;
            }
            if (!memcmp(ck + 8, "hdrl", 4)) end_of_hdrl = pos + 8 + sz;
            pos += 12; continue;                              // descend (hdrl / strl)
        }
        if (!memcmp(ck, "strh", 4) && sz >= 48) {
            uint8_t s[56]; if (rd_at(io, pos + 8, s, 48)) break;
            cur = nstream++;
            if (!memcmp(s, "vids", 4)) {
                curtype = 1; a->vstream = cur;
                uint32_t scale = rd32(s + 20), rate = rd32(s + 24);
                a->fps_x1000 = scale ? (uint32_t)((uint64_t)rate * 1000u / scale) : 0; vlen = rd32(s + 32);
                info->fps_x1000 = a->fps_x1000;
            } else if (!memcmp(s, "auds", 4)) {
                curtype = 2; a->astream = cur; alen = rd32(s + 32);
            } else curtype = 0;
        } else if (!memcmp(ck, "strf", 4) && cur >= 0) {
            uint8_t f[40]; uint32_t n = sz < 40 ? sz : 40; if (rd_at(io, pos + 8, f, n)) break;
            if (curtype == 1 && n >= 20) {
                info->has_video = 1; info->width = (uint16_t)rd32(f + 4); info->height = (uint16_t)abs((int32_t)rd32(f + 8));
                uint32_t comp = rd32(f + 16);
                uint32_t up = comp & ~0x20202020u;          // case-insensitive 'MJPG'
                info->vcodec = (up == FOURCC('M', 'J', 'P', 'G') || up == FOURCC('J', 'P', 'E', 'G')) ? EM_VC_MJPEG : EM_VC_OTHER;
            } else if (curtype == 2 && n >= 16) {
                info->has_audio = 1; a->atag = rd16(f); a->ach = rd16(f + 2); a->arate = rd32(f + 4); a->ablock = rd16(f + 12);
                info->channels = (uint8_t)a->ach; info->sample_rate = a->arate;
                if (a->atag == 1 && rd16(f + 14) == 16) info->acodec = EM_AC_PCM16;
                else if (a->atag == 0x11 && n >= 20) { info->acodec = EM_AC_IMA; a->aspb = rd16(f + 18); info->adpcm_block = a->ablock; }
                else info->acodec = EM_AC_OTHER;
            }
        }
        pos += 8 + sz + (sz & 1);
        if (end_of_hdrl && pos > end_of_hdrl + 64 * 1024) break;
    }
    if (!have_movi || (!info->has_video && !info->has_audio)) { em_free(a); return EM_ERR_FORMAT; }
    if (info->has_video && a->fps_x1000) info->duration_ms = (uint32_t)((uint64_t)vlen * 1000000u / a->fps_x1000);
    else if (info->has_audio && a->arate && a->ablock) info->duration_ms = (uint32_t)((uint64_t)alen * 1000u / a->arate);
    *out = &a->base;
    return EM_OK;
}

// ---- mux -----------------------------------------------------------------------------------------
static void tag(uint8_t *p, const char *t) { memcpy(p, t, 4); }

static uint32_t avi_header(const struct em_mux *m, uint8_t *h, uint32_t movi_len, uint32_t idx_len, uint32_t fps_x1000) {
    const em_media_info_t *i = &m->info;
    int aud = i->has_audio;
    uint32_t hsz = HDR_SIZE_V + (aud ? AUD_STRL : 0);
    uint32_t hdrl = 4 + 64 + 124 + (aud ? AUD_STRL : 0);
    memset(h, 0, hsz);
    tag(h, "RIFF"); tag(h + 8, "AVI ");
    // RIFF size = whole file minus the 8-byte RIFF header = headers + movi list header + chunks + idx1
    wr32(h + 4, (hsz - 8) + 12 + movi_len + (idx_len ? 8 + idx_len : 0));
    tag(h + 12, "LIST"); wr32(h + 16, hdrl); tag(h + 20, "hdrl");
    uint8_t *p = h + 24;
    tag(p, "avih"); wr32(p + 4, 56);
    uint32_t us = fps_x1000 ? (uint32_t)(1000000000ull / fps_x1000) : 0;
    wr32(p + 8, us); wr32(p + 20, 0x10);                                  // AVIF_HASINDEX
    wr32(p + 24, m->vframes); wr32(p + 32, aud ? 2 : 1); wr32(p + 36, 1 << 20);
    wr32(p + 40, i->width); wr32(p + 44, i->height);
    p += 64;
    // video strl
    tag(p, "LIST"); wr32(p + 4, 116); tag(p + 8, "strl"); p += 12;
    tag(p, "strh"); wr32(p + 4, 56); tag(p + 8, "vids"); tag(p + 12, "MJPG");
    wr32(p + 28, 1000); wr32(p + 32, fps_x1000); wr32(p + 40, m->vframes); wr32(p + 44, 1 << 20); wr32(p + 48, 0xFFFFFFFF);
    wr16(p + 60, i->width); wr16(p + 62, i->height);
    p += 64;
    tag(p, "strf"); wr32(p + 4, 40); wr32(p + 8, 40); wr32(p + 12, i->width); wr32(p + 16, i->height);
    wr16(p + 20, 1); wr16(p + 22, 24); tag(p + 24, "MJPG"); wr32(p + 28, (uint32_t)i->width * i->height * 3);
    p += 48;
    if (aud) {
        tag(p, "LIST"); wr32(p + 4, 92); tag(p + 8, "strl"); p += 12;
        tag(p, "strh"); wr32(p + 4, 56); tag(p + 8, "auds");
        wr32(p + 28, 1); wr32(p + 32, i->sample_rate); wr32(p + 40, m->aframes); wr32(p + 44, 1 << 16); wr32(p + 48, 0xFFFFFFFF);
        wr32(p + 52, 2u * i->channels);
        p += 64;
        tag(p, "strf"); wr32(p + 4, 16);
        wr16(p + 8, 1); wr16(p + 10, i->channels); wr32(p + 12, i->sample_rate);
        wr32(p + 16, i->sample_rate * 2u * i->channels); wr16(p + 20, 2u * i->channels); wr16(p + 22, 16);
        p += 24;
    }
    tag(p, "LIST"); wr32(p + 4, 4 + movi_len); tag(p + 8, "movi");
    return hsz + 12;
}

em_err_t em_avi_mux_begin(struct em_mux *m) {
    if (!m->info.has_video || m->info.vcodec != EM_VC_MJPEG) return EM_ERR_UNSUPPORTED;
    if (m->info.has_audio && (m->info.acodec != EM_AC_PCM16 || m->info.channels < 1 || m->info.channels > 2)) return EM_ERR_UNSUPPORTED;
    uint8_t z[400]; uint32_t n = HDR_SIZE_V + (m->info.has_audio ? AUD_STRL : 0) + 12;
    memset(z, 0, sizeof z);
    if (io_write_all(m->io, z, n)) return EM_ERR_IO;
    m->data_start = n; m->pos = n;
    return EM_OK;
}

// Called after the caller wrote the chunk payload; appends pad byte and records index.
em_err_t em_avi_mux_chunk(struct em_mux *m, int audio, const void *data, uint32_t n, uint32_t pts_or_frames) {
    if (m->n >= m->cap) return EM_ERR_FULL;
    uint8_t h[8]; memcpy(h, audio ? "01wb" : "00dc", 4); wr32(h + 4, n);
    uint32_t off = m->pos;
    if (io_write_all(m->io, h, 8) || (n && io_write_all(m->io, data, n))) return EM_ERR_IO;
    if (n & 1) { uint8_t z = 0; if (io_write_all(m->io, &z, 1)) return EM_ERR_IO; }
    m->pos += 8 + n + (n & 1);
    m->idx[m->n].off = off; m->idx[m->n].size = n | (audio ? 0x80000000u : 0); m->idx[m->n].pts = pts_or_frames; m->n++;
    return EM_OK;
}

em_err_t em_avi_mux_end(struct em_mux *m) {
    uint32_t movi_len = m->pos - m->data_start + 0;           // bytes of chunks
    uint32_t movi_pos = m->data_start - 4;                    // file offset of the 'movi' fourcc
    uint32_t idx_len = m->n * 16;
    for (uint32_t i = 0; i < m->n; i++) {
        uint8_t e[16]; int aud = (m->idx[i].size >> 31) & 1;
        memcpy(e, aud ? "01wb" : "00dc", 4); wr32(e + 4, aud ? 0 : 0x10);
        wr32(e + 8, m->idx[i].off - movi_pos); wr32(e + 12, m->idx[i].size & 0x7FFFFFFF);
        if (i == 0) { uint8_t t[8]; memcpy(t, "idx1", 4); wr32(t + 4, idx_len); if (io_write_all(m->io, t, 8)) return EM_ERR_IO; }
        if (io_write_all(m->io, e, 16)) return EM_ERR_IO;
    }
    uint32_t fps = m->info.fps_x1000;
    if (m->vframes > 1 && m->last_pts > m->first_pts)
        fps = (uint32_t)((uint64_t)(m->vframes - 1) * 1000000u / (m->last_pts - m->first_pts));
    if (!fps) fps = 15000;
    uint8_t h[HDR_SIZE_V + AUD_STRL + 12];
    avi_header(m, h, movi_len, m->n ? idx_len : 0, fps);   // movi_len = bytes of chunk data
    if (m->io->seek(m->io->ctx, 0) || io_write_all(m->io, h, m->data_start)) return EM_ERR_IO;
    return EM_OK;
}
