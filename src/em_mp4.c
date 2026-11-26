// em_mp4.c - ISO base media (MP4 / 3GP / MOV) demuxer and muxer.
// Demux keeps O(1) RAM: sample tables stay in the file and are read on demand.
#include "em_container.h"
#include <stdlib.h>

// ---- demux -------------------------------------------------------------------------------------
typedef struct {
    uint8_t kind; uint32_t timescale, duration, fourcc;
    uint32_t stts_off, stts_n, stsc_off, stsc_n, stsz_off, stsz_n, stsz_const, stco_off, stco_n; int co64;
    uint32_t ch, rate, w, h, bits;
    // cursor
    uint32_t sample, chunk, stsc_i, in_chunk, spc, chunk_base, chunk_off, stts_i, stts_rem, dts, stts_delta;
    int have_chunk, done;
    // prefetched next packet
    int pf_ok; uint32_t pf_off, pf_size, pf_pts;
} trk_t;

typedef struct { struct em_demux base; trk_t t[2]; int nt; int vi, ai; } mp4_t;

static int rd_at(em_io_t *io, uint32_t off, void *d, uint32_t n) { return io->seek(io->ctx, off) == 0 && io_read_exact(io, d, n) == 0 ? 0 : -1; }
static int rd32_at(em_io_t *io, uint32_t off, uint32_t *v) { uint8_t b[4]; if (rd_at(io, off, b, 4)) return -1; *v = rd32be(b); return 0; }

static int fetch(mp4_t *m, trk_t *t) {
    em_io_t *io = m->base.io; t->pf_ok = 0;
    if (t->done) return 0;
    if (!t->have_chunk) {
        if (t->chunk >= t->stco_n) { t->done = 1; return 0; }
        // locate stsc entry covering this chunk
        for (;;) {
            if (t->stsc_i + 1 >= t->stsc_n) break;
            uint32_t nf; if (rd32_at(io, t->stsc_off + 8 + (t->stsc_i + 1) * 12, &nf)) { t->done = 1; return 0; }
            if (nf <= t->chunk + 1) t->stsc_i++; else break;
        }
        if (rd32_at(io, t->stsc_off + 8 + t->stsc_i * 12 + 4, &t->spc) || !t->spc) { t->done = 1; return 0; }
        if (t->co64) {
            uint8_t b[8]; if (rd_at(io, t->stco_off + 8 + t->chunk * 8, b, 8)) { t->done = 1; return 0; }
            t->chunk_base = rd32be(b + 4);             // 4 GB files are out of scope on FAT anyway
        } else if (rd32_at(io, t->stco_off + 8 + t->chunk * 4, &t->chunk_base)) { t->done = 1; return 0; }
        t->in_chunk = 0; t->chunk_off = 0; t->have_chunk = 1;
    }
    uint32_t size, nsamp = 1;
    if (t->stsz_const) { nsamp = t->spc - t->in_chunk; size = nsamp * t->stsz_const; }
    else {
        if (t->sample >= t->stsz_n || rd32_at(io, t->stsz_off + 12 + t->sample * 4, &size)) { t->done = 1; return 0; }
    }
    // timestamp from stts cursor (video) or sample count (const-size audio)
    uint32_t pts_ms;
    if (t->stsz_const) pts_ms = t->timescale ? (uint32_t)((uint64_t)t->sample * 1000u / t->timescale) : 0;
    else {
        while (t->stts_rem == 0 && t->stts_i < t->stts_n) {
            uint32_t c, d; if (rd32_at(io, t->stts_off + 8 + t->stts_i * 8, &c) || rd32_at(io, t->stts_off + 8 + t->stts_i * 8 + 4, &d)) break;
            t->stts_rem = c; t->stts_delta = d; t->stts_i++;
        }
        pts_ms = t->timescale ? (uint32_t)((uint64_t)t->dts * 1000u / t->timescale) : 0;
    }
    t->pf_off = t->chunk_base + t->chunk_off; t->pf_size = size; t->pf_pts = pts_ms; t->pf_ok = 1;
    // advance cursor past this packet
    t->chunk_off += size; t->in_chunk += nsamp; t->sample += nsamp;
    if (!t->stsz_const) { if (t->stts_rem) { t->dts += t->stts_delta; t->stts_rem--; } }
    if (t->in_chunk >= t->spc) { t->chunk++; t->have_chunk = 0; }
    return 1;
}

