// em_port_esp.c - ESP-IDF 5.x backend. NOT compiled in the sandbox this was written in (no
// xtensa toolchain there): review against your IDF version before first flash.
#include "em_port_esp.h"
#include "media_hal.h"
#include "panel_hal.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"

static const char *TAG = "eccles_media";

static em_esp_cfg_t s_cfg;
static i2s_chan_handle_t s_tx;
static uint32_t s_tx_rate;
static int s_inited;

// ---- allocator: big, CPU-cold buffers (frames, windows, index) -> PSRAM; hot state -> internal ----
static void *esp_alloc(size_t n, int flags) {
    void *p = NULL;
    if (flags == EM_MEM_BIG) p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    return p;
}
static void esp_release(void *p) { heap_caps_free(p); }

em_err_t em_esp_init(const em_esp_cfg_t *cfg) {
    if (!cfg) return EM_ERR_ARG;
    s_cfg = *cfg;
    if (!s_cfg.fps) s_cfg.fps = 15;
    if (!s_cfg.mic_rate) s_cfg.mic_rate = 16000;
    if (!s_cfg.task_prio) s_cfg.task_prio = 5;
    em_set_allocator(esp_alloc, esp_release);
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    cc.dma_desc_num = 6;          // DMA ring: the CPU only refills descriptors, never bit-bangs samples
    cc.dma_frame_num = 240;
    cc.auto_clear = true;         // underrun => silence, not a stuck buffer
    if (i2s_new_channel(&cc, &s_tx, NULL) != ESP_OK) return EM_ERR_IO;
    s_inited = 1;
    return EM_OK;
}

static int tx_configure(uint32_t rate) {
    if (!s_inited) return -1;
    if (s_tx_rate == rate) return 0;
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = s_cfg.i2s_bclk, .ws = s_cfg.i2s_ws, .dout = s_cfg.i2s_dout, .din = I2S_GPIO_UNUSED},
    };
    if (s_tx_rate) { i2s_channel_disable(s_tx); if (i2s_channel_reconfig_std_clock(s_tx, &sc.clk_cfg) != ESP_OK) return -1; }
    else if (i2s_channel_init_std_mode(s_tx, &sc) != ESP_OK) return -1;
    if (i2s_channel_enable(s_tx) != ESP_OK) return -1;
    s_tx_rate = rate;
    return 0;
}

// ---- sinks -------------------------------------------------------------------------------------------
typedef struct {
    int ch; int x0, y0, w;
    int16_t st[512];               // mono->stereo scratch (256 frames)
} sink_t;

static int snk_audio_config(void *u, uint32_t rate, int ch) { sink_t *s = u; s->ch = ch; return tx_configure(rate); }
static int snk_audio_write(void *u, const int16_t *pcm, int frames) {
    sink_t *s = u;
    while (frames > 0) {
        const int16_t *src = pcm; int n = frames; size_t bytes, wr;
        if (s->ch == 1) {
            n = frames > 256 ? 256 : frames;
            for (int i = 0; i < n; i++) s->st[2 * i] = s->st[2 * i + 1] = pcm[i];
            src = s->st;
        }
        bytes = (size_t)n * 4;
        // blocks while all 6 DMA descriptors are full: this is the player's pacing/back-pressure
        if (i2s_channel_write(s_tx, src, bytes, &wr, portMAX_DELAY) != ESP_OK) return -1;
        pcm += n; frames -= n;
    }
    return 0;
}
static void snk_video_begin(void *u, int w, int h) {
    sink_t *s = u;
    s->w = w;
    s->x0 = w < PANEL_WIDTH ? (PANEL_WIDTH - w) / 2 : 0;
    s->y0 = PANEL_STATUS_BAR_H + (h < PANEL_APP_AREA_H ? (PANEL_APP_AREA_H - h) / 2 : 0);
}
static int snk_video_rows(void *u, int y, int n, const uint16_t *px, int w) {
    sink_t *s = u;
    int ww = w > PANEL_WIDTH ? PANEL_WIDTH : w;
    if (w > PANEL_WIDTH) return 0;      // caller should have picked a scale; clipping rows would corrupt the stride
    // panel_hal_blit() is the SPI/QSPI DMA path; the decoder keeps this band valid until it has returned
    // twice, so an asynchronous blit that finishes within one band-decode time is safe.
    panel_hal_blit(s->x0, s->y0 + y, ww, n, px);
    taskYIELD();
    return 0;
}
static void snk_sleep(void *u, uint32_t ms) { (void)u; vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1)); }
static uint32_t snk_now(void *u) { (void)u; return (uint32_t)(esp_timer_get_time() / 1000); }

