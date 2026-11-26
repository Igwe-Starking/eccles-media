#include "eccles_media.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(int argc, char **argv) {
    // 1) PCM16 stereo write -> read back exact
    enum { N = 20000 };
    static int16_t src[N * 2], dst[(N + 2000) * 2];
    for (int i = 0; i < N; i++) { src[2*i] = (int16_t)(12000 * sin(i * 0.05)); src[2*i+1] = (int16_t)(9000 * sin(i * 0.031 + 1)); }
    FILE *f = fopen("/tmp/t_pcm.wav", "wb+"); em_io_t io; em_io_file(&io, f);
    em_wav_t *w; CHECK(em_wav_create(&w, &io, 22050, 2, 0) == EM_OK, "create");
    for (int o = 0; o < N; o += 1000) CHECK(em_wav_write(w, src + o * 2, 1000) == 1000, "write");
    CHECK(em_wav_finish(w) == EM_OK, "finish"); fclose(f);
    f = fopen("/tmp/t_pcm.wav", "rb"); em_io_file(&io, f);
    em_audio_info_t ai; CHECK(em_wav_open(&w, &io, &ai) == EM_OK, "open");
    CHECK(ai.sample_rate == 22050 && ai.channels == 2 && ai.total_frames == N, "info %u %u %u", ai.sample_rate, ai.channels, ai.total_frames);
    int got = 0, n; while ((n = em_wav_read(w, dst + got * 2, 777)) > 0) got += n;
    CHECK(got == N && !memcmp(src, dst, sizeof src), "pcm roundtrip got=%d", got);
    CHECK(em_wav_seek(w, 5000) == EM_OK && em_wav_read(w, dst, 100) == 100 && !memcmp(dst, src + 10000, 400), "seek");
    em_wav_close(w); fclose(f);

    // 2) IMA ADPCM write -> read back, check SNR; ffmpeg cross-check done by script
    f = fopen("/tmp/t_ima.wav", "wb+"); em_io_file(&io, f);
    CHECK(em_wav_create(&w, &io, 22050, 2, 1) == EM_OK, "create ima");
    for (int o = 0; o < N; o += 1000) em_wav_write(w, src + o * 2, 1000);
    CHECK(em_wav_finish(w) == EM_OK, "finish ima"); fclose(f);
    f = fopen("/tmp/t_ima.wav", "rb"); em_io_file(&io, f);
    CHECK(em_wav_open(&w, &io, &ai) == EM_OK, "open ima");
    got = 0; while ((n = em_wav_read(w, dst + got * 2, 333)) > 0) got += n;
    double se = 0, sn = 0; for (int i = 0; i < N * 2 && i < got * 2; i++) { double d = dst[i] - src[i]; se += d * d; sn += (double)src[i] * src[i]; }
    double snr = 10 * log10(sn / se);
    printf("ima: frames=%d snr=%.1f dB\n", got, snr);
    CHECK(got >= N && snr > 30, "ima snr");
    em_wav_close(w); fclose(f);

    // 3) PCM8 mono read of an ffmpeg-made file, if supplied
    if (argc > 1) {
        f = fopen(argv[1], "rb"); em_io_file(&io, f);
        CHECK(em_wav_open(&w, &io, &ai) == EM_OK, "open ext %s", argv[1]);
        int16_t *buf = malloc(100000 * 4); got = 0;
        while ((n = em_wav_read(w, buf + got * ai.channels, 1000)) > 0) got += n;
        printf("ext: rate=%u ch=%u frames=%d\n", ai.sample_rate, ai.channels, got);
        FILE *o = fopen(argc > 2 ? argv[2] : "/tmp/ext.raw", "wb"); fwrite(buf, 2 * ai.channels, got, o); fclose(o);
        em_wav_close(w); fclose(f);
    }
    printf(fails ? "WAV: %d FAILED\n" : "WAV: all ok\n", fails);
    return fails != 0;
}