static em_err_t mp4_next(struct em_demux *b, em_packet_t *pkt) {
    mp4_t *m = (mp4_t *)b;
    int best = -1;
    for (int i = 0; i < m->nt; i++) {
        if (!m->t[i].pf_ok && !m->t[i].done) fetch(m, &m->t[i]);
        if (m->t[i].pf_ok && (best < 0 || m->t[i].pf_off < m->t[best].pf_off)) best = i;
    }
    if (best < 0) return EM_EOF;
    trk_t *t = &m->t[best];
    pkt->kind = t->kind; pkt->size = t->pf_size; pkt->pts_ms = t->pf_pts;
    b->pay_off = t->pf_off; b->pay_left = t->pf_size; b->pay_seeked = 0;
    t->pf_ok = 0;
    return EM_OK;
}
static void mp4_destroy(struct em_demux *b) { em_free(b); }

static void parse_stsd(mp4_t *m, trk_t *t, uint32_t off, uint32_t end) {
    em_io_t *io = m->base.io; uint8_t e[48];
    if (end - off < 16 + 8) return;
    if (rd_at(io, off + 8, e, sizeof e)) return;       // first entry (skip ver/flags + count)
    t->fourcc = rd32be(e + 4);
    if (t->kind == EM_KIND_VIDEO) { t->w = rd16be(e + 32); t->h = rd16be(e + 34); }
    else { t->ch = rd16be(e + 24); t->bits = rd16be(e + 26); t->rate = rd16be(e + 32); }
}

static void parse_boxes(mp4_t *m, trk_t *T, uint32_t pos, uint32_t end, int depth, uint32_t *tkw, uint32_t *tkh) {
    em_io_t *io = m->base.io;
    while (pos + 8 <= end) {
        uint8_t h[8]; if (rd_at(io, pos, h, 8)) return;
        uint32_t sz = rd32be(h), ty = rd32be(h + 4);
        if (sz < 8 || pos + sz > end) return;
        uint32_t body = pos + 8, bend = pos + sz;
        if (ty == FOURCC_BE('m', 'd', 'i', 'a') || ty == FOURCC_BE('m', 'i', 'n', 'f') || ty == FOURCC_BE('s', 't', 'b', 'l')) {
            if (depth < 6) parse_boxes(m, T, body, bend, depth + 1, tkw, tkh);
        } else if (ty == FOURCC_BE('t', 'k', 'h', 'd')) {
            uint8_t b[92]; uint32_t n = sz - 8 < sizeof b ? sz - 8 : sizeof b;
            if (n >= 84 && !rd_at(io, body, b, n) && b[0] == 0) { *tkw = rd32be(b + 76) >> 16; *tkh = rd32be(b + 80) >> 16; }
        } else if (ty == FOURCC_BE('m', 'd', 'h', 'd')) {
            uint8_t b[32];
            if (!rd_at(io, body, b, 24)) {
                if (b[0] == 1) { if (!rd_at(io, body, b, 32)) { T->timescale = rd32be(b + 20); T->duration = rd32be(b + 28); } }
                else { T->timescale = rd32be(b + 12); T->duration = rd32be(b + 16); }
            }
        } else if (ty == FOURCC_BE('h', 'd', 'l', 'r')) {
            uint8_t b[12];
            if (!rd_at(io, body, b, 12)) {
                uint32_t ht = rd32be(b + 8);
                if (ht == FOURCC_BE('v', 'i', 'd', 'e')) T->kind = EM_KIND_VIDEO;
                else if (ht == FOURCC_BE('s', 'o', 'u', 'n')) T->kind = EM_KIND_AUDIO;
            }
        } else if (ty == FOURCC_BE('s', 't', 's', 'd')) parse_stsd(m, T, body, bend);
        else if (ty == FOURCC_BE('s', 't', 't', 's')) { T->stts_off = body; rd32_at(io, body + 4, &T->stts_n); }
        else if (ty == FOURCC_BE('s', 't', 's', 'c')) { T->stsc_off = body; rd32_at(io, body + 4, &T->stsc_n); }
        else if (ty == FOURCC_BE('s', 't', 's', 'z')) { T->stsz_off = body; rd32_at(io, body + 4, &T->stsz_const); rd32_at(io, body + 8, &T->stsz_n); }
        else if (ty == FOURCC_BE('s', 't', 'c', 'o')) { T->stco_off = body; rd32_at(io, body + 4, &T->stco_n); }
        else if (ty == FOURCC_BE('c', 'o', '6', '4')) { T->stco_off = body; rd32_at(io, body + 4, &T->stco_n); T->co64 = 1; }
        pos = bend;
    }
}

