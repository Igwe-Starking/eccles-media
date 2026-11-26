// eccles_media.h - ultra-light media stack for the Eccles Phone.
//
// Design rules (these are what keep it light):
//   * No global state, no hidden threads. Every call is cooperative: the
//     caller (a task in port/, or a test) decides when work happens.
//   * No allocation after open(). Each handle allocates ONE block up front
//     through em_alloc() and frees it on close(), so RAM use is known and
//     fragmentation-free. Large blocks can be routed to PSRAM by the port.
//   * Streaming everywhere. Images are decoded/encoded in bands of rows,
//     video is demuxed one packet at a time, audio one buffer at a time.
//     Nothing ever holds a whole frame or file.
//   * Sinks/sources are callbacks, so DMA is the caller's side of the
//     contract: rows are double-buffered, so a DMA transfer of band N can
//     run while band N+1 is being decoded (see port/em_port_esp.c).
//   * Pixels are RGB565, native-endian uint16_t (same as panel_hal_blit).
#ifndef ECCLES_MEDIA_H
#define ECCLES_MEDIA_H

#include <stdint.h>
#include <stddef.h>

#define ECCLES_MEDIA_VERSION_MAJOR 1
#define ECCLES_MEDIA_VERSION_MINOR 0
#define ECCLES_MEDIA_VERSION_PATCH 0
#define ECCLES_MEDIA_VERSION_STRING "1.0.0"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EM_OK = 0,
    EM_EOF = 1,
    EM_ERR_IO = -1,
    EM_ERR_FORMAT = -2,
    EM_ERR_UNSUPPORTED = -3,
    EM_ERR_NOMEM = -4,
    EM_ERR_ARG = -5,
    EM_ERR_FULL = -6,
    EM_ERR_ABORT = -7
} em_err_t;

// ---- memory ---------------------------------------------------------------
// Flags let the port place big, CPU-cold buffers in PSRAM and small hot ones
// (Huffman tables, bit buffers) in internal RAM. Default = malloc/free.
#define EM_MEM_FAST 0   // internal RAM preferred
#define EM_MEM_BIG  1   // PSRAM fine
void em_set_allocator(void *(*alloc)(size_t n, int flags), void (*free_fn)(void *p));
void *em_alloc(size_t n, int flags);
void  em_free(void *p);

// ---- byte I/O ---------------------------------------------------------------
typedef struct em_io {
    int      (*read)(void *ctx, void *dst, uint32_t n);        // bytes read (<n at EOF), <0 error
    int      (*write)(void *ctx, const void *src, uint32_t n); // bytes written, <0 error
    int      (*seek)(void *ctx, uint32_t abs_off);             // 0 ok
    uint32_t (*tell)(void *ctx);
    void *ctx;
} em_io_t;

typedef struct { uint8_t *buf; uint32_t len, cap, pos; } em_membuf_t;
void em_io_mem(em_io_t *io, em_membuf_t *m, void *buf, uint32_t len, uint32_t cap);
#ifndef EM_NO_STDIO
#include <stdio.h>
void em_io_file(em_io_t *io, FILE *f);   // works with ESP-IDF VFS (LittleFS/FAT/SD)
#endif

// ---- pixel / image types -------------------------------------------------------
typedef struct {
    uint16_t width, height;
    uint8_t  subsamp;   // JPEG: 0 gray, 1 4:4:4, 2 4:2:2, 3 4:2:0 (0 for PNG)
} em_img_info_t;

// Rows callback (decode side): `rgb565` holds `nrows` rows of `width` pixels for
// output rows y..y+nrows-1. The buffer is one of two owned by the decoder and
// stays valid until the callback has returned TWICE after this call - i.e. you
// may start a DMA from it and return immediately. Return non-zero to abort.
typedef int (*em_rows_cb)(void *user, int y, int nrows, const uint16_t *rgb565, int width);
// Source callback (encode side): fill `dst` with rows y..y+nrows-1 (RGB565).
typedef int (*em_src_cb)(void *user, int y, int nrows, uint16_t *dst);

// ---- JPEG (baseline + extended sequential, Huffman; no progressive/arith) -----
typedef struct em_jpeg em_jpeg_t;
em_err_t em_jpeg_open(em_jpeg_t **out, em_io_t *io, em_img_info_t *info);
// scale_shift 0..3 -> output is 1/1, 1/2, 1/4, 1/8 size (8 = DC-only, very fast).
em_err_t em_jpeg_decode(em_jpeg_t *j, int scale_shift, em_rows_cb cb, void *user);
void     em_jpeg_close(em_jpeg_t *j);
// Re-point an open decoder at a new JPEG (e.g. next video frame) without reallocating.
em_err_t em_jpeg_reset(em_jpeg_t *j, em_io_t *io, em_img_info_t *info);
// subsamp: 0 gray, 1 4:4:4, 3 4:2:0. quality 1..100. Streaming: ~(16*w*2 + 2KB) RAM.
em_err_t em_jpeg_encode(em_io_t *out, int w, int h, int quality, int subsamp,
                        em_src_cb src, void *user);

// ---- PNG (decode: non-interlaced, all colour types; encode: 8-bit RGB) ---------
typedef struct em_png em_png_t;
em_err_t em_png_open(em_png_t **out, em_io_t *io, em_img_info_t *info);
// Transparent pixels are composited onto bg565.
em_err_t em_png_decode(em_png_t *p, uint16_t bg565, em_rows_cb cb, void *user);
void     em_png_close(em_png_t *p);
em_err_t em_png_encode(em_io_t *out, int w, int h, em_src_cb src, void *user);

