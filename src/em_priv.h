// em_priv.h - tiny helpers shared by the codecs. Not part of the public API.
#ifndef EM_PRIV_H
#define EM_PRIV_H
#include "eccles_media.h"
#include <string.h>

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint16_t rd16be(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t rd32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline void wr16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static inline void wr16be(uint8_t *p, uint32_t v) { p[0] = v >> 8; p[1] = v; }
static inline void wr32be(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FOURCC_BE(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static inline uint16_t pack565(int r, int g, int b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }
static inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static inline int io_read_exact(em_io_t *io, void *dst, uint32_t n) {
    return io->read(io->ctx, dst, n) == (int)n ? 0 : -1;
}
static inline int io_write_all(em_io_t *io, const void *src, uint32_t n) {
    return io->write(io->ctx, src, n) == (int)n ? 0 : -1;
}
#endif