// ---- player task -----------------------------------------------------------------------------------------
static struct {
    TaskHandle_t task; volatile int stop, running;
    char path[96]; int scale; em_esp_done_cb cb; void *user;
    em_player_t *player;
} s_play;

static int start_task(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, TaskHandle_t *out) {
#if CONFIG_FREERTOS_UNICORE
    return xTaskCreate(fn, name, stack, arg, s_cfg.task_prio, out) == pdPASS ? 0 : -1;
#else
    return xTaskCreatePinnedToCore(fn, name, stack, arg, s_cfg.task_prio, out, s_cfg.media_core) == pdPASS ? 0 : -1;
#endif
}

static void play_task(void *arg) {
    (void)arg;
    em_err_t res = EM_OK;
    FILE *f = fopen(s_play.path, "rb");
    static sink_t sink;
    memset(&sink, 0, sizeof sink);
    if (!f) { res = EM_ERR_IO; goto out; }
    static char iobuf[4096];
    setvbuf(f, iobuf, _IOFBF, sizeof iobuf);          // our readers do small reads; coalesce them
    em_io_t io; em_io_file(&io, f);
    em_sink_t sk = {snk_audio_config, snk_audio_write, snk_video_rows, snk_video_begin, snk_sleep, snk_now, &sink};
    em_player_info_t info;
    res = em_player_open(&s_play.player, &io, &sk, &info);
    if (res) goto closefile;
    em_player_set_scale(s_play.player, s_play.scale);
    while (!s_play.stop) {
        res = em_player_step(s_play.player);
        if (res != EM_OK) break;
    }
    if (res == EM_EOF) res = EM_OK;
    if (info.has_audio && s_tx) { /* let the DMA ring drain, then it auto-clears to silence */ vTaskDelay(pdMS_TO_TICKS(120)); }
    em_player_close(s_play.player); s_play.player = NULL;
closefile:
    fclose(f);
out:
    s_play.running = 0;
    if (s_play.cb) s_play.cb(s_play.user, res);
    s_play.task = NULL;
    vTaskDelete(NULL);
}

em_err_t em_esp_play_start(const char *path, int scale_shift, em_esp_done_cb cb, void *user) {
    if (!s_inited || !path || s_play.running) return EM_ERR_ARG;
    strlcpy(s_play.path, path, sizeof s_play.path);
    s_play.scale = scale_shift; s_play.cb = cb; s_play.user = user; s_play.stop = 0; s_play.running = 1;
    if (start_task(play_task, "em_play", 6144, NULL, &s_play.task)) { s_play.running = 0; return EM_ERR_NOMEM; }
    return EM_OK;
}
void em_esp_play_stop(void) { s_play.stop = 1; while (s_play.running) vTaskDelay(pdMS_TO_TICKS(10)); }
int em_esp_playing(void) { return s_play.running; }
uint32_t em_esp_play_pos_ms(void) { return s_play.player ? em_player_pos_ms(s_play.player) : 0; }

// ---- capture ------------------------------------------------------------------------------------------------
#define JBUF_SZ (128 * 1024)      // largest JPEG frame we accept from the sensor

static em_err_t jpeg_dims(const uint8_t *j, size_t n, uint16_t *w, uint16_t *h) {
    em_membuf_t mb; em_io_t io; em_io_mem(&io, &mb, (void *)j, (uint32_t)n, (uint32_t)n);
    em_jpeg_t *d; em_img_info_t ii; em_err_t e = em_jpeg_open(&d, &io, &ii);
    if (e) return e;
    *w = ii.width; *h = ii.height; em_jpeg_close(d);
    return EM_OK;
}