// ---- Audio ----------------------------------------------------------------------
typedef struct {
    uint32_t sample_rate;
    uint8_t  channels;       // 1 or 2
    uint32_t total_frames;   // 0 if unknown
} em_audio_info_t;

// WAV: PCM 8/16-bit and IMA-ADPCM read; PCM16 and IMA-ADPCM write.
typedef struct em_wav em_wav_t;
em_err_t em_wav_open(em_wav_t **out, em_io_t *io, em_audio_info_t *info);
int      em_wav_read(em_wav_t *w, int16_t *pcm, int max_frames);   // frames, 0 = EOF, <0 err
em_err_t em_wav_seek(em_wav_t *w, uint32_t frame);
void     em_wav_close(em_wav_t *w);
em_err_t em_wav_create(em_wav_t **out, em_io_t *io, uint32_t rate, int channels, int adpcm);
int      em_wav_write(em_wav_t *w, const int16_t *pcm, int frames);
em_err_t em_wav_finish(em_wav_t *w);   // patches header sizes, frees

// IMA ADPCM block codec (shared by WAV and AVI/MP4 audio)
int em_adpcm_block_frames(int channels, int block_bytes);
void em_adpcm_decode_block(const uint8_t *blk, int block_bytes, int channels, int16_t *pcm);
int em_adpcm_encode_block(const int16_t *pcm, int frames, int channels, uint8_t *blk);

// MP3: frame scanner always available; decoding needs minimp3.h (EM_WITH_MINIMP3).
typedef struct em_mp3 em_mp3_t;
em_err_t em_mp3_open(em_mp3_t **out, em_io_t *io, em_audio_info_t *info);
int      em_mp3_read(em_mp3_t *m, int16_t *pcm, int max_frames);  // >=1152 frames needed
void     em_mp3_close(em_mp3_t *m);

// ---- Containers --------------------------------------------------------------------
typedef enum { EM_KIND_NONE = 0, EM_KIND_VIDEO = 1, EM_KIND_AUDIO = 2 } em_kind_t;
typedef enum {
    EM_VC_NONE = 0, EM_VC_MJPEG = 1, EM_VC_OTHER = 2,    // OTHER (H.263/H.264/...) is demuxed, not decoded
    EM_AC_NONE = 0, EM_AC_PCM16 = 1, EM_AC_IMA = 2, EM_AC_OTHER = 3   // OTHER (AAC/AMR/MP3) passes through
} em_codec_t;

typedef struct {
    int has_video, has_audio;
    uint16_t width, height;
    uint32_t fps_x1000;
    em_codec_t vcodec;
    uint32_t sample_rate; uint8_t channels; em_codec_t acodec;
    uint32_t duration_ms;
    uint16_t adpcm_block;          // bytes, when acodec == EM_AC_IMA
} em_media_info_t;

typedef struct { em_kind_t kind; uint32_t size; uint32_t pts_ms; } em_packet_t;

typedef struct em_demux em_demux_t;
// Auto-detects AVI, MP4/3GP/MOV. (WAV/MP3/JPEG/PNG go through em_player.)
em_err_t em_demux_open(em_demux_t **out, em_io_t *io, em_media_info_t *info);
em_err_t em_demux_next(em_demux_t *d, em_packet_t *pkt);       // EM_EOF at end
int      em_demux_read(em_demux_t *d, void *dst, uint32_t n);  // bytes read from current payload (0 = end), <0 error
void     em_demux_close(em_demux_t *d);

// Muxer (AVI, MP4, 3GP). Index lives in RAM (12 bytes/packet) up to `max_packets`;
// when full, mux_* returns EM_ERR_FULL: finish() and start a new segment.
typedef enum { EM_FMT_AVI = 1, EM_FMT_MP4 = 2, EM_FMT_3GP = 3 } em_fmt_t;
typedef struct em_mux em_mux_t;
em_err_t em_mux_create(em_mux_t **out, em_io_t *io, em_fmt_t fmt, const em_media_info_t *info,
                       uint32_t max_packets);
em_err_t em_mux_video(em_mux_t *m, const void *jpeg, uint32_t n, uint32_t pts_ms);
em_err_t em_mux_audio(em_mux_t *m, const int16_t *pcm, uint32_t frames);
em_err_t em_mux_finish(em_mux_t *m);

// ---- Player ---------------------------------------------------------------------------
// Sinks are where DMA lives. audio_write() should be a DMA-backed blocking write
// (I2S). video_rows is the panel blit (SPI/QSPI DMA). Both may be NULL.
typedef struct {
    int  (*audio_config)(void *u, uint32_t rate, int channels);
    int  (*audio_write)(void *u, const int16_t *pcm, int frames);   // blocks until queued
    int  (*video_rows)(void *u, int y, int nrows, const uint16_t *rgb565, int w);
    void (*video_begin)(void *u, int w, int h);
    void (*sleep_ms)(void *u, uint32_t ms);
    uint32_t (*now_ms)(void *u);
    void *user;
} em_sink_t;

typedef struct em_player em_player_t;
typedef struct { int is_image; int has_video, has_audio; uint16_t width, height; uint32_t duration_ms; } em_player_info_t;
em_err_t em_player_open(em_player_t **out, em_io_t *io, const em_sink_t *sink, em_player_info_t *info);
em_err_t em_player_step(em_player_t *p);    // one unit of work; EM_EOF when finished
void     em_player_set_scale(em_player_t *p, int scale_shift);   // JPEG/MJPEG 1/1..1/8 (0..3); call before first step
uint32_t em_player_pos_ms(const em_player_t *p);
uint32_t em_player_dropped(const em_player_t *p);
void     em_player_close(em_player_t *p);

#ifdef __cplusplus
}
#endif
#endif
