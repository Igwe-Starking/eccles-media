// em_player.c - one cooperative player for stills, audio files and video files.
// em_player_step() does one bounded unit of work (one audio buffer or one video
// frame). A task calls it in a loop; nothing here blocks except the sinks, which
// is where DMA back-pressure is supposed to happen.
#include "em_priv.h"
#include <stdlib.h>

#ifndef EM_LATE_DROP_MS
#define EM_LATE_DROP_MS 120      // video frames later than this vs the audio clock are skipped
#endif
#define ABUF_SAMPLES 4096        // int16 samples in the audio staging buffer (8 KB)
#define ARAW_BYTES   2048        // ADPCM block staging

enum { K_JPEG = 1, K_PNG, K_WAV, K_MP3, K_AV };

struct em_player {
    em_sink_t sink; int kind; em_io_t *io; int scale;
    int done, image_shown;
    // audio
    em_wav_t *wav; em_mp3_t *mp3; uint32_t arate; int ach; uint64_t aframes;
    int16_t *abuf; uint8_t *araw;
    // video
    em_demux_t *dm; em_media_info_t mi;
    em_jpeg_t *jpg; int began; uint32_t dropped, last_pts, start_ms; int started;
    em_io_t pkt_io; uint32_t pkt_pos;
    int ima_ok;
};

static int pkt_read(void *c, void *dst, uint32_t n) { em_player_t *p = c; int r = em_demux_read(p->dm, dst, n); if (r > 0) p->pkt_pos += (uint32_t)r; return r; }
static uint32_t pkt_tell(void *c) { return ((em_player_t *)c)->pkt_pos; }

static int rows_fwd(void *u, int y, int n, const uint16_t *px, int w) {
    em_player_t *p = u;
    return p->sink.video_rows ? p->sink.video_rows(p->sink.user, y, n, px, w) : 0;
}

static int sniff(em_io_t *io) {
    uint8_t h[12];
    if (io->seek(io->ctx, 0) || io->read(io->ctx, h, 12) < 12) return 0;
    if (h[0] == 0xFF && h[1] == 0xD8) return K_JPEG;
    if (!memcmp(h, "\x89PNG", 4)) return K_PNG;
    if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) return K_WAV;
    if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "AVI ", 4)) return K_AV;
    if (!memcmp(h + 4, "ftyp", 4) || !memcmp(h + 4, "moov", 4) || !memcmp(h + 4, "mdat", 4)) return K_AV;
    if (!memcmp(h, "ID3", 3) || (h[0] == 0xFF && (h[1] & 0xE0) == 0xE0)) return K_MP3;
    return 0;
}

