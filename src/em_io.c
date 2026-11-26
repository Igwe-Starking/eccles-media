#include "eccles_media.h"
#include <stdlib.h>
#include <string.h>

static void *(*g_alloc)(size_t, int) = NULL;
static void (*g_free)(void *) = NULL;

void em_set_allocator(void *(*a)(size_t, int), void (*f)(void *)) { g_alloc = a; g_free = f; }
void *em_alloc(size_t n, int flags) { return g_alloc ? g_alloc(n, flags) : malloc(n); }
void em_free(void *p) { if (!p) return; if (g_free) g_free(p); else free(p); }

static int mem_read(void *c, void *dst, uint32_t n) {
    em_membuf_t *m = c;
    uint32_t left = m->len > m->pos ? m->len - m->pos : 0;
    if (n > left) n = left;
    memcpy(dst, m->buf + m->pos, n); m->pos += n; return (int)n;
}
static int mem_write(void *c, const void *src, uint32_t n) {
    em_membuf_t *m = c;
    if (m->pos + n > m->cap) return -1;
    memcpy(m->buf + m->pos, src, n); m->pos += n;
    if (m->pos > m->len) m->len = m->pos;
    return (int)n;
}
static int mem_seek(void *c, uint32_t o) { em_membuf_t *m = c; if (o > m->cap) return -1; m->pos = o; return 0; }
static uint32_t mem_tell(void *c) { return ((em_membuf_t *)c)->pos; }

void em_io_mem(em_io_t *io, em_membuf_t *m, void *buf, uint32_t len, uint32_t cap) {
    m->buf = buf; m->len = len; m->cap = cap; m->pos = 0;
    io->read = mem_read; io->write = mem_write; io->seek = mem_seek; io->tell = mem_tell; io->ctx = m;
}

#ifndef EM_NO_STDIO
static int f_read(void *c, void *d, uint32_t n) { return (int)fread(d, 1, n, (FILE *)c); }
static int f_write(void *c, const void *s, uint32_t n) { return (int)fwrite(s, 1, n, (FILE *)c); }
static int f_seek(void *c, uint32_t o) { return fseek((FILE *)c, (long)o, SEEK_SET); }
static uint32_t f_tell(void *c) { return (uint32_t)ftell((FILE *)c); }
void em_io_file(em_io_t *io, FILE *f) {
    io->read = f_read; io->write = f_write; io->seek = f_seek; io->tell = f_tell; io->ctx = f;
}
#endif
