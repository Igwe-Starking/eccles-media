// em_port_esp.h - ESP-IDF glue for eccles_media: DMA audio out (I2S), panel blit,
// player task, camera/mic recording. Everything privacy-sensitive goes through
// media_hal_* so the host-drawn camera/mic indicators cannot be skipped.
#ifndef EM_PORT_ESP_H
#define EM_PORT_ESP_H

#include "eccles_media.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int i2s_bclk, i2s_ws, i2s_dout;   // speaker/codec I2S TX pins
    int mic_rate;                     // sample rate media_hal_mic_read() delivers (mono PCM16), e.g. 16000
    int fps;                          // nominal recording fps (AVI/MP4 header), e.g. 15
    int media_core;                   // core for the player/recorder tasks (leave WiFi/BT core alone), e.g. 1
    int task_prio;                    // below the WiFi task, e.g. 5
} em_esp_cfg_t;

// Installs the PSRAM-aware allocator and brings up I2S TX with a 6-descriptor DMA ring.
em_err_t em_esp_init(const em_esp_cfg_t *cfg);

// ---- playback (jpg/png stills, wav/mp3, avi/mp4/3gp) ----
typedef void (*em_esp_done_cb)(void *user, em_err_t result);
em_err_t em_esp_play_start(const char *path, int scale_shift, em_esp_done_cb cb, void *user);
void     em_esp_play_stop(void);
uint32_t em_esp_play_pos_ms(void);
int      em_esp_playing(void);

// ---- capture ----
// JPEG: the sensor's own hardware JPEG is written to the file untouched (zero codec CPU).
em_err_t em_esp_capture_jpeg(const char *path);
// PNG: camera JPEG is decoded (scale_shift) into a PSRAM frame, then PNG-encoded.
em_err_t em_esp_capture_png(const char *path, int scale_shift);

// ---- recording (container chosen from the extension: .avi .mp4 .3gp, MJPEG + PCM16) ----
em_err_t em_esp_record_start(const char *path, int with_audio);
em_err_t em_esp_record_stop(void);
// audio only: .wav, adpcm=1 stores IMA-ADPCM (4:1 smaller, ~1/4 the SD write traffic)
em_err_t em_esp_record_audio_start(const char *path, int adpcm);

#ifdef __cplusplus
}
#endif
#endif
