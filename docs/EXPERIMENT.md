# 可复现实验：10 秒录制 (out.h264 + out.pcm)

## 1) 编译

### 用 Makefile
```bash
make -j
```

### 用 CMake
```bash
mkdir -p build
cd build
cmake ..
cmake --build . -j
```

## 2) 录制命令

默认录制 **10 秒** 输出：

- `out.h264` (H.264 Annex-B)
- `out.pcm`  (s16le raw pcm)

```bash
./rkav_repro \
  --video-dev /dev/video0 \
  --size 1280x720 \
  --fps 30 \
  --bitrate 2000000 \
  --audio-dev hw:0,0 \
  --sr 48000 \
  --ch 2 \
  --sec 10 \
  --out-h264 out.h264 \
  --out-pcm out.pcm
```

运行时会打印：
- `[CFG] ...` 最终配置摘要（只一行）
- `[STAT] ...` 每秒统计
- `[v4l2] device fmt ...` fourcc/stride/plane sizeimage

## 3) ffprobe 检查（视频）

```bash
ffprobe -hide_banner -f h264 -show_streams -show_format out.h264
```

## 4) ffprobe 检查（音频 raw pcm）

raw pcm 需要明确参数：48000Hz / 2ch / s16le。

```bash
ffprobe -hide_banner \
  -f s16le -ar 48000 -ac 2 \
  -show_streams -show_format out.pcm
```

## 5) 转 WAV（可选）

```bash
ffmpeg -y -f s16le -ar 48000 -ac 2 -i out.pcm out.wav
ffprobe -hide_banner -show_streams -show_format out.wav
```

## 6) mediainfo（可选）

```bash
mediainfo out.h264
mediainfo out.wav
```
