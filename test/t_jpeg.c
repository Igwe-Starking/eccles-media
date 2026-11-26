#include "eccles_media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int g_w, g_h; static uint16_t *g_out;
static int rows_cb(void *u, int y, int n, const uint16_t *px, int w) { (void)u; memcpy(g_out + (size_t)y * w, px, (size_t)n * w * 2); return 0; }
static int src_cb(void *u, int y, int n, uint16_t *dst) { (void)u; memcpy(dst, g_out + (size_t)y * g_w, (size_t)n * g_w * 2); return 0; }
static uint8_t *slurp(const char *p, uint32_t *n) { FILE *f = fopen(p, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *b = malloc(*n); if (fread(b, 1, *n, f) != *n) return NULL; fclose(f); return b; }
int main(int argc, char **argv) {
    if (argc >= 5 && !strcmp(argv[1], "dec")) {       // dec in.jpg out.raw scale
        uint32_t n; uint8_t *b = slurp(argv[2], &n); em_membuf_t m; em_io_t io; em_io_mem(&io, &m, b, n, n);
        em_jpeg_t *j; em_img_info_t ii; em_err_t e = em_jpeg_open(&j, &io, &ii);
        if (e) { printf("open err %d\n", e); return 2; }
        int sh = atoi(argv[4]); g_w = (ii.width + (1 << sh) - 1) >> sh; g_h = (ii.height + (1 << sh) - 1) >> sh;
        g_out = calloc((size_t)g_w * g_h, 2);
        clock_t t0 = clock(); e = em_jpeg_decode(j, sh, rows_cb, NULL); double ms = 1000.0 * (clock() - t0) / CLOCKS_PER_SEC;
        if (e) { printf("decode err %d\n", e); return 3; }
        FILE *o = fopen(argv[3], "wb"); fwrite(g_out, 2, (size_t)g_w * g_h, o); fclose(o);
        printf("%dx%d sub=%d ok %.2fms\n", g_w, g_h, ii.subsamp, ms); return 0;
    }
    if (argc >= 8 && !strcmp(argv[1], "enc")) {       // enc w h in.raw out.jpg quality subsamp
        g_w = atoi(argv[2]); g_h = atoi(argv[3]); uint32_t n; g_out = (uint16_t *)slurp(argv[4], &n);
        uint8_t *buf = malloc(8 << 20); em_membuf_t m; em_io_t io; em_io_mem(&io, &m, buf, 0, 8 << 20);
        clock_t t0 = clock(); em_err_t e = em_jpeg_encode(&io, g_w, g_h, atoi(argv[6]), atoi(argv[7]), src_cb, NULL); double ms = 1000.0 * (clock() - t0) / CLOCKS_PER_SEC;
        if (e) { printf("enc err %d\n", e); return 2; }
        FILE *o = fopen(argv[5], "wb"); fwrite(buf, 1, m.len, o); fclose(o);
        printf("encoded %u bytes %.2fms\n", m.len, ms); return 0;
    }
    return 1;
}
