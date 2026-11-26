#include "eccles_media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    // ID3v2 (junk tag) + 200 CBR frames (128 kbps/44.1k => 417/418 bytes) + a stretch of garbage mid-stream
    uint32_t cap = 400000; uint8_t *f = calloc(1, cap); uint32_t n = 0;
    memcpy(f, "ID3\x03\x00\x00", 6); f[6] = 0; f[7] = 0; f[8] = 0x10; f[9] = 0x00; n = 10 + (0x10 << 7);   // synchsafe size 2048
    memset(f + 10, 0xFF, 2048);                                                                         // tag body full of 0xFF: must be skipped, not mis-synced
    int frames = 200;
    for (int i = 0; i < frames; i++) {
        int pad = (i % 7) == 0; int fb = 144 * 128000 / 44100 + pad;
        f[n] = 0xFF; f[n + 1] = 0xFB; f[n + 2] = (uint8_t)(0x90 | (pad << 1)); f[n + 3] = 0x00; n += fb;
        if (i == 100) { for (int k = 0; k < 333; k++) f[n++] = (uint8_t)(k * 13); }                   // junk between frames
    }
    em_membuf_t mb; em_io_t io; em_io_mem(&io, &mb, f, n, cap);
    em_mp3_t *m; em_audio_info_t ai; em_err_t e = em_mp3_open(&m, &io, &ai);
    printf("open: err=%d rate=%u ch=%u\n", e, ai.sample_rate, ai.channels);
    if (e) return 1;
    static int16_t pcm[2304]; int got = 0, total = 0; while ((got = em_mp3_read(m, pcm, 1152)) > 0) total += got;
    printf("decoded %d frames -> %d samples/ch (expected %d)\n", total / 1152, total, frames * 1152);
    em_mp3_close(m);
    int ok = e == 0 && total == frames * 1152 && ai.sample_rate == 44100 && ai.channels == 2;
    printf(ok ? "MP3 adapter (mock decoder): ok\n" : "MP3 adapter (mock decoder): FAILED\n"); return !ok;
}
