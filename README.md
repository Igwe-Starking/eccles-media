<div align="center">

# 🎬 ECCLES MEDIA

### Images, audio and video on a microcontroller. Streaming, small, no hidden tasks.

**Plain C99 · no third-party code (MP3 optional) · built for the Eccles Phone**

[![CI](https://github.com/igwe-starking/eccles-media/actions/workflows/ci.yml/badge.svg)](https://github.com/igwe-starking/eccles-media/actions/workflows/ci.yml)
[![Version](https://img.shields.io/badge/version-v1.0.0-blue.svg)](CHANGELOG.md)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C99](https://img.shields.io/badge/C-99-orange.svg)](#)
[![Code size](https://img.shields.io/badge/code-~44%20KB%20(x86--64%20-Os)-brightgreen.svg)](#-memory---the-honest-part)

</div>

```c
#include "em_port_esp.h"                          // ESP-IDF port

em_esp_cfg_t cfg = {.i2s_bclk = 4, .i2s_ws = 5, .i2s_dout = 6,   // YOUR pins
                    .mic_rate = 16000, .fps = 15, .media_core = 1, .task_prio = 5};
em_esp_init(&cfg);
em_esp_play_start("/sdcard/clip.avi", /*scale_shift*/ 1, on_done, NULL);
```

Show a photo, play a clip, record from the camera. Data moves in bands, packets and buffers, so a whole
file is never held in RAM by the codecs.

---

## ✨ Why ECCLES MEDIA

| | |
|---|---|
| 🌊 **Streaming** | Images move in row bands, video one packet at a time, audio one buffer at a time. MP4 sample tables stay in the file. |
| 🧮 **Predictable memory** | Handles allocate up front through `em_alloc()`, so you can route big buffers to PSRAM. |
| 🤝 **Cooperative** | The portable core starts no tasks. `em_player_step()` does one bounded unit of work. |
| 🚚 **DMA friendly** | Decoders hand back double-buffered row bands, so a blit of band N overlaps decoding of band N+1. |
| ⏱ **A/V sync** | Audio is the master clock. Late video frames are dropped, never queued (`EM_LATE_DROP_MS`). |
| 📷 **Free video encoding** | OV2640/OV5640 JPEG frames are already MJPEG: sensor → `em_mux_video()` → file. |
| 🧪 **Cross-checked** | Tested against Pillow (libjpeg, libpng) and ffmpeg. |

---

## 🚀 Quick start

No hardware needed:

```bash
make examples && build/example_host
```
```
eccles_media 1.0.0
encoded 320x240 JPEG, decoded 240 rows
```

The program ([`examples/host/main.c`](examples/host/main.c)) encodes a picture into memory and decodes it back:

```c
static uint8_t file[64 * 1024];
em_membuf_t mb; em_io_t io;

em_io_mem(&io, &mb, file, 0, sizeof file);                       // write to RAM
em_jpeg_encode(&io, 320, 240, 85, 3, gradient, NULL);            // quality 85, 4:2:0

em_io_mem(&io, &mb, file, mb.len, sizeof file);                  // read it back
em_jpeg_t *jpg; em_img_info_t info;
if (em_jpeg_open(&jpg, &io, &info) == EM_OK) {
    em_jpeg_decode(jpg, 0, count_rows, &rows);                   // RGB565, band by band
    em_jpeg_close(jpg);
}
```

### Installing

- **ESP-IDF**: put the folder in `components/eccles_media/` and add `eccles_media` to `REQUIRES`. The port
  needs your project to provide `media_hal.h` (camera, mic) and `panel_hal.h` (display).
- **Plain CMake**: `add_subdirectory(eccles-media)` and link `eccles::media` (portable core only).
- **Anything else**: compile `src/*.c` with `-Iinclude -Isrc`.

**MP3:** put [`minimp3.h`](https://github.com/lieff/minimp3) (CC0) in `src/`; CMake then defines
`EM_WITH_MINIMP3`. Without it, `em_mp3_open()` returns `EM_ERR_UNSUPPORTED`.

---

## 🧭 How it works

```
 file ──► em_io ──► demux / parser ──► decoder ──► sink callbacks ──► I2S DMA · panel DMA
                                       (bands, buffers)

 camera JPEG + mic PCM ──► muxer ──► AVI / MP4 / 3GP file
```

The ESP-IDF port runs the player in a task pinned to a core (default core 1), below WiFi priority, with a
6 × 240 frame I2S DMA ring. `audio_write()` blocks only when the ring is full, which paces playback.

---

## 📚 The API

| | |
|---|---|
| **I/O** | `em_io_file(&io, FILE*)` · `em_io_mem(&io, &mb, buf, len, cap)` · or fill an `em_io_t` yourself |
| **Memory** | `em_set_allocator(alloc, free)` |
| **JPEG** | `em_jpeg_open` · `_decode` · `_reset` · `_close` · `_encode` |
| **PNG** | `em_png_open` · `_decode` · `_close` · `_encode` |
| **WAV / ADPCM** | `em_wav_open` · `_read` · `_seek` · `_close` · `_create` · `_write` · `_finish` · `em_adpcm_*` |
| **MP3** | `em_mp3_open` · `_read` · `_close` |
| **Containers** | `em_demux_open` · `_next` · `_read` · `_close` · `em_mux_create` · `_video` · `_audio` · `_finish` |
| **Player** | `em_player_open` · `_step` · `_set_scale` · `_pos_ms` · `_dropped` · `_close` |
| **ESP-IDF port** | `em_esp_init` · `em_esp_play_start` / `_stop` · `em_esp_capture_jpeg` / `_png` · `em_esp_record_start` / `_stop` · `em_esp_record_audio_start` |

Most calls return `em_err_t`: **`EM_OK`**, **`EM_EOF`**, or a negative `EM_ERR_*` (`IO`, `FORMAT`,
`UNSUPPORTED`, `NOMEM`, `ARG`, `FULL`, `ABORT`). The full documentation is in
[`include/eccles_media.h`](include/eccles_media.h).

### Recording (ESP-IDF)

```c
em_esp_capture_jpeg("/sdcard/img.jpg");                  // camera JPEG straight to file
em_esp_capture_png("/sdcard/img.png", 1);
em_esp_record_start("/sdcard/vid.mp4", /*with_audio*/ 1); // .avi / .mp4 / .3gp
em_esp_record_stop();
em_esp_record_audio_start("/sdcard/rec.wav", /*adpcm*/ 1); // stop with em_esp_record_stop()
```

**Privacy:** the port captures only through `media_hal_camera_capture()` and `media_hal_mic_read()`, so the
camera and mic indicators owned by your HAL are not bypassed. A real HAL should hold an indicator on for
the whole streaming session, not pulse it per call.

---

## 🔌 Formats

| | Decode / play | Encode / record |
|---|---|---|
| **JPEG** | baseline + extended sequential, any 1/2 sampling, restart markers, 1/1 to 1/8 scaling | baseline, 4:2:0 / 4:4:4 / gray, quality 1-100 |
| **PNG** | all colour types, 1-16 bit, palette, tRNS, alpha composited on a background colour | 8-bit RGB, adaptive filter + LZ77 + fixed Huffman |
| **WAV** | PCM8 / PCM16 / IMA-ADPCM | PCM16 / IMA-ADPCM (4:1) |
| **MP3** | via minimp3 | – |
| **AVI** | MJPEG + PCM16 / IMA-ADPCM | MJPEG + PCM16 |
| **MP4 / 3GP / MOV** | MJPEG + PCM16 (`sowt`); other codecs are demuxed and flagged `EM_VC_OTHER` / `EM_AC_OTHER` | MJPEG + PCM16 |

---

## 🧠 Memory - the honest part

Peak heap measured with a counting allocator on a host build (your own file and frame buffers excluded):

| Operation | Peak heap |
|---|---|
| JPEG decode 320×240 4:2:0 at 1/1, 1/2, 1/4, 1/8 | 35.8, 15.2, 10.0, 8.7 KB |
| JPEG encode 320×240 4:2:0 | 21.0 KB |
| PNG decode 250×100 (incl. 32 KB inflate window) | 43.9 KB |
| PNG encode 320×240 | 23.9 KB |
| WAV PCM / IMA read handle | 0.1 / 2.6 KB |
| AVI / MP4 demux handle | 0.1 / 0.3 KB |
| Player, 160×120 MJPEG AVI, video only | 32.4 KB |
| MP4 mux index, 36,000 packets (about 10 min) | 422 KB (PSRAM) |

- Decoders and the player may allocate more than once: the player creates its JPEG decoder on the first
  video frame, then reuses it with `em_jpeg_reset()`.
- The ESP-IDF port uses extra buffers: 128 KB for a captured camera JPEG, and `em_esp_capture_png()`
  holds one decoded frame.
- The only global in the portable core's allocation path is the allocator hook. The port keeps its own
  task state.
- Largest single stack frame is about 1.3 KB; 6 KB task stacks are what the port uses for playback and video.
- Code at `-Os` on x86-64: about 44 KB for `src/`.

## ✅ Limits

- **No H.264, H.263, AAC or AMR decoding.** Real phone-camera `.mp4` / `.3gp` files use those codecs: they
  are parsed and reported but not played. Files recorded by this library use MJPEG + PCM (ffmpeg and VLC
  play them; a stock phone gallery may not). 3GP here is the container only.
- **No MP3 encoder.** Record to WAV (IMA-ADPCM gives 4:1).
- **No progressive JPEG, interlaced PNG or arithmetic coding** (rejected with `EM_ERR_UNSUPPORTED`).
- Chroma upsampling is nearest-neighbour.
- No AVI/MP4 seeking. The mux index is RAM limited: at `EM_ERR_FULL` finish the file and start another
  (the ESP-IDF port rolls to a numbered segment automatically).

## 🔬 How it was tested

- `make test` runs 32 checks (needs gcc, Python 3 with NumPy and Pillow, ffmpeg): Pillow and ffmpeg decode
  our output and we decode theirs.
- 304 corrupted or truncated JPEGs decode without a crash.
- The MP3 adapter is tested against a **mock** decoder, not the real minimp3.

> **Not done (be aware):** the ESP-IDF port has only been syntax-checked against API-shaped stubs, never
> built with the IDF toolchain or run on hardware. PNG, WAV and container parsers have no corrupted-input
> fuzzing. H.263 / AMR files are not covered.

---

## 🗂 Repository layout

```
include/eccles_media.h   portable API + documentation
src/                     codecs, containers, player (plain C99)
port/                    ESP-IDF glue: I2S, panel, camera, mic
examples/host/           runs on your PC
test/                    host regression suite
```

## 🛠 Development

```bash
make examples     # build the host example
make test         # full regression suite
make size         # code size of the portable core
```

Contributions are welcome: see [CONTRIBUTING.md](CONTRIBUTING.md). Found a security issue? See
[SECURITY.md](SECURITY.md).

## 🧩 The EcclesRTLib family

| Library | What it gives your firmware |
|---|---|
| [`eccles-wasm`](https://github.com/igwe-starking/eccles-wasm) | Sandboxed WebAssembly |
| [`eccles-web`](https://github.com/igwe-starking/eccles-web) | HTML, CSS and JavaScript pages |
| [`eccles-gui`](https://github.com/igwe-starking/eccles-gui) | 2D widgets and 3D without a framebuffer |
| **`eccles-media`** (this one) | Images, audio and video |
| [`eccles-rtmem`](https://github.com/igwe-starking/eccles-rtmem) | The rest of your firmware's allocations |

## 📄 License

[MIT](LICENSE) © Nwobodo Ecclesiastes
