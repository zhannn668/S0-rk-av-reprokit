# rk-av-reprokit

一个面向 **Rockchip RK35xx（RK3568/RK3588）** 的最小可复现实验工程：  
**V4L2 采集 → MPP H.264 编码 → 文件输出**，同时采集 **ALSA PCM 音频**，并提供统一配置与每秒统计输出，方便你做“花屏/丢帧/码率/音频卡顿”定位。

> 默认录制 10 秒，输出 `out.h264` + `out.pcm`，并在运行时打印设备格式（fourcc/stride/plane）以及每秒统计。

---

## 功能特性

### 1) 统一配置入口
- 所有关键参数集中在 `AppConfig`：
  - 视频：分辨率 / 帧率 / 码率 / 设备节点
  - 音频：采样率 / 通道数 / 设备节点
  - 输出：`out.h264` / `out.pcm` / 录制时长
- 启动时打印一行最终配置摘要：`[CFG] ...`

### 2) 统一日志与统计（每秒一行）
每秒打印：
- `video_fps`：编码输出帧数
- `enc_bitrate`：编码输出码率（kbps）
- `audio_chunks_per_sec`：音频写入块数
- `drop_count`：丢帧计数（基于 V4L2 `sequence` gap + 编码/写入失败）

同时在打开相机后打印设备格式（排查花屏关键）：
- `fourcc`
- `bytesperline (stride)`
- `plane sizeimage`

### 3) 可复现实验产物
- 录制 10 秒输出：
  - `out.h264`（Annex-B H.264 bitstream）
  - `out.pcm`（s16le raw PCM）
- 提供 `docs/EXPERIMENT.md`：包含 `ffprobe`/`ffmpeg`/`mediainfo` 校验命令

---

## 目录结构

```text
rk-av-reprokit/
├─ src/
│  ├─ main.c
│  ├─ app_config.c/.h
│  ├─ av_stats.c/.h
│  ├─ v4l2_capture.c/.h
│  ├─ encoder_mpp.c/.h
│  ├─ audio_capture.c/.h
│  ├─ sink.c/.h
│  └─ log.c/.h
├─ docs/
│  └─ EXPERIMENT.md
├─ Makefile
├─ CMakeLists.txt
└─ README.md
```

---

## 编译

### 方式 A：Makefile（交叉编译/Buildroot 常用）

```bash
make -j
```

> 如果你的 MPP 库名是 `-lmpp` 而不是 `-lrockchip_mpp`，用：
```bash
make -j MPP_LIB=-lmpp
```

### 方式 B：CMake（主机编译/调试也方便）

```bash
mkdir -p build
cd build
cmake ..
cmake --build . -j
```

---

## 运行

直接运行默认参数（10 秒）：

```bash
./bin/rkav_repro
```

自定义参数示例：

```bash
./bin/rkav_repro \
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

---

## 输出说明

运行时你会看到三类关键日志：

1) 配置摘要（只一行）
```text
[CFG] video=/dev/video0 1280x720@30 bitrate=2000000 | audio=hw:0,0 48000Hz ch=2 | out=out.h264,out.pcm | sec=10
```

2) 设备格式（排查花屏非常关键）
```text
[v4l2] device fmt: fourcc=NV12 w=1280 h=720 num_planes=2
[v4l2] plane[0]: bytesperline(stride)=... sizeimage=...
[v4l2] plane[1]: bytesperline(stride)=... sizeimage=...
```

3) 每秒统计
```text
[STAT] video_fps=30 enc_bitrate=1950kbps audio_chunks_per_sec=50 drop_count=0
```

---

## 可复现实验校验

详细命令见：`docs/EXPERIMENT.md`

常用校验：

```bash
ffprobe -hide_banner -f h264 -show_streams -show_format out.h264
ffprobe -hide_banner -f s16le -ar 48000 -ac 2 -show_streams -show_format out.pcm
ffmpeg -y -f s16le -ar 48000 -ac 2 -i out.pcm out.wav
mediainfo out.h264
mediainfo out.wav
```

---

## 常见问题

### 1) 花屏/颜色异常
优先看启动时打印的：
- fourcc 是否符合预期（NV12/NV12M/YUYV）
- stride(bytesperline) 是否比 width 大（很多设备会对齐到 16/32）
- plane sizeimage 是否合理

### 2) drop_count 上升
- V4L2 `sequence` gap：说明采集端已经丢帧（CPU/IO/带宽不够或驱动队列问题）
- 编码或写文件失败：检查存储写入速度、权限、磁盘满

### 3) 音频设备打不开
- 用 `arecord -l` 查看设备
- 修改 `--audio-dev` 为实际 ALSA 设备，例如 `hw:1,0`

---

## License
按你的仓库需要自行补充（MIT / Apache-2.0 等）。