static void parse_trak(mp4_t *m, uint32_t off, uint32_t end) {
    trk_t T; memset(&T, 0, sizeof T);
    uint32_t tk_w = 0, tk_h = 0;
    parse_boxes(m, &T, off, end, 0, &tk_w, &tk_h);
    if (T.kind == EM_KIND_NONE || !T.stco_n || !T.stsc_n || m->nt >= 2) return;
    if (T.kind == EM_KIND_VIDEO && !T.w) { T.w = tk_w; T.h = tk_h; }
    if ((T.kind == EM_KIND_VIDEO && m->vi >= 0) || (T.kind == EM_KIND_AUDIO && m->ai >= 0)) return;
    m->t[m->nt] = T;
    if (T.kind == EM_KIND_VIDEO) m->vi = m->nt; else m->ai = m->nt;
    m->nt++;
}

em_err_t em_mp4_open(struct em_demux **out, em_io_t *io, em_media_info_t *info) {
    uint8_t h[16];
    if (rd_at(io, 0, h, 8)) return EM_ERR_FORMAT;
    uint32_t t4 = rd32be(h + 4);
    if (t4 != FOURCC_BE('f', 't', 'y', 'p') && t4 != FOURCC_BE('m', 'o', 'o', 'v') && t4 != FOURCC_BE('m', 'd', 'a', 't') &&
        t4 != FOURCC_BE('f', 'r', 'e', 'e') && t4 != FOURCC_BE('w', 'i', 'd', 'e') && t4 != FOURCC_BE('s', 'k', 'i', 'p')) return EM_ERR_FORMAT;
    mp4_t *m = em_alloc(sizeof *m, EM_MEM_FAST);
    if (!m) return EM_ERR_NOMEM;
    memset(m, 0, sizeof *m);
    m->base.io = io; m->base.next = mp4_next; m->base.destroy = mp4_destroy; m->vi = m->ai = -1;
    uint32_t pos = 0; int have_moov = 0;
    uint32_t mv_ts = 0, mv_dur = 0;
    for (int guard = 0; guard < 64; guard++) {
        if (rd_at(io, pos, h, 8)) break;
        uint64_t sz = rd32be(h); uint32_t ty = rd32be(h + 4), hdr = 8;
        if (sz == 1) { uint8_t b[8]; if (rd_at(io, pos + 8, b, 8)) break; sz = ((uint64_t)rd32be(b) << 32) | rd32be(b + 4); hdr = 16; }
        if (ty == FOURCC_BE('m', 'o', 'o', 'v') && sz >= hdr + 8) {
            have_moov = 1;
            uint32_t p = pos + hdr, end = pos + (uint32_t)sz;
            while (p + 8 <= end) {
                uint8_t c[8]; if (rd_at(io, p, c, 8)) break;
                uint32_t csz = rd32be(c), cty = rd32be(c + 4);
                if (csz < 8 || p + csz > end) break;
                if (cty == FOURCC_BE('t', 'r', 'a', 'k')) parse_trak(m, p + 8, p + csz);
                else if (cty == FOURCC_BE('m', 'v', 'h', 'd')) { uint8_t b[20]; if (!rd_at(io, p + 8, b, 20) && b[0] == 0) { mv_ts = rd32be(b + 12); mv_dur = rd32be(b + 16); } }
                p += csz;
            }
            break;
        }
        if (sz == 0) break;
        pos += (uint32_t)sz;
    }
    if (!have_moov || m->nt == 0) { em_free(m); return EM_ERR_FORMAT; }
    memset(info, 0, sizeof *info);
    if (m->vi >= 0) {
        trk_t *t = &m->t[m->vi];
        info->has_video = 1; info->width = (uint16_t)t->w; info->height = (uint16_t)t->h;
        uint32_t f = t->fourcc;
        info->vcodec = (f == FOURCC_BE('j', 'p', 'e', 'g') || f == FOURCC_BE('m', 'j', 'p', 'a') || f == FOURCC_BE('m', 'j', 'p', 'b') || f == FOURCC_BE('M', 'J', 'P', 'G')) ? EM_VC_MJPEG : EM_VC_OTHER;
        if (t->stsz_n && t->duration) info->fps_x1000 = (uint32_t)((uint64_t)t->stsz_n * t->timescale * 1000u / t->duration);
    }
    if (m->ai >= 0) {
        trk_t *t = &m->t[m->ai];
        info->has_audio = 1; info->channels = (uint8_t)t->ch; info->sample_rate = t->rate ? t->rate : t->timescale;
        info->acodec = (t->fourcc == FOURCC_BE('s', 'o', 'w', 't') && t->bits == 16) ? EM_AC_PCM16 : EM_AC_OTHER;
        if (info->acodec == EM_AC_PCM16 && !t->stsz_const) { info->acodec = EM_AC_OTHER; }   // per-sample PCM tables: not worth the cost
    }
    uint32_t dur = 0;
    for (int i = 0; i < m->nt; i++) if (m->t[i].timescale) { uint32_t d = (uint32_t)((uint64_t)m->t[i].duration * 1000u / m->t[i].timescale); if (d > dur) dur = d; }
    if (!dur && mv_ts) dur = (uint32_t)((uint64_t)mv_dur * 1000u / mv_ts);
    info->duration_ms = dur;
    *out = &m->base;
    return EM_OK;
}

