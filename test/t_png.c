#include "eccles_media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_w, g_h; static uint16_t *g_out;
static int rows_cb(void *u, int y, int n, const uint16_t *px, int w) { (void)u; memcpy(g_out + (size_t)y * w, px, (size_t)n * w * 2); return 0; }
static int src_cb(void *u, int y, int n, uint16_t *dst) { (void)u; memcpy(dst, g_out + (size_t)y * g_w, (size_t)n * g_w * 2); return 0; }

static uint8_t *slurp(const char *p, uint32_t *n) { FILE *f = fopen(p, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *b = malloc(*n); if (!b || fread(b, 1, *n, f) != *n) { fclose(f); return NULL; } fclose(f); return b; }

int main(int argc, char **argv) {
    // decode: png path -> raw RGB565 dump
    if (argc >= 3 && !strcmp(argv[1], "dec")) {
        uint32_t n; uint8_t *b = slurp(argv[2], &n); em_membuf_t m; em_io_t io; em_io_mem(&io, &m, b, n, n);
        em_png_t *p; em_img_info_t ii; em_err_t e = em_png_open(&p, &io, &ii);
        if (e) { printf("open err %d\n", e); return 2; }
        g_w = ii.width; g_h = ii.height; g_out = malloc((size_t)g_w * g_h * 2);
        e = em_png_decode(p, 0xFFFF, rows_cb, NULL);
        if (e) { printf("decode err %d\n", e); return 3; }
        FILE *o = fopen(argv[3], "wb"); fwrite(g_out, 2, (size_t)g_w * g_h, o); fclose(o);
        printf("%dx%d ok\n", g_w, g_h); return 0;
    }
    // encode: raw RGB565 (w h file) -> png
    if (argc >= 6 && !strcmp(argv[1], "enc")) {
        g_w = atoi(argv[2]); g_h = atoi(argv[3]); uint32_t n; g_out = (uint16_t *)slurp(argv[4], &n);
        uint8_t *buf = malloc(4 << 20); em_membuf_t m; em_io_t io; em_io_mem(&io, &m, buf, 0, 4 << 20);
        em_err_t e = em_png_encode(&io, g_w, g_h, src_cb, NULL);
        if (e) { printf("enc err %d\n", e); return 2; }
        FILE *o = fopen(argv[5], "wb"); fwrite(buf, 1, m.len, o); fclose(o);
        printf("encoded %u bytes (raw %u)\n", m.len, g_w * g_h * 3); return 0;
    }
    return 1;
}
