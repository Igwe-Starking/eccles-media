#include "eccles_media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

typedef struct { uint64_t afr; int aconf_rate, aconf_ch; int frames, rows_calls; uint64_t vhash; uint32_t now, sleep_total, cost; int begin_w, begin_h; } rec_t;
static int a_cfg(void *u, uint32_t r, int c) { rec_t *s = u; s->aconf_rate = (int)r; s->aconf_ch = c; return 0; }
static int a_wr(void *u, const int16_t *p, int n) { rec_t *s = u; (void)p; s->afr += (uint64_t)n; return 0; }
static uint64_t fnv(uint64_t h, const void *d, size_t n) { const uint8_t *p = d; while (n--) { h ^= *p++; h *= 1099511628211ull; } return h; }
static int v_rows(void *u, int y, int n, const uint16_t *px, int w) { rec_t *s = u; if (y == 0) { s->frames++; s->now += s->cost; } s->rows_calls++; s->vhash = fnv(s->vhash, px, (size_t)n * w * 2); return 0; }
static void v_begin(void *u, int w, int h) { rec_t *s = u; s->begin_w = w; s->begin_h = h; }
static void v_sleep(void *u, uint32_t ms) { rec_t *s = u; s->now += ms; s->sleep_total += ms; }
static uint32_t v_now(void *u) { return ((rec_t *)u)->now; }

