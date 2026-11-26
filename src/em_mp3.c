// em_mp3.c - MP3 playback adapter. The Layer III maths is NOT reimplemented here:
// a bit-exact decoder is thousands of lines of tables, so this wraps minimp3
// (CC0, one header, ~7KB state) - drop minimp3.h next to this file and build
// with -DEM_WITH_MINIMP3. This file owns everything around it: ID3v2 skipping,
// refilling the input window, and handing back PCM in the caller's buffer.
#include "em_priv.h"
#include <stdlib.h>

#ifndef EM_WITH_MINIMP3
em_err_t em_mp3_open(em_mp3_t **out, em_io_t *io, em_audio_info_t *info) { (void)out; (void)io; (void)info; return EM_ERR_UNSUPPORTED; }
int em_mp3_read(em_mp3_t *m, int16_t *pcm, int max_frames) { (void)m; (void)pcm; (void)max_frames; return -1; }
void em_mp3_close(em_mp3_t *m) { (void)m; }
#else
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

#define MP3_WIN (16 * 1024)

struct em_mp3 {
    em_io_t *io; mp3dec_t dec;
    uint32_t pos, len; int eof;
    uint32_t rate; int ch;
    uint8_t buf[MP3_WIN];
};

static void refill(em_mp3_t *m) {
    if (m->pos) { memmove(m->buf, m->buf + m->pos, m->len - m->pos); m->len -= m->pos; m->pos = 0; }
    while (m->len < MP3_WIN && !m->eof) {
        int n = m->io->read(m->io->ctx, m->buf + m->len, MP3_WIN - m->len);
        if (n <= 0) { m->eof = 1; break; }
        m->len += (uint32_t)n;
    }
}

em_err_t em_mp3_open(em_mp3_t **out, em_io_t *io, em_audio_info_t *info) {
    uint8_t h[10];
    if (io->seek(io->ctx, 0) || io->read(io->ctx, h, 10) != 10) return EM_ERR_FORMAT;
    uint32_t start = 0;
    if (!memcmp(h, "ID3", 3)) start = 10 + ((uint32_t)(h[6] & 0x7F) << 21 | (uint32_t)(h[7] & 0x7F) << 14 | (uint32_t)(h[8] & 0x7F) << 7 | (h[9] & 0x7F)) + ((h[5] & 0x10) ? 10 : 0);
    if (io->seek(io->ctx, start)) return EM_ERR_IO;
    em_mp3_t *m = em_alloc(sizeof *m, EM_MEM_BIG);
    if (!m) return EM_ERR_NOMEM;
    memset(m, 0, sizeof *m); m->io = io;
    mp3dec_init(&m->dec);
    refill(m);
    // probe the first frame without decoding it
    mp3dec_frame_info_t fi; memset(&fi, 0, sizeof fi);
    mp3dec_t probe; mp3dec_init(&probe);
    int s = mp3dec_decode_frame(&probe, m->buf, (int)m->len, NULL, &fi);
    if (!s || fi.frame_bytes <= 0 || fi.hz <= 0) { em_free(m); return EM_ERR_FORMAT; }
    m->rate = (uint32_t)fi.hz; m->ch = fi.channels;
    if (info) { info->sample_rate = m->rate; info->channels = (uint8_t)m->ch; info->total_frames = 0; }
    *out = m;
    return EM_OK;
}

int em_mp3_read(em_mp3_t *m, int16_t *pcm, int max_frames) {
    if (max_frames < MINIMP3_MAX_SAMPLES_PER_FRAME / 2) return -1;     // caller must offer >= 1152 frames
    for (int guard = 0; guard < 64; guard++) {
        if (m->len - m->pos < MP3_WIN / 2 && !m->eof) refill(m);
        if (m->pos >= m->len) return 0;
        mp3dec_frame_info_t fi;
        int samples = mp3dec_decode_frame(&m->dec, m->buf + m->pos, (int)(m->len - m->pos), (mp3d_sample_t *)pcm, &fi);
        if (fi.frame_bytes == 0) { if (m->eof) return 0; refill(m); continue; }
        m->pos += (uint32_t)fi.frame_bytes;
        if (samples > 0) return samples;
    }
    return 0;
}
void em_mp3_close(em_mp3_t *m) { em_free(m); }
#endif
