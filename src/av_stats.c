// av_stats.c
#include "av_stats.h"
#include "log.h"

void av_stats_init(AvStats *s)
{
    if (!s) return;
    atomic_store(&s->video_frames, 0);
    atomic_store(&s->enc_bytes, 0);
    atomic_store(&s->audio_chunks, 0);
    atomic_store(&s->drop_count, 0);
}

void av_stats_tick_print(AvStats *s)
{
    if (!s) return;
    uint64_t frames = atomic_exchange(&s->video_frames, 0);
    uint64_t bytes  = atomic_exchange(&s->enc_bytes, 0);
    uint64_t achk   = atomic_exchange(&s->audio_chunks, 0);
    uint64_t drops  = atomic_exchange(&s->drop_count, 0);

    uint64_t kbps = (bytes * 8) / 1000; // assume 1s

    LOGI("[STAT] video_fps=%llu enc_bitrate=%llukbps audio_chunks_per_sec=%llu drop_count=%llu",
         (unsigned long long)frames,
         (unsigned long long)kbps,
         (unsigned long long)achk,
         (unsigned long long)drops);
}