static uint8_t *slurp(const char *p, uint32_t *n) { FILE *f = fopen(p, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *b = malloc(*n); if (fread(b, 1, *n, f) != *n) return NULL; fclose(f); return b; }

static int ref_rows(void *u, int y, int n, const uint16_t *px, int w) { rec_t *s = u; (void)y; s->vhash = fnv(s->vhash, px, (size_t)n * w * 2); return 0; }

static em_err_t play(const char *path, rec_t *r, int use_audio, int scale, em_player_info_t *pi, uint32_t *dropped) {
    uint32_t n; uint8_t *b = slurp(path, &n); if (!b) return EM_ERR_IO;
    em_membuf_t mb; em_io_t io; em_io_mem(&io, &mb, b, n, n);
    em_sink_t sk = {use_audio ? a_cfg : NULL, use_audio ? a_wr : NULL, v_rows, v_begin, v_sleep, v_now, r};
    em_player_t *p; em_err_t e = em_player_open(&p, &io, &sk, pi);
    if (e) { free(b); return e; }
    em_player_set_scale(p, scale);
    int guard = 0; while ((e = em_player_step(p)) == EM_OK && ++guard < 100000) {}
    if (dropped) *dropped = em_player_dropped(p);
    em_player_close(p); free(b);
    return e == EM_EOF ? EM_OK : e;
}

int main(void) {
    rec_t r; em_player_info_t pi; uint32_t dr;
    const char *av[] = {"/tmp/m_ours.avi", "/tmp/m_ours.mp4", "/tmp/m_ours.3gp"};
    for (int i = 0; i < 3; i++) {
        memset(&r, 0, sizeof r);
        em_err_t e = play(av[i], &r, 1, 0, &pi, &dr);
        printf("%s: err=%d video frames=%d audio frames=%llu @%dHz/%dch dropped=%u size=%dx%d\n", av[i], e, r.frames, (unsigned long long)r.afr, r.aconf_rate, r.aconf_ch, dr, r.begin_w, r.begin_h);
        CHECK(e == EM_OK && r.frames == 45 && r.afr == 47985 && r.aconf_rate == 16000 && dr == 0 && r.begin_w == 160, "av playback");
        // plumbing check: same frames decoded directly through the demuxer must hash identically
        uint32_t n; uint8_t *b = slurp(av[i], &n); em_membuf_t mb; em_io_t io; em_io_mem(&io, &mb, b, n, n);
        em_demux_t *d; em_media_info_t mi; em_demux_open(&d, &io, &mi); em_packet_t pk; static uint8_t fr[65536]; rec_t ref; memset(&ref, 0, sizeof ref);
        while (em_demux_next(d, &pk) == EM_OK) { int got = em_demux_read(d, fr, pk.size); if (pk.kind != EM_KIND_VIDEO) continue;
            em_membuf_t jm; em_io_t jio; em_io_mem(&jio, &jm, fr, (uint32_t)got, (uint32_t)got); em_jpeg_t *j; em_img_info_t ii; em_jpeg_open(&j, &jio, &ii); em_jpeg_decode(j, 0, ref_rows, &ref); em_jpeg_close(j); }
        em_demux_close(d); free(b);
        CHECK(ref.vhash == r.vhash, "pixel hash differs from direct decode");
    }
    memset(&r, 0, sizeof r);
    em_err_t e = play("/tmp/ff.avi", &r, 1, 0, &pi, &dr);
    printf("ffmpeg-made avi: err=%d frames=%d audio=%llu @%dHz/%dch\n", e, r.frames, (unsigned long long)r.afr, r.aconf_rate, r.aconf_ch);
    CHECK(e == EM_OK && r.frames == 24 && r.afr == 44100 && r.aconf_ch == 2, "ff.avi");
    memset(&r, 0, sizeof r); e = play("/tmp/ff.mov", &r, 1, 1, &pi, &dr);
    printf("ffmpeg-made mov @1/2 scale: err=%d frames=%d audio=%llu begin=%dx%d\n", e, r.frames, (unsigned long long)r.afr, r.begin_w, r.begin_h);
    CHECK(e == EM_OK && r.frames == 24 && r.begin_w == 88 && r.begin_h == 72, "ff.mov scaled");
    memset(&r, 0, sizeof r); e = play("/tmp/t_pcm.wav", &r, 1, 0, &pi, &dr);
    printf("wav: err=%d frames=%llu dur=%ums\n", e, (unsigned long long)r.afr, pi.duration_ms); CHECK(e == EM_OK && r.afr == 20000, "wav");
    memset(&r, 0, sizeof r); e = play("/tmp/t_ima.wav", &r, 1, 0, &pi, &dr);
    printf("ima wav: err=%d frames=%llu\n", e, (unsigned long long)r.afr); CHECK(e == EM_OK && r.afr >= 20000, "ima wav");
    memset(&r, 0, sizeof r); e = play("/tmp/j_420_q85.jpg", &r, 0, 0, &pi, &dr);
    printf("jpeg still: err=%d image=%d %dx%d rows_calls=%d\n", e, pi.is_image, pi.width, pi.height, r.rows_calls); CHECK(e == EM_OK && pi.is_image && pi.width == 320 && r.frames == 1, "jpeg");
    memset(&r, 0, sizeof r); e = play("/tmp/p_rgba.png", &r, 0, 0, &pi, &dr);
    printf("png still: err=%d image=%d %dx%d\n", e, pi.is_image, pi.width, pi.height); CHECK(e == EM_OK && pi.is_image && r.frames == 1, "png");
    memset(&r, 0, sizeof r); e = play("/tmp/h264.mp4", &r, 1, 0, &pi, &dr);
    printf("h264+aac mp4: err=%d (expect %d = unsupported, demux works but no decoder)\n", e, EM_ERR_UNSUPPORTED); CHECK(e == EM_ERR_UNSUPPORTED, "h264 should be refused cleanly");
    // timing: video-only file, fake clock. Fast decode -> paced by sleeps, nothing dropped. Slow decode -> frames dropped.
    memset(&r, 0, sizeof r); e = play("/tmp/vonly.avi", &r, 0, 0, &pi, &dr);
    printf("video-only, instant decode: frames=%d dropped=%u slept=%ums (clip %ums)\n", r.frames, dr, r.sleep_total, pi.duration_ms);
    CHECK(e == EM_OK && dr == 0 && r.frames == 24 && r.sleep_total > 1700 && r.sleep_total < 2100, "pacing");
    memset(&r, 0, sizeof r); r.cost = 150; e = play("/tmp/vonly.avi", &r, 0, 0, &pi, &dr);
    printf("video-only, 150ms/frame decode: shown=%d dropped=%u (shown+dropped=%d)\n", r.frames, dr, r.frames + (int)dr);
    CHECK(e == EM_OK && dr > 5 && r.frames + (int)dr == 24, "late frames must be dropped, not queued");
    // garbage must be refused, not crash
    { uint8_t junk[64]; for (int i = 0; i < 64; i++) junk[i] = (uint8_t)(i * 37); em_membuf_t mb; em_io_t io; em_io_mem(&io, &mb, junk, 64, 64);
      em_sink_t sk = {0}; em_player_t *p; CHECK(em_player_open(&p, &io, &sk, NULL) != EM_OK, "junk accepted"); }
    printf(fails ? "PLAYER: %d FAILED\n" : "PLAYER: all ok\n", fails);
    return fails != 0;
}