// ---- mux ---------------------------------------------------------------------------------------
// Layout: ftyp | mdat (size patched at finish) | moov. The moov is written in two passes over the
// same builder: pass 1 only measures (recording each box's size), pass 2 writes.
typedef struct { em_io_t *io; int err, writing; uint32_t n; uint32_t sz[64]; int seq; } bw_t;

static void bput(bw_t *b, const void *d, uint32_t n) { b->n += n; if (b->writing && !b->err && io_write_all(b->io, d, n)) b->err = 1; }
static void b32(bw_t *b, uint32_t v) { uint8_t t[4]; wr32be(t, v); bput(b, t, 4); }
static void b16(bw_t *b, uint32_t v) { uint8_t t[2]; wr16be(t, v); bput(b, t, 2); }
static void bz(bw_t *b, uint32_t n) { uint8_t z[16] = {0}; while (n) { uint32_t c = n < 16 ? n : 16; bput(b, z, c); n -= c; } }
typedef struct { int k; uint32_t start; } bx_t;
static bx_t bopen(bw_t *b, const char *type) {
    bx_t x; x.k = b->seq++; x.start = b->n;
    b32(b, b->writing ? b->sz[x.k] : 0); bput(b, type, 4);
    return x;
}
static void bclose(bw_t *b, bx_t x) { if (!b->writing) b->sz[x.k] = b->n - x.start; }

static const uint32_t k_matrix[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};