em_err_t em_esp_capture_jpeg(const char *path) {
    uint8_t *buf = esp_alloc(JBUF_SZ, EM_MEM_BIG);
    if (!buf) return EM_ERR_NOMEM;
    size_t n = media_hal_camera_capture(buf, JBUF_SZ);      // HAL turns the camera indicator on/off itself
    em_err_t e = n ? EM_OK : EM_ERR_IO;
    if (!e) { FILE *f = fopen(path, "wb"); if (!f || fwrite(buf, 1, n, f) != n) e = EM_ERR_IO; if (f) fclose(f); }
    esp_release(buf);
    return e;
}

typedef struct { uint16_t *px; int w; } frame_t;
static int frame_fill(void *u, int y, int n, const uint16_t *rows, int w) { frame_t *f = u; memcpy(f->px + (size_t)y * w, rows, (size_t)n * w * 2); f->w = w; return 0; }
static int frame_src(void *u, int y, int n, uint16_t *dst) { frame_t *f = u; memcpy(dst, f->px + (size_t)y * f->w, (size_t)n * f->w * 2); return 0; }

em_err_t em_esp_capture_png(const char *path, int scale_shift) {
    uint8_t *buf = esp_alloc(JBUF_SZ, EM_MEM_BIG);
    if (!buf) return EM_ERR_NOMEM;
    size_t n = media_hal_camera_capture(buf, JBUF_SZ);
    uint16_t w = 0, h = 0; em_err_t e = n ? jpeg_dims(buf, n, &w, &h) : EM_ERR_IO;
    frame_t fr = {0};
    if (!e) {
        int ow = (w + (1 << scale_shift) - 1) >> scale_shift, oh = (h + (1 << scale_shift) - 1) >> scale_shift;
        fr.px = esp_alloc((size_t)ow * oh * 2, EM_MEM_BIG);
        if (!fr.px) e = EM_ERR_NOMEM;
        else {
            em_membuf_t mb; em_io_t io; em_io_mem(&io, &mb, buf, (uint32_t)n, (uint32_t)n);
            em_jpeg_t *d; em_img_info_t ii;
            e = em_jpeg_open(&d, &io, &ii);
            if (!e) { e = em_jpeg_decode(d, scale_shift, frame_fill, &fr); em_jpeg_close(d); }
            if (!e) {
                FILE *f = fopen(path, "wb");
                if (!f) e = EM_ERR_IO;
                else { em_io_t out; em_io_file(&out, f); e = em_png_encode(&out, ow, oh, frame_src, &fr); fclose(f); }
            }
        }
    }
    esp_release(fr.px); esp_release(buf);
    return e;
}

// ---- recorder -------------------------------------------------------------------------------------------------------
static struct {
    TaskHandle_t task; volatile int stop, running; em_err_t result;
    char path[96]; int with_audio, adpcm, audio_only;
} s_rec;

static void seg_name(char *dst, size_t n, const char *path, int seg) {
    if (!seg) { strlcpy(dst, path, n); return; }
    const char *dot = strrchr(path, '.');
    size_t base = dot ? (size_t)(dot - path) : strlen(path);
    snprintf(dst, n, "%.*s-%03d%s", (int)base, path, seg, dot ? dot : "");
}

static em_fmt_t fmt_from_ext(const char *p) {
    const char *dot = strrchr(p, '.');
    if (dot && !strcasecmp(dot, ".mp4")) return EM_FMT_MP4;
    if (dot && !strcasecmp(dot, ".3gp")) return EM_FMT_3GP;
    return EM_FMT_AVI;
}

