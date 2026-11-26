// Measures peak heap used by each operation with a counting allocator.
#include "eccles_media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t live, peak; static int allocs;
typedef struct { size_t n; } hdr_t;
static void *c_alloc(size_t n, int f) { (void)f; hdr_t *h = malloc(sizeof(hdr_t) + 16 + n); if (!h) return NULL; h->n = n; live += n; if (live > peak) peak = live; allocs++; return (uint8_t *)h + 16 + sizeof(hdr_t) - sizeof(hdr_t) ; }
static void c_free(void *p) { if (!p) return; hdr_t *h = (hdr_t *)((uint8_t *)p - 16); live -= h->n; free(h); }
static uint8_t *slurp(const char *p, uint32_t *n) { FILE *f = fopen(p, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *b = malloc(*n); if (fread(b, 1, *n, f) != *n) return NULL; fclose(f); return b; }
static int sink_rows(void *u, int y, int n, const uint16_t *p, int w) { (void)u; (void)y; (void)n; (void)p; (void)w; return 0; }
static int src_rows(void *u, int y, int n, uint16_t *d) { int w = *(int *)u; for (int i = 0; i < n * w; i++) d[i] = (uint16_t)((i * 31 + y * 7) ^ (i >> 3)); return 0; }
#define REPORT(name) printf("  %-44s peak %6.1f KB   (%d allocations)\n", name, peak / 1024.0, allocs)
#define RESET() do { live = peak = 0; allocs = 0; } while (0)

int main(void) {
    em_set_allocator(c_alloc, c_free);
    uint32_t n; uint8_t *b; em_membuf_t mb; em_io_t io;
    printf("Peak heap per operation (exact, counting allocator; excludes caller's own file/frame buffers):\n");
    b = slurp("/tmp/j_420_q85.jpg", &n); em_io_mem(&io, &mb, b, n, n);
    for (int sh = 0; sh <= 3; sh++) { RESET(); em_jpeg_t *j; em_img_info_t ii; em_io_mem(&io, &mb, b, n, n); em_jpeg_open(&j, &io, &ii); em_jpeg_decode(j, sh, sink_rows, NULL); em_jpeg_close(j);
        char s[96]; snprintf(s, sizeof s, "JPEG decode 320x240 4:2:0, scale 1/%d", 1 << sh); REPORT(s); }
    { RESET(); em_jpeg_t *j; em_img_info_t ii; em_io_mem(&io, &mb, b, n, n); em_jpeg_open(&j, &io, &ii); em_jpeg_decode(j, 0, sink_rows, NULL);
      for (int k = 0; k < 10; k++) { em_io_mem(&io, &mb, b, n, n); em_jpeg_reset(j, &io, &ii); em_jpeg_decode(j, 0, sink_rows, NULL); } em_jpeg_close(j);
      REPORT("  ... 11 frames decoded via em_jpeg_reset"); }
    free(b);
    { int w = 320; uint8_t *out = malloc(1 << 20); em_membuf_t om; em_io_t oio;
      RESET(); em_io_mem(&oio, &om, out, 0, 1 << 20); em_jpeg_encode(&oio, 320, 240, 80, 3, src_rows, &w); REPORT("JPEG encode 320x240 4:2:0");
      RESET(); em_io_mem(&oio, &om, out, 0, 1 << 20); em_png_encode(&oio, 320, 240, src_rows, &w); REPORT("PNG encode 320x240 (4 KB window)");
      RESET(); em_io_mem(&oio, &om, out, 0, 1 << 20); em_png_encode(&oio, 320, 240, src_rows, &w); (void)0;
      free(out); }
    b = slurp("/tmp/p_rgb250x100.png", &n); { RESET(); em_png_t *p; em_img_info_t ii; em_io_mem(&io, &mb, b, n, n); em_png_open(&p, &io, &ii); em_png_decode(p, 0xFFFF, sink_rows, NULL); em_png_close(p); REPORT("PNG decode 250x100 (incl. 32 KB inflate window)"); } free(b);
    { RESET(); em_wav_t *w; em_audio_info_t ai; b = slurp("/tmp/t_pcm.wav", &n); em_io_mem(&io, &mb, b, n, n); em_wav_open(&w, &io, &ai); em_wav_close(w); REPORT("WAV PCM read handle"); free(b);
      RESET(); b = slurp("/tmp/t_ima.wav", &n); em_io_mem(&io, &mb, b, n, n); em_wav_open(&w, &io, &ai); em_wav_close(w); REPORT("WAV IMA-ADPCM read handle"); free(b); }
    b = slurp("/tmp/m_ours.avi", &n); { RESET(); em_demux_t *d; em_media_info_t mi; em_io_mem(&io, &mb, b, n, n); em_demux_open(&d, &io, &mi); em_demux_close(d); REPORT("AVI demux handle"); } free(b);
    b = slurp("/tmp/m_ours.mp4", &n); { RESET(); em_demux_t *d; em_media_info_t mi; em_io_mem(&io, &mb, b, n, n); em_demux_open(&d, &io, &mi); em_demux_close(d); REPORT("MP4/3GP demux handle (tables stay in the file)"); } free(b);
    { RESET(); static uint8_t o[1 << 20]; em_membuf_t om; em_io_t oio; em_io_mem(&oio, &om, o, 0, sizeof o); em_media_info_t mi; memset(&mi, 0, sizeof mi); mi.has_video = 1; mi.width = 320; mi.height = 240; mi.vcodec = EM_VC_MJPEG; mi.fps_x1000 = 15000;
      em_mux_t *m; em_mux_create(&m, &oio, EM_FMT_MP4, &mi, 36000); REPORT("MP4 mux, index for 36000 packets (~10 min)"); em_mux_finish(m); }
    // full AVI playback incl. player staging + JPEG handle
    { RESET(); b = slurp("/tmp/m_ours.avi", &n); em_io_mem(&io, &mb, b, n, n);
      em_sink_t sk = {0}; sk.video_rows = sink_rows; em_player_t *p; em_player_open(&p, &io, &sk, NULL); while (em_player_step(p) == EM_OK) {} em_player_close(p); free(b); REPORT("Player: 160x120 MJPEG AVI (video only)"); }
    return 0;
}
