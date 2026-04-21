
```编译运行指令:

gcc srt_cam_push.c -o srt_cam_push \
  $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-video-1.0 glib-2.0) \
  -lasound -lpthread

./srt_cam_push 


```

# Pusher README

## 1. 组件职责

Pusher 负责：

- 采集摄像头视频与 ALSA 音频
- 编码视频（x264）与音频（AAC/Opus）
- 封装为 MPEG-TS
- 通过 SRT 推送到 relay
- 向 TS 中注入 telemetry private packet
- 提供 TCP 控制端口，响应 relay / player 的控制命令

这是整个系统的**唯一源头**，因此任何码率、帧率、分辨率、黑屏/静音切换，最终都需要在这里落实。

---

## 2. 默认配置

当前默认配置包括：

- 分辨率：`1280x720`
- 帧率：`25 fps`
- 视频码率：`2000 kbps`
- GOP：`50`
- profile：`baseline`
- SRT latency：`20 ms`
- 控制端口：`10090`

音频默认启用，优先从 ALSA 枚举中选择合适设备，找不到时回退到 `default`。

---

## 3. 管线说明

推流端核心管线由以下部分构成：

### 视频主链

- `v4l2src`
- `jpegdec`
- `videoconvert`
- `capsfilter (vcaps)`
- `input-selector (vsel)`
- `x264enc (venc)`
- `h264parse (vparse)`
- `mpegtsmux`
- `identity (tlmyinject)`
- `srtclientsink`

### 视频占位链

- `videotestsrc pattern=black`
- `capsfilter (blackcaps)`
- `vsel.sink_1`

### 音频主链

- `alsasrc`
- `audioresample`
- `audioconvert`
- `input-selector (asel)`
- 音频编码器
- `mpegtsmux`

### 音频占位链

- `audiotestsrc wave=silence`
- `asel.sink_1`

---

## 4. Telemetry 注入

Pusher 不仅推真实媒体，还会在 TS 中定期插入 telemetry private packet。

作用：

- 为 relay 提供可追踪的 `seq + t_push_us`
- 为 observer 解析与 ACK 回传提供基础
- 支持 relay 计算 Push→Relay / Push→ACK 等时延

如果没有这一层，relay 无法获得真正的端到端时延观测。

---

## 5. 控制面

Pusher 内部运行一个 TCP 控制服务，默认监听 `10090`。

### 支持的命令

#### 5.1 暂停 / 恢复

```text
PAUSE
RESUME
```

- `PAUSE`：切视频到黑屏，占音频到静音
- `RESUME`：恢复正常音视频，并请求一次关键帧

#### 5.2 请求关键帧

```text
IDR
```

作用：

- 切换档位后尽快重新同步
- 恢复播放时缩短等待关键帧时间

#### 5.3 调整码率

```text
SET_BITRATE 1200
```

作用于 `x264enc bitrate`。

#### 5.4 调整帧率

```text
SET_FPS 15
```

通过更新 `vcaps / blackcaps` 的 raw video caps 生效。

#### 5.5 调整分辨率

```text
SET_RESOLUTION 960x540
```

也支持：

```text
SET_RESOLUTION 960 540
```

#### 5.6 视频模式切换

```text
VIDEO_MODE BLACK
VIDEO_MODE NORMAL
```

#### 5.7 音频模式切换

```text
AUDIO_MODE SILENT
AUDIO_MODE NORMAL
```

---

## 6. 控制设计原则

### 先便宜，后激进

推荐优先级：

1. `IDR`
2. `SET_BITRATE`
3. `SET_FPS`
4. `SET_RESOLUTION`
5. `VIDEO_MODE BLACK / AUDIO_MODE SILENT`

### 恢复时比降档更慢

- 降档要快
- 升档要慢
- 每次恢复后最好补一次 `IDR`

---




