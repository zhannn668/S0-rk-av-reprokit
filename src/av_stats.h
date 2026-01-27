// av_stats.h
#pragma once

#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    atomic_uint_fast64_t video_frames;   // per 1s
    atomic_uint_fast64_t enc_bytes;      // per 1s
    atomic_uint_fast64_t audio_chunks;   // per 1s
    atomic_uint_fast64_t drop_count;     // per 1s
} AvStats;

void av_stats_init(AvStats *s);
void av_stats_tick_print(AvStats *s);

static inline void av_stats_inc_video_frame(AvStats *s) {
    atomic_fetch_add_explicit(&s->video_frames, 1, memory_order_relaxed);
}
static inline void av_stats_add_enc_bytes(AvStats *s, uint64_t bytes) {
    atomic_fetch_add_explicit(&s->enc_bytes, bytes, memory_order_relaxed);
}
static inline void av_stats_inc_audio_chunk(AvStats *s) {
    atomic_fetch_add_explicit(&s->audio_chunks, 1, memory_order_relaxed);
}
static inline void av_stats_add_drop(AvStats *s, uint64_t n) {
    atomic_fetch_add_explicit(&s->drop_count, n, memory_order_relaxed);
}

#ifdef __cplusplus
}
#endif