static void rec_video_task(void *arg) {
    (void)arg;
    uint8_t *jbuf = esp_alloc(JBUF_SZ, EM_MEM_BIG);
    static int16_t pcm[2048];
    static char iobuf[8192];
    em_err_t e = jbuf ? EM_OK : EM_ERR_NOMEM;
    em_mux_t *mux = NULL; FILE *f = NULL; int seg = 0;
    int64_t t0 = 0; uint16_t w = 0, h = 0;
    const uint32_t max_packets = 36000;                 // 12 B each = 432 KB of PSRAM, ~10 min at 30 fps + audio
    while (!e && !s_rec.stop) {
        size_t n = media_hal_camera_capture(jbuf, JBUF_SZ);      // indicator handled inside the HAL
        if (!n) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        if (!mux) {
            if ((e = jpeg_dims(jbuf, n, &w, &h))) break;
            char name[112]; seg_name(name, sizeof name, s_rec.path, seg++);
            f = fopen(name, "wb+"); if (!f) { e = EM_ERR_IO; break; }
            setvbuf(f, iobuf, _IOFBF, sizeof iobuf);
            static em_io_t io; em_io_file(&io, f);
            em_media_info_t mi; memset(&mi, 0, sizeof mi);
            mi.has_video = 1; mi.width = w; mi.height = h; mi.fps_x1000 = (uint32_t)s_cfg.fps * 1000; mi.vcodec = EM_VC_MJPEG;
            if (s_rec.with_audio) { mi.has_audio = 1; mi.sample_rate = (uint32_t)s_cfg.mic_rate; mi.channels = 1; mi.acodec = EM_AC_PCM16; }
            if ((e = em_mux_create(&mux, &io, fmt_from_ext(s_rec.path), &mi, max_packets))) break;
            t0 = esp_timer_get_time();
        }
        uint32_t pts = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        e = em_mux_video(mux, jbuf, (uint32_t)n, pts);
        if (e == EM_OK && s_rec.with_audio) {
            size_t b = media_hal_mic_read((uint8_t *)pcm, sizeof pcm);
            if (b >= 2) e = em_mux_audio(mux, pcm, (uint32_t)(b / 2));
        }
        if (e == EM_ERR_FULL) {                           // index full: close this segment, roll to the next file
            e = em_mux_finish(mux); mux = NULL; fclose(f); f = NULL;
        }
        vTaskDelay(1);                                    // let IDLE/WiFi run; frame cadence is set by the sensor
    }
    if (mux) { em_err_t fe = em_mux_finish(mux); if (!e) e = fe; }
    if (f) fclose(f);
    esp_release(jbuf);
    s_rec.result = e; s_rec.running = 0; s_rec.task = NULL;
    vTaskDelete(NULL);
}

static void rec_audio_task(void *arg) {
    (void)arg;
    static int16_t pcm[2048]; static char iobuf[8192];
    em_err_t e = EM_OK;
    FILE *f = fopen(s_rec.path, "wb+");
    em_wav_t *w = NULL;
    if (!f) e = EM_ERR_IO;
    else {
        setvbuf(f, iobuf, _IOFBF, sizeof iobuf);
        static em_io_t io; em_io_file(&io, f);
        e = em_wav_create(&w, &io, (uint32_t)s_cfg.mic_rate, 1, s_rec.adpcm);
    }
    while (!e && !s_rec.stop) {
        size_t b = media_hal_mic_read((uint8_t *)pcm, sizeof pcm);      // HAL owns the mic indicator
        if (b >= 2) { if (em_wav_write(w, pcm, (int)(b / 2)) < 0) e = EM_ERR_IO; }
        else vTaskDelay(pdMS_TO_TICKS(5));
        vTaskDelay(1);
    }
    if (w) { em_err_t fe = em_wav_finish(w); if (!e) e = fe; }
    if (f) fclose(f);
    s_rec.result = e; s_rec.running = 0; s_rec.task = NULL;
    vTaskDelete(NULL);
}

em_err_t em_esp_record_start(const char *path, int with_audio) {
    if (!s_inited || !path || s_rec.running) return EM_ERR_ARG;
    strlcpy(s_rec.path, path, sizeof s_rec.path);
    s_rec.with_audio = with_audio; s_rec.stop = 0; s_rec.running = 1; s_rec.result = EM_OK;
    if (start_task(rec_video_task, "em_rec", 6144, NULL, &s_rec.task)) { s_rec.running = 0; return EM_ERR_NOMEM; }
    return EM_OK;
}
em_err_t em_esp_record_audio_start(const char *path, int adpcm) {
    if (!s_inited || !path || s_rec.running) return EM_ERR_ARG;
    strlcpy(s_rec.path, path, sizeof s_rec.path);
    s_rec.adpcm = adpcm; s_rec.stop = 0; s_rec.running = 1; s_rec.result = EM_OK;
    if (start_task(rec_audio_task, "em_arec", 4096, NULL, &s_rec.task)) { s_rec.running = 0; return EM_ERR_NOMEM; }
    return EM_OK;
}
em_err_t em_esp_record_stop(void) {
    s_rec.stop = 1;
    while (s_rec.running) vTaskDelay(pdMS_TO_TICKS(10));
    return s_rec.result;
}
