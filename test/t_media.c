#include "eccles_media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint8_t *slurp(const char *p, uint32_t *n) { FILE *f = fopen(p, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *b = malloc(*n); if (fread(b, 1, *n, f) != *n) return NULL; fclose(f); return b; }

// synthetic animated frame, encoded with our own JPEG encoder
static int W = 160, H = 120, g_frame;
static int src_cb(void *u, int y, int n, uint16_t *dst) {
    (void)u;
    for (int r = 0; r < n; r++)
        for (int x = 0; x < W; x++) {
            int yy = y + r;
            int R = (x * 255 / W + g_frame * 5) & 255, G = (yy * 255 / H) & 255, B = (((x - g_frame * 3) * (x - g_frame * 3) + (yy - 60) * (yy - 60)) < 900) ? 255 : 40;
            dst[r * W + x] = (uint16_t)(((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3));
        }
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "mux")) {
        em_fmt_t fmt = !strcmp(argv[2], "avi") ? EM_FMT_AVI : !strcmp(argv[2], "3gp") ? EM_FMT_3GP : EM_FMT_MP4;
        int nframes = 45, fps = 15, rate = 16000, ch = argc > 4 ? atoi(argv[4]) : 1;
        em_media_info_t mi; memset(&mi, 0, sizeof mi);
        mi.has_video = 1; mi.width = W; mi.height = H; mi.fps_x1000 = fps * 1000; mi.vcodec = EM_VC_MJPEG;
        mi.has_audio = 1; mi.sample_rate = rate; mi.channels = (uint8_t)ch; mi.acodec = EM_AC_PCM16;
        FILE *f = fopen(argv[3], "wb+"); em_io_t io; em_io_file(&io, f);
        em_mux_t *m; em_err_t e = em_mux_create(&m, &io, fmt, &mi, 1000);
        if (e) { printf("create err %d\n", e); return 2; }
        static uint8_t jbuf[64 * 1024]; static int16_t pcm[4096];
        FILE *vref = fopen("/tmp/mux_ref.mjpg", "wb"), *aref = fopen("/tmp/mux_ref.pcm", "wb");
        int apos = 0;
        for (int i = 0; i < nframes; i++) {
            g_frame = i;
            em_membuf_t mb; em_io_t jio; em_io_mem(&jio, &mb, jbuf, 0, sizeof jbuf);
            if (em_jpeg_encode(&jio, W, H, 80, 3, src_cb, NULL)) { printf("jpeg err\n"); return 2; }
            uint32_t pts = (uint32_t)((uint64_t)i * 1000 / fps);
            if ((e = em_mux_video(m, jbuf, mb.len, pts))) { printf("video err %d\n", e); return 2; }
            fwrite(jbuf, 1, mb.len, vref);
            int af = rate / fps + (i % 3 == 0 ? 1 : 0);                 // uneven chunk sizes exercise stsc runs
            for (int k = 0; k < af; k++, apos++) for (int c = 0; c < ch; c++) pcm[k * ch + c] = (int16_t)(8000 * sin(apos * 0.07 * (c + 1)));
            if ((e = em_mux_audio(m, pcm, (uint32_t)af))) { printf("audio err %d\n", e); return 2; }
            fwrite(pcm, 2 * ch, (size_t)af, aref);
        }
        fclose(vref); fclose(aref);
        e = em_mux_finish(m); fclose(f);
        printf("mux %s: %s\n", argv[2], e ? "FAILED" : "ok"); return e != 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "demux")) {                         // demux file outprefix
        uint32_t n; uint8_t *b = slurp(argv[2], &n); if (!b) { printf("no file\n"); return 2; }
        em_membuf_t mb; em_io_t io; em_io_mem(&io, &mb, b, n, n);
        em_demux_t *d; em_media_info_t mi;
        em_err_t e = em_demux_open(&d, &io, &mi);
        if (e) { printf("open err %d\n", e); return 2; }
        printf("video=%d %dx%d fps=%.2f vc=%d | audio=%d %uHz ch=%d ac=%d | dur=%ums\n", mi.has_video, mi.width, mi.height, mi.fps_x1000 / 1000.0, mi.vcodec,
               mi.has_audio, mi.sample_rate, mi.channels, mi.acodec, mi.duration_ms);
        char p1[256], p2[256]; snprintf(p1, sizeof p1, "%s.mjpg", argv[3]); snprintf(p2, sizeof p2, "%s.pcm", argv[3]);
        FILE *fv = fopen(p1, "wb"), *fa = fopen(p2, "wb");
        em_packet_t pk; int nv = 0, na = 0; uint32_t last_v = 0, last_a = 0; static uint8_t buf[256 * 1024]; int mono_ok = 1;
        while ((e = em_demux_next(d, &pk)) == EM_OK) {
            if (pk.size > sizeof buf) { printf("packet too big\n"); return 2; }
            int r = em_demux_read(d, buf, pk.size);
            if (r != (int)pk.size) { printf("short read %d/%u\n", r, pk.size); return 2; }
            if (pk.kind == EM_KIND_VIDEO) { nv++; if (nv > 1 && pk.pts_ms < last_v) mono_ok = 0; last_v = pk.pts_ms; fwrite(buf, 1, pk.size, fv); }
            else { na++; if (na > 1 && pk.pts_ms < last_a) mono_ok = 0; last_a = pk.pts_ms; fwrite(buf, 1, pk.size, fa); }
        }
        fclose(fv); fclose(fa);
        printf("packets: video=%d (last pts %u) audio=%d (last pts %u) monotonic=%d end=%d\n", nv, last_v, na, last_a, mono_ok, e);
        em_demux_close(d); return 0;
    }
    return 1;
}
