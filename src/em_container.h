// em_container.h - private: demux/mux object layouts shared by AVI and MP4.
#ifndef EM_CONTAINER_H
#define EM_CONTAINER_H
#include "em_priv.h"

struct em_demux {
    em_io_t *io;
    em_err_t (*next)(struct em_demux *d, em_packet_t *pkt);
    void (*destroy)(struct em_demux *d);
    uint32_t pay_off, pay_left;     // current packet payload: absolute offset + bytes still unread
    int pay_seeked;
};

typedef struct { uint32_t off, size, pts; } em_idx_t;   // size bit31 = audio; pts = ms (video) or frames (audio)

struct em_mux {
    em_io_t *io;
    em_fmt_t fmt;
    em_media_info_t info;
    em_idx_t *idx; uint32_t n, cap;
    uint32_t data_start;            // file offset of first payload byte region
    uint32_t pos;                   // current write position
    uint32_t vframes, aframes, last_pts, first_pts; int have_first;
    int broken;
};

em_err_t em_avi_open(struct em_demux **out, em_io_t *io, em_media_info_t *info);
em_err_t em_mp4_open(struct em_demux **out, em_io_t *io, em_media_info_t *info);
em_err_t em_avi_mux_begin(struct em_mux *m);
em_err_t em_avi_mux_end(struct em_mux *m);
em_err_t em_mp4_mux_begin(struct em_mux *m);
em_err_t em_mp4_mux_end(struct em_mux *m);
em_err_t em_avi_mux_chunk(struct em_mux *m, int audio, const void *data, uint32_t n, uint32_t pts_or_frames);
em_err_t em_mp4_mux_sample(struct em_mux *m, int audio, const void *data, uint32_t n, uint32_t pts_or_frames);
#endif
