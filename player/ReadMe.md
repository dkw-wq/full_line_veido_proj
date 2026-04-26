
```
Windows上编译运行顺序:

1.
(1)
PS C:\Users\dkw\.a_dkwrtc\full_line_veido_proj> cmake -S player -B player/build `
   -G "Visual Studio 17 2022" -A x64 `
   -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
   -DVCPKG_TARGET_TRIPLET=x64-windows

(2)
& "D:vcpkg\installed\x64-windows\tools\Qt6\bin\windeployqt.exe" --release player\build\Release\srt_player.exe

//"(2)"运行一次即可，注意编译时把vcpkg等路径换成自己电脑里的文件路径

2.
cmake --build player/build --config Release

3.
.\player\build\Release\srt_player.exe "srt://10.158.134.17:9001?mode=caller&latency=20&streamid=cam1" 

```

# Player README

## 1. 组件职责

Player 负责：

- 拉取 relay 输出的 SRT 流
- 使用 FFmpeg 解复用与解码
- 使用 D3D11 硬件视频解码与渲染
- 播放音频
- 在 UI 中展示状态机
- 提供“暂停推流 / 恢复推流”“开始录制 / 停止录制”等交互

它不是控制器主脑，但它是**最终体验侧**，因此它的状态对调参与验证非常关键。

---

## 2. 启动方式

```bash
player.exe <srt_url>
```

示例：

```bash
player.exe srt://10.160.196.17:9001
```

Player 会从 URL 中提取 host，用于连接 pusher 控制口。

---

## 3. 核心状态机

播放器内部定义了以下主要状态：

- `Idle`
- `Starting`
- `OpeningInput`
- `ProbingStreams`
- `Initializing`
- `WaitingKeyframe`
- `Playing`
- `Reconnecting`
- `Stopping`
- `Stopped`
- `Error`

### 状态设计要点

- `state` 由播放线程驱动
- `remote_paused` 只是显示覆盖层，不直接篡改 `state`
- UI 统一从 `PlayerStatus` 快照渲染

这套设计有利于避免状态散落与 UI 闪烁。

---

## 4. 视频链路

视频主要流程：

- `avformat_open_input`
- `avformat_find_stream_info`
- 找到最佳视频流
- 初始化 FFmpeg 解码器
- 创建 D3D11 硬解设备
- 首个关键帧后开始正常渲染

渲染层采用 D3D11，适合在 Windows 端做低延迟播放与硬件加速。

---

## 5. 音频链路

音频主要流程：

- 初始化 FFmpeg 音频解码器
- 通过 `QAudioSink` 输出
- 维护 `audio_clock`
- 在视频侧根据音频时钟做基本同步

播放器在 UI 上会展示当前是否存在音频流。

---

## 6. 等待关键帧与恢复

Player 很重要的一个状态是 `WaitingKeyframe`。

含义：

- 虽然链路接上了，但还没有拿到可安全解码/显示的关键帧
- 恢复推流、切换档位、重连后，通常都要再等一次关键帧

这也是为什么 relay / pusher 侧经常需要补发 `IDR`。

---

## 7. 重连策略

当出现以下情况时，player 会进入 `Reconnecting`：

- 打开输入失败
- 探测流失败
- 读帧失败
- 长时间无数据
- 视频解码器或设备初始化失败后重试

UI 中会展示：

- 当前消息
- 重连次数
- 状态颜色

---

## 8. 远端控制按钮

播放器 UI 有两类控制按钮：

### 8.1 暂停推流 / 恢复推流

这是发给 pusher 的远端控制，而不是本地 pause 播放。

表现为：

- `remote_paused = true` 时，UI 文案显示“远端已暂停”
- 按钮切成“恢复推流”

### 8.2 开始录制 / 停止录制

录制状态不是直接读底层 recorder，而是使用：

- `Idle`
- `Requested`
- `Active`
- `StopRequested`

这样可避免按钮闪烁与状态竞争。

---

## 9. 典型观测字段

Player 端很适合关注：

- `state`
- `message`
- `reconnect_count`
- `last_packet_ms_ago`
- `audio_clock`
- `remote_paused`
- `record_phase`

这些字段能帮助你判断：

- 是不是在等关键帧
- 是不是在重连
- 是否真的有音频
- 当前录制是否进入底层实况

---
