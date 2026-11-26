// em_demux.c / mux front-ends: format sniffing and the thin muxer API over AVI + MP4/3GP.
#include "em_container.h"

em_err_t em_demux_open(em_demux_t **out, em_io_t *io, em_media_info_t *info) {
    uint8_t h[12];
    if (!out || !io || !info) return EM_ERR_ARG;
    if (io->seek(io->ctx, 0) || io->read(io->ctx, h, 12) != 12) return EM_ERR_FORMAT;
    em_err_t e;
    if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "AVI ", 4)) e = em_avi_open(out, io, info);
    else e = em_mp4_open(out, io, info);
    return e;
}

em_err_t em_demux_next(em_demux_t *d, em_packet_t *pkt) { return d->next(d, pkt); }

int em_demux_read(em_demux_t *d, void *dst, uint32_t n) {
    if (n > d->pay_left) n = d->pay_left;
    if (!n) return 0;
    if (!d->pay_seeked) { if (d->io->seek(d->io->ctx, d->pay_off)) return -1; d->pay_seeked = 1; }
    int r = d->io->read(d->io->ctx, dst, n);
    if (r < 0) return -1;
    d->pay_off += (uint32_t)r; d->pay_left -= (uint32_t)r;
    return r;
}

void em_demux_close(em_demux_t *d) { if (d) d->destroy(d); }

// ---- muxer front-end ---------------------------------------------------------------------------------
em_err_t em_mux_create(em_mux_t **out, em_io_t *io, em_fmt_t fmt, const em_media_info_t *info, uint32_t max_packets) {
    if (!out || !io || !info || !max_packets || (fmt != EM_FMT_AVI && fmt != EM_FMT_MP4 && fmt != EM_FMT_3GP)) return EM_ERR_ARG;
    struct em_mux *m = em_alloc(sizeof *m + (size_t)max_packets * sizeof(em_idx_t), EM_MEM_BIG);
    if (!m) return EM_ERR_NOMEM;
    memset(m, 0, sizeof *m);
    m->io = io; m->fmt = fmt; m->info = *info; m->idx = (em_idx_t *)(m + 1); m->cap = max_packets;
    em_err_t e = fmt == EM_FMT_AVI ? em_avi_mux_begin(m) : em_mp4_mux_begin(m);
    if (e) { em_free(m); return e; }
    *out = m;
    return EM_OK;
}

em_err_t em_mux_video(em_mux_t *m, const void *jpeg, uint32_t n, uint32_t pts_ms) {
    if (!m || m->broken) return EM_ERR_ARG;
    if (!m->have_first) { m->first_pts = pts_ms; m->have_first = 1; }
    em_err_t e = m->fmt == EM_FMT_AVI ? em_avi_mux_chunk(m, 0, jpeg, n, pts_ms) : em_mp4_mux_sample(m, 0, jpeg, n, pts_ms);
    if (e == EM_OK) { m->vframes++; m->last_pts = pts_ms; }
    else if (e != EM_ERR_FULL) m->broken = 1;
    return e;
}

em_err_t em_mux_audio(em_mux_t *m, const int16_t *pcm, uint32_t frames) {
    if (!m || m->broken || !m->info.has_audio) return EM_ERR_ARG;
    uint32_t bytes = frames * 2u * m->info.channels;
    em_err_t e = m->fmt == EM_FMT_AVI ? em_avi_mux_chunk(m, 1, pcm, bytes, frames) : em_mp4_mux_sample(m, 1, pcm, bytes, frames);
    if (e == EM_OK) m->aframes += frames; else if (e != EM_ERR_FULL) m->broken = 1;
    return e;
}

em_err_t em_mux_finish(em_mux_t *m) {
    if (!m) return EM_ERR_ARG;
    em_err_t e = m->fmt == EM_FMT_AVI ? em_avi_mux_end(m) : em_mp4_mux_end(m);
    em_free(m);
    return e;
}
