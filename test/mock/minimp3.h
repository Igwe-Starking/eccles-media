// MOCK of the minimp3 API used by em_mp3.c, for testing the adapter's framing logic only.
#include <stdint.h>
#include <string.h>
#define MINIMP3_MAX_SAMPLES_PER_FRAME (1152 * 2)
typedef int16_t mp3d_sample_t;
typedef struct { int frame_bytes, frame_offset, channels, hz, layer, bitrate_kbps; } mp3dec_frame_info_t;
typedef struct { int n; } mp3dec_t;
static void mp3dec_init(mp3dec_t *d) { d->n = 0; }
static int mock_hdr(const uint8_t *b, int len, int *fb) {
    if (len < 4 || b[0] != 0xFF || (b[1] & 0xFE) != 0xFA) return 0;           // MPEG1 Layer III
    static const int br[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    int bitrate = br[b[2] >> 4] * 1000, pad = (b[2] >> 1) & 1;
    if (!bitrate) return 0;
    *fb = 144 * bitrate / 44100 + pad; return 1;
}
static int mp3dec_decode_frame(mp3dec_t *d, const uint8_t *mp3, int bytes, mp3d_sample_t *pcm, mp3dec_frame_info_t *fi) {
    memset(fi, 0, sizeof *fi);
    int off = 0, fb = 0;
    while (off + 4 <= bytes && !mock_hdr(mp3 + off, bytes - off, &fb)) off++;       // resync like the real one
    if (off + 4 > bytes || off + fb > bytes) { fi->frame_bytes = off; return 0; }
    fi->frame_bytes = off + fb; fi->channels = 2; fi->hz = 44100; fi->layer = 3; fi->frame_offset = off;
    if (pcm) for (int i = 0; i < 1152 * 2; i++) pcm[i] = (mp3d_sample_t)(d->n & 0x7FFF);
    if (pcm) d->n++;
    return 1152;
}