em_err_t em_player_open(em_player_t **out, em_io_t *io, const em_sink_t *sink, em_player_info_t *info) {
    if (!out || !io || !sink) return EM_ERR_ARG;
    int k = sniff(io);
    if (!k) return EM_ERR_FORMAT;
    em_player_t *p = em_alloc(sizeof *p + ABUF_SAMPLES * 2 + ARAW_BYTES, EM_MEM_FAST);
    if (!p) return EM_ERR_NOMEM;
    memset(p, 0, sizeof *p);
    p->sink = *sink; p->kind = k; p->io = io;
    p->abuf = (int16_t *)(p + 1); p->araw = (uint8_t *)(p->abuf + ABUF_SAMPLES);
    em_player_info_t pi; memset(&pi, 0, sizeof pi);
    em_err_t e = EM_OK;
    if (k == K_JPEG) { em_img_info_t ii; io->seek(io->ctx, 0); e = em_jpeg_open(&p->jpg, io, &ii); pi.is_image = 1; pi.has_video = 1; pi.width = ii.width; pi.height = ii.height; }
    else if (k == K_PNG) { em_img_info_t ii; em_png_t *png; e = em_png_open(&png, io, &ii); if (!e) { em_png_close(png); } pi.is_image = 1; pi.has_video = 1; pi.width = ii.width; pi.height = ii.height; }
    else if (k == K_WAV) {
        em_audio_info_t ai; e = em_wav_open(&p->wav, io, &ai);
        if (!e) { p->arate = ai.sample_rate; p->ach = ai.channels; pi.has_audio = 1; pi.duration_ms = ai.sample_rate ? (uint32_t)((uint64_t)ai.total_frames * 1000u / ai.sample_rate) : 0; }
    } else if (k == K_MP3) {
        em_audio_info_t ai; e = em_mp3_open(&p->mp3, io, &ai);
        if (!e) { p->arate = ai.sample_rate; p->ach = ai.channels; pi.has_audio = 1; }
    } else {
        e = em_demux_open(&p->dm, io, &p->mi);
        if (!e) {
            pi.has_video = p->mi.has_video && p->mi.vcodec == EM_VC_MJPEG;
            pi.has_audio = p->mi.has_audio && (p->mi.acodec == EM_AC_PCM16 || p->mi.acodec == EM_AC_IMA) && sink->audio_write != NULL;
            pi.width = p->mi.width; pi.height = p->mi.height; pi.duration_ms = p->mi.duration_ms;
            p->arate = p->mi.sample_rate; p->ach = p->mi.channels;
            p->ima_ok = p->mi.acodec != EM_AC_IMA || (p->mi.adpcm_block >= 12 && p->mi.adpcm_block <= ARAW_BYTES &&
                        em_adpcm_block_frames(p->ach, p->mi.adpcm_block) * p->ach <= ABUF_SAMPLES);
            if (!pi.has_video && !pi.has_audio) e = EM_ERR_UNSUPPORTED;   // e.g. H.264-only: demuxable, not playable here
            p->mi.has_audio = pi.has_audio;
        }
    }
    if (e) { em_free(p); return e; }
    if (pi.has_audio && sink->audio_config && sink->audio_config(sink->user, p->arate, p->ach)) { em_player_close(p); return EM_ERR_IO; }
    if (info) *info = pi;
    *out = p;
    return EM_OK;
}

void em_player_set_scale(em_player_t *p, int s) { if (p && s >= 0 && s <= 3) p->scale = s; }

static em_err_t show_jpeg(em_player_t *p, em_io_t *io) {
    em_img_info_t ii; em_err_t e;
    if (!p->jpg) e = em_jpeg_open(&p->jpg, io, &ii); else e = em_jpeg_reset(p->jpg, io, &ii);
    if (e) return e;
    if (!p->began && p->sink.video_begin) { p->sink.video_begin(p->sink.user, (ii.width + (1 << p->scale) - 1) >> p->scale, (ii.height + (1 << p->scale) - 1) >> p->scale); }
    p->began = 1;
    return em_jpeg_decode(p->jpg, p->scale, rows_fwd, p);
}

static int audio_out(em_player_t *p, const int16_t *pcm, int frames) {
    if (!p->sink.audio_write) return 0;
    int r = p->sink.audio_write(p->sink.user, pcm, frames);
    p->aframes += (uint32_t)frames;
    return r;
}

static uint32_t clock_ms(em_player_t *p) {
    if (p->mi.has_audio && p->arate) return (uint32_t)(p->aframes * 1000u / p->arate);
    uint32_t now = p->sink.now_ms ? p->sink.now_ms(p->sink.user) : 0;
    if (!p->started) { p->start_ms = now; p->started = 1; }
    return now - p->start_ms;
}