static void emit_track(bw_t *b, const struct em_mux *m, int audio) {
    const em_media_info_t *in = &m->info;
    uint32_t cnt = 0, total_frames = 0;
    for (uint32_t i = 0; i < m->n; i++) if (((m->idx[i].size >> 31) & 1) == (uint32_t)audio) { cnt++; if (audio) total_frames += m->idx[i].pts; }
    uint32_t dur_ms, mdia_ts;
    uint32_t vdelta_last = 33;
    if (!audio) {
        uint32_t prev = 0, first = 1, lastd = 0;
        for (uint32_t i = 0; i < m->n; i++) if (!(m->idx[i].size >> 31)) { if (!first) lastd = m->idx[i].pts - prev; prev = m->idx[i].pts; first = 0; }
        if (lastd) vdelta_last = lastd;
        dur_ms = (cnt ? prev - m->first_pts + vdelta_last : 0); mdia_ts = 1000;
    } else { dur_ms = in->sample_rate ? (uint32_t)((uint64_t)total_frames * 1000u / in->sample_rate) : 0; mdia_ts = in->sample_rate; }
    uint32_t mdur = audio ? total_frames : dur_ms;

    bx_t trak = bopen(b, "trak");
    bx_t tkhd = bopen(b, "tkhd");
    b32(b, 7); b32(b, 0); b32(b, 0); b32(b, audio ? 2 : 1); b32(b, 0); b32(b, dur_ms); bz(b, 8);
    b16(b, 0); b16(b, 0); b16(b, audio ? 0x0100 : 0); b16(b, 0);
    for (int i = 0; i < 9; i++) b32(b, k_matrix[i]);
    b32(b, audio ? 0 : (uint32_t)in->width << 16); b32(b, audio ? 0 : (uint32_t)in->height << 16);
    bclose(b, tkhd);
    bx_t mdia = bopen(b, "mdia");
    bx_t mdhd = bopen(b, "mdhd");
    b32(b, 0); b32(b, 0); b32(b, 0); b32(b, mdia_ts); b32(b, mdur); b16(b, 0x55C4); b16(b, 0);
    bclose(b, mdhd);
    bx_t hdlr = bopen(b, "hdlr");
    b32(b, 0); b32(b, 0); bput(b, audio ? "soun" : "vide", 4); bz(b, 12);
    { const char *nm = audio ? "SoundHandler" : "VideoHandler"; bput(b, nm, (uint32_t)strlen(nm) + 1); }
    bclose(b, hdlr);
    bx_t minf = bopen(b, "minf");
    if (audio) { bx_t x = bopen(b, "smhd"); b32(b, 0); b32(b, 0); bclose(b, x); }
    else { bx_t x = bopen(b, "vmhd"); b32(b, 1); bz(b, 8); bclose(b, x); }
    { bx_t dinf = bopen(b, "dinf"); bx_t dref = bopen(b, "dref"); b32(b, 0); b32(b, 1);
      bx_t url = bopen(b, "url "); b32(b, 1); bclose(b, url); bclose(b, dref); bclose(b, dinf); }
    bx_t stbl = bopen(b, "stbl");
    { bx_t x = bopen(b, "stsd"); b32(b, 0); b32(b, 1);
      if (!audio) {
          bx_t e = bopen(b, "jpeg"); bz(b, 6); b16(b, 1); bz(b, 16); b16(b, in->width); b16(b, in->height);
          b32(b, 0x00480000); b32(b, 0x00480000); b32(b, 0); b16(b, 1); bz(b, 32); b16(b, 0x18); b16(b, 0xFFFF); bclose(b, e);
      } else {
          bx_t e = bopen(b, "sowt"); bz(b, 6); b16(b, 1); b16(b, 0); b16(b, 0); b32(b, 0);
          b16(b, in->channels); b16(b, 16); b16(b, 0); b16(b, 0); b32(b, in->sample_rate << 16); bclose(b, e);
      }
      bclose(b, x); }
    // stts
    { bx_t x = bopen(b, "stts"); b32(b, 0);
      if (audio) { b32(b, 1); b32(b, total_frames); b32(b, 1); }
      else {
          // run-length of per-frame deltas
          uint32_t runs = 0, prev = 0, pd = 0xFFFFFFFF, rc = 0, first = 1; uint32_t pass = 0;
          for (pass = 0; pass < 2; pass++) {
              if (pass == 1) { b32(b, runs); }
              runs = 0; prev = 0; pd = 0xFFFFFFFF; rc = 0; first = 1;
              uint32_t seen = 0;
              for (uint32_t i = 0; i < m->n; i++) {
                  if (m->idx[i].size >> 31) continue;
                  seen++;
                  if (!first) {
                      uint32_t d = m->idx[i].pts - prev;
                      if (d == pd) rc++;
                      else { if (rc) { if (pass == 1) { b32(b, rc); b32(b, pd); } runs++; } pd = d; rc = 1; }
                  }
                  prev = m->idx[i].pts; first = 0;
                  if (seen == cnt) {   // last frame: reuse previous delta (or default)
                      uint32_t d = (pd == 0xFFFFFFFF) ? vdelta_last : pd;
                      if (d == pd) rc++; else { if (rc) { if (pass == 1) { b32(b, rc); b32(b, pd); } runs++; } pd = d; rc = 1; }
                  }
              }
              if (rc) { if (pass == 1) { b32(b, rc); b32(b, pd); } runs++; }
              if (pass == 0) { /* count only */ if (!b->writing) { b->n += 0; } }
          }
          (void)runs;
      }
      bclose(b, x); }
    // stsc
    { bx_t x = bopen(b, "stsc"); b32(b, 0);
      if (!audio) { b32(b, 1); b32(b, 1); b32(b, 1); b32(b, 1); }
      else {
          uint32_t entries = 0, last = 0xFFFFFFFF, chunk_no = 0;
          for (int pass = 0; pass < 2; pass++) {
              if (pass == 1) b32(b, entries);
              entries = 0; last = 0xFFFFFFFF; chunk_no = 0;
              for (uint32_t i = 0; i < m->n; i++) {
                  if (!(m->idx[i].size >> 31)) continue;
                  chunk_no++;
                  if (m->idx[i].pts != last) { if (pass == 1) { b32(b, chunk_no); b32(b, m->idx[i].pts); b32(b, 1); } entries++; last = m->idx[i].pts; }
              }
          }
      }
      bclose(b, x); }
    // stsz
    { bx_t x = bopen(b, "stsz"); b32(b, 0);
      if (audio) { b32(b, 2u * in->channels); b32(b, total_frames); }
      else { b32(b, 0); b32(b, cnt); for (uint32_t i = 0; i < m->n; i++) if (!(m->idx[i].size >> 31)) b32(b, m->idx[i].size & 0x7FFFFFFF); }
      bclose(b, x); }
    // stco
    { bx_t x = bopen(b, "stco"); b32(b, 0); b32(b, cnt);
      for (uint32_t i = 0; i < m->n; i++) if (((m->idx[i].size >> 31) & 1) == (uint32_t)audio) b32(b, m->idx[i].off);
      bclose(b, x); }
    bclose(b, stbl); bclose(b, minf); bclose(b, mdia); bclose(b, trak);
}

