// av_stats.c
#include "av_stats.h"
#include "log.h"

/*
 * 初始化统计结构体：将各计数器清零。
 *
 * 这里使用 C11 原子类型，保证多线程下更新/读取计数不会产生数据竞争。
 *
 * @param s  统计对象指针
 */
void av_stats_init(AvStats *s)
{
    if (!s) return;
    atomic_store(&s->video_frames, 0);
    atomic_store(&s->enc_bytes, 0);
    atomic_store(&s->audio_chunks, 0);
    atomic_store(&s->drop_count, 0);
}

/*
 * 每秒打印一次统计信息。
 *
 * 设计：
 * - 使用 atomic_exchange 将计数器“读取并清零”，把累计值按 1 秒窗口输出。
 * - 这样多线程只负责累加，打印线程负责按周期归零，得到近似实时速率。
 *
 * 指标解释：
 * - video_fps：过去 1 秒编码成功的视频帧数
 * - enc_bitrate：过去 1 秒编码输出字节数换算的 kbps（按 1000 进位）
 * - audio_chunks_per_sec：过去 1 秒写入的音频 chunk 数
 * - drop_count：过去 1 秒检测到的丢帧/异常次数
 *
 * @param s  统计对象指针
 */
void av_stats_tick_print(AvStats *s)
{
    if (!s) return;
    uint64_t frames = atomic_exchange(&s->video_frames, 0);
    uint64_t bytes  = atomic_exchange(&s->enc_bytes, 0);
    uint64_t achk   = atomic_exchange(&s->audio_chunks, 0);
    uint64_t drops  = atomic_exchange(&s->drop_count, 0);

    /*
     * 假设 tick 周期为 1 秒：
     * kbps = (bytes * 8) / 1000
     */
    uint64_t kbps = (bytes * 8) / 1000; // assume 1s

    LOGI("[STAT] video_fps=%llu enc_bitrate=%llukbps audio_chunks_per_sec=%llu drop_count=%llu",
         (unsigned long long)frames,
         (unsigned long long)kbps,
         (unsigned long long)achk,
         (unsigned long long)drops);
}