em_err_t em_player_step(em_player_t *p) {
    if (!p || p->done) return EM_EOF;
    switch (p->kind) {
    case K_JPEG: case K_PNG: {
        if (p->image_shown) { p->done = 1; return EM_EOF; }
        p->image_shown = 1;
        em_err_t e;
        if (p->kind == K_JPEG) { p->io->seek(p->io->ctx, 0); e = show_jpeg(p, p->io); }
        else {
            em_png_t *png; em_img_info_t ii;
            p->io->seek(p->io->ctx, 0);
            e = em_png_open(&png, p->io, &ii);
            if (!e) { if (p->sink.video_begin) p->sink.video_begin(p->sink.user, ii.width, ii.height); e = em_png_decode(png, 0x0000, rows_fwd, p); em_png_close(png); }
        }
        return e;
    }
    case K_WAV: case K_MP3: {
        int n = p->kind == K_WAV ? em_wav_read(p->wav, p->abuf, ABUF_SAMPLES / p->ach > 1152 ? 1152 : ABUF_SAMPLES / p->ach)
                                 : em_mp3_read(p->mp3, p->abuf, 1152);
        if (n < 0) return EM_ERR_FORMAT;
        if (n == 0) { p->done = 1; return EM_EOF; }
        return audio_out(p, p->abuf, n) ? EM_ERR_IO : EM_OK;
    }
    default: break;
    }
    em_packet_t pk;
    em_err_t e = em_demux_next(p->dm, &pk);
    if (e) { p->done = 1; return EM_EOF; }
    if (pk.kind == EM_KIND_AUDIO) {
        if (!p->mi.has_audio || !p->ima_ok) return EM_OK;
        uint32_t left = pk.size;
        if (p->mi.acodec == EM_AC_PCM16) {
            uint32_t fb = 2u * p->ach, chunk = (ABUF_SAMPLES * 2 / fb) * fb;
            while (left >= fb) {
                uint32_t want = left < chunk ? left : chunk; want -= want % fb;
                int r = em_demux_read(p->dm, p->abuf, want);
                if (r <= 0) break;
                r -= r % (int)fb;
                if (audio_out(p, p->abuf, r / (int)fb)) return EM_ERR_IO;
                left -= (uint32_t)r;
            }
        } else {
            int ba = p->mi.adpcm_block, fr = em_adpcm_block_frames(p->ach, ba);
            while (left >= (uint32_t)ba) {
                if (em_demux_read(p->dm, p->araw, (uint32_t)ba) != ba) break;
                em_adpcm_decode_block(p->araw, ba, p->ach, p->abuf);
                if (audio_out(p, p->abuf, fr)) return EM_ERR_IO;
                left -= (uint32_t)ba;
            }
        }
        return EM_OK;
    }
    if (pk.kind == EM_KIND_VIDEO && p->mi.vcodec == EM_VC_MJPEG) {
        uint32_t clk = clock_ms(p);
        if (clk > pk.pts_ms + EM_LATE_DROP_MS) { p->dropped++; return EM_OK; }          // too late: skip decode
        if (!p->mi.has_audio && pk.pts_ms > clk && p->sink.sleep_ms) p->sink.sleep_ms(p->sink.user, pk.pts_ms - clk);
        p->pkt_pos = 0;
        p->pkt_io.read = pkt_read; p->pkt_io.tell = pkt_tell; p->pkt_io.write = NULL; p->pkt_io.seek = NULL; p->pkt_io.ctx = p;
        em_err_t r = show_jpeg(p, &p->pkt_io);
        p->last_pts = pk.pts_ms;
        if (r == EM_ERR_FORMAT) { p->dropped++; return EM_OK; }                          // one corrupt frame must not end playback
        return r;
    }
    return EM_OK;
}

uint32_t em_player_pos_ms(const em_player_t *p) {
    if (!p) return 0;
    if (p->kind == K_WAV || p->kind == K_MP3 || (p->kind == K_AV && p->mi.has_audio && p->arate)) return p->arate ? (uint32_t)(p->aframes * 1000u / p->arate) : 0;
    return p->last_pts;
}
uint32_t em_player_dropped(const em_player_t *p) { return p ? p->dropped : 0; }

void em_player_close(em_player_t *p) {
    if (!p) return;
    em_jpeg_close(p->jpg); em_wav_close(p->wav); em_mp3_close(p->mp3); em_demux_close(p->dm);
    em_free(p);
}