static void emit_moov(bw_t *b, const struct em_mux *m) {
    uint32_t dur = 0;
    if (m->vframes) {
        uint32_t last = m->last_pts - m->first_pts; dur = last + (m->vframes > 1 ? last / (m->vframes - 1) : 33);
    }
    if (m->info.has_audio && m->info.sample_rate) { uint32_t d = (uint32_t)((uint64_t)m->aframes * 1000u / m->info.sample_rate); if (d > dur) dur = d; }
    bx_t moov = bopen(b, "moov");
    bx_t mvhd = bopen(b, "mvhd");
    b32(b, 0); b32(b, 0); b32(b, 0); b32(b, 1000); b32(b, dur); b32(b, 0x00010000); b16(b, 0x0100); bz(b, 10);
    for (int i = 0; i < 9; i++) b32(b, k_matrix[i]);
    bz(b, 24); b32(b, m->info.has_audio ? 3 : 2);
    bclose(b, mvhd);
    emit_track(b, m, 0);
    if (m->info.has_audio) emit_track(b, m, 1);
    bclose(b, moov);
}

em_err_t em_mp4_mux_begin(struct em_mux *m) {
    if (!m->info.has_video || m->info.vcodec != EM_VC_MJPEG) return EM_ERR_UNSUPPORTED;
    if (m->info.has_audio && (m->info.acodec != EM_AC_PCM16 || m->info.channels < 1 || m->info.channels > 2)) return EM_ERR_UNSUPPORTED;
    uint8_t h[32]; uint32_t n = 24;
    wr32be(h, 24); memcpy(h + 4, "ftyp", 4);
    if (m->fmt == EM_FMT_3GP) { memcpy(h + 8, "3gp5", 4); wr32be(h + 12, 0); memcpy(h + 16, "3gp5", 4); memcpy(h + 20, "isom", 4); }
    else { memcpy(h + 8, "isom", 4); wr32be(h + 12, 0x200); memcpy(h + 16, "isom", 4); memcpy(h + 20, "iso2", 4); }
    wr32be(h + 24, 0); memcpy(h + 28, "mdat", 4);     // mdat header: size patched at finish
    if (io_write_all(m->io, h, 32)) return EM_ERR_IO;
    (void)n;
    m->data_start = 32; m->pos = 32;
    return EM_OK;
}

em_err_t em_mp4_mux_sample(struct em_mux *m, int audio, const void *data, uint32_t n, uint32_t pts_or_frames) {
    if (m->n >= m->cap) return EM_ERR_FULL;
    if (io_write_all(m->io, data, n)) return EM_ERR_IO;
    m->idx[m->n].off = m->pos; m->idx[m->n].size = n | (audio ? 0x80000000u : 0); m->idx[m->n].pts = pts_or_frames; m->n++;
    m->pos += n;
    return EM_OK;
}

em_err_t em_mp4_mux_end(struct em_mux *m) {
    uint8_t t[4]; wr32be(t, m->pos - 24);                           // mdat box size = header(8) + payload
    if (m->io->seek(m->io->ctx, 24) || io_write_all(m->io, t, 4)) return EM_ERR_IO;
    if (m->io->seek(m->io->ctx, m->pos)) return EM_ERR_IO;
    bw_t *b = em_alloc(sizeof *b, EM_MEM_FAST);
    if (!b) return EM_ERR_NOMEM;
    memset(b, 0, sizeof *b); b->io = m->io;
    emit_moov(b, m);                                                // pass 1: measure
    b->writing = 1; b->seq = 0; b->n = 0;
    emit_moov(b, m);                                                // pass 2: write
    em_err_t r = b->err ? EM_ERR_IO : EM_OK;
    em_free(b);
    return r;
}
