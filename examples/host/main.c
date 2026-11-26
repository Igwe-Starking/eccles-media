/* eccles_media on your PC: encode a JPEG into memory and decode it back. */
#include <stdio.h>
#include "eccles_media.h"

static int gradient(void *user, int y, int nrows, uint16_t *dst) {
    (void)user;
    for (int r = 0; r < nrows; r++)
        for (int x = 0; x < 320; x++)
            dst[r * 320 + x] = (uint16_t)(((x >> 4) << 11) | (((y + r) >> 2) << 5));
    return 0;
}

static int count_rows(void *user, int y, int nrows, const uint16_t *px, int width) {
    (void)y; (void)px; (void)width;
    *(int *)user += nrows;
    return 0;
}

int main(void) {
    static uint8_t file[64 * 1024];
    em_membuf_t mb;
    em_io_t io;
    int rows = 0;

    printf("eccles_media %s\n", ECCLES_MEDIA_VERSION_STRING);

    em_io_mem(&io, &mb, file, 0, sizeof file);
    if (em_jpeg_encode(&io, 320, 240, 85, 3, gradient, NULL) != EM_OK) return 1;

    em_io_mem(&io, &mb, file, mb.len, sizeof file);
    em_jpeg_t *jpg;
    em_img_info_t info;
    if (em_jpeg_open(&jpg, &io, &info) != EM_OK) return 1;
    em_err_t e = em_jpeg_decode(jpg, 0, count_rows, &rows);
    em_jpeg_close(jpg);
    if (e != EM_OK) return 1;

    printf("encoded %ux%u JPEG, decoded %d rows\n", info.width, info.height, rows);
    return 0;
}
