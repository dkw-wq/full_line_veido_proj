# full_line_veido_proj

简述:

pusher 和 srt_server 运行在树莓派上，树莓派需要接一个USB摄像头采集视频数据。
srt_server 是一个srt 服务器用于用于中转数据(接收推流端数据后推送给播放端，播放端可有多个)。

player端运行在Windows， 编译运行用的是vcpkg,ffmpeg和Qt
player下的build不用看，是系统生成的。

observer用于不准确地计算延迟

dashboard.html可直接连接用于观察中继上传的各种数据。

链路的各个节点徐注意更改为自己树莓派或其他设备的IP

详细运行指令可参考各文件夹下的ReadMe.md


# SRT 低延迟一对多链路系统总览

## 1. 项目概述

本项目是一个围绕 **SRT 低延迟一对多分发** 构建的端到端系统，包含以下核心组成：

- **推流端（pusher）**：采集摄像头/麦克风，编码为 H.264 + AAC/Opus，封装为 MPEG-TS，经 SRT 推送到中继。
- **中继端（relay）**：接收一路推流，分发到多个拉流端；同时观测 telemetry、统计链路指标，并执行 subscriber 级与 global 级控制策略。
- **播放端（player）**：基于 FFmpeg + D3D11 硬解与渲染，负责拉流播放、重连、关键帧等待、录制、暂停推流控制。
- **observer（旁路观测端）**：作为额外 subscriber 接入 relay，解析 telemetry 并通过 UDP ACK 回送给 relay，用于计算 Push→Relay / Relay→ACK / Push→ACK 延迟。
- **Dashboard**：通过 WebSocket 连接 relay，将全局状态、带宽、时延、坏端比例、当前档位、各 subscriber 控制状态可视化。

系统目标不是单纯“能推能播”，而是：

1. 可观测：看见各链路的关键时延和异常。
2. 可控制：支持按 subscriber 与按全局的自适应动作。
3. 可恢复：当网络劣化后能自动降档、占位保活，并在恢复后逐步升档。

---

## 2. 总体链路

```text
Camera/Mic
   │
   ▼
Pusher (GStreamer)
   │  SRT/TS + telemetry private TS packet
   ▼
Relay
   ├── Subscriber #1 (Player)
   ├── Subscriber #2 (Player)
   ├── Subscriber #3 (Observer)
   └── ...

Observer ──UDP ACK──► Relay
Relay ──WebSocket JSON──► Dashboard
Relay ──TCP control──► Pusher
Player ──TCP control──► Pusher
```

### 数据平面

- 推流端输出 MPEG-TS over SRT。
- relay 接收后对多个 subscriber 广播。
- player 侧拉取 SRT 流并播放。

### 观测平面

- 推流端定期向 TS 中注入 telemetry private section。
- relay 观察原始 TS 包，记录 `seq -> t_push_us`。
- observer 读取同一份流，提取 telemetry 后发送 ACK。
- relay 收到 ACK 后计算链路时延。

### 控制平面

- relay 根据 subscriber 与 global 指标做控制决策。
- relay 通过 TCP 控制口向 pusher 下发 `IDR / SET_BITRATE / SET_FPS / SET_RESOLUTION / VIDEO_MODE / AUDIO_MODE`。
- player 也可通过控制口手动执行暂停/恢复推流。

---

## 3. 组件目录

```text
README.md                # 本总览文档
pusher/README.md         # 推流端说明
relay/README.md          # 中继端说明
player/README.md         # 播放端说明
```

---

## 4. 快速启动

### 4.1 启动 relay

示例：

```bash
./build/srt_relay_server \
  --bind 0.0.0.0 \
  --pub-port 9000 \
  --sub-port 9001 \
  --ws-port 8765 \
  --pusher-ctrl-port 10090

or:
./build/srt_relay_server --bind 0.0.0.0 --pub-port 9000 --sub-port 9001
```

常用参数：

- `--pub-port`：推流端接入端口，默认 `9000`
- `--sub-port`：播放端 / observer 接入端口，默认 `9001`
- `--ws-port`：dashboard WebSocket 端口，默认 `8765`
- `--bind`：绑定地址，默认 `0.0.0.0`
- `--pusher-ctrl-host`：显式指定 pusher 控制地址；不指定时默认使用 publisher peer IP
- `--pusher-ctrl-port`：pusher 控制端口，默认 `10090`
- `--no-auto-control`：关闭 relay 对 pusher 的自动控制

### 4.2 启动 pusher

推流端默认向 relay 的 `9000` 端口推流，控制端口默认 `10090`。

启动前需要确认：

- 摄像头设备路径存在
- 音频设备可被 ALSA 识别
- relay 地址可达

编译:
gcc srt_cam_push.c -o srt_cam_push \
  $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-video-1.0 glib-2.0) \
  -lasound -lpthread

运行:
./srt_cam_push 

### 4.3 启动 player

```bash

.\player\build\Release\srt_player.exe "srt://10.160.196.17:9001?mode=caller&latency=20&streamid=cam1" 

编译:
cmake -S player -B player/build `
   -G "Visual Studio 17 2022" -A x64 `
   -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
   -DVCPKG_TARGET_TRIPLET=x64-windows

```

### 4.4 打开 Dashboard

直接用浏览器打开 HTML 文件，输入：

```text
ws://<relay_ip>:8765
```

点击“连接”即可。

---

## 5. Dashboard 说明

Dashboard 是整个系统的统一观测入口，用于确认“当前是否正常”“哪里出问题”“控制器正在做什么”。

### 5.1 Dashboard 展示内容

#### 连接区

- WebSocket 地址输入框
- 连接 / 断开按钮
- 当前连接状态与最近更新时间

#### 全局指标区

- 推流端在线状态
- 拉流端数
- 入流带宽 / 出流带宽
- 总接收 / 总发送
- TS 不连续
- 队列丢包
- Push→Relay
- Relay→ACK
- Push→ACK
- ACK 新鲜度
- 全局状态（如 `Normal` / `GlobalWarning` / `GlobalDegraded` / `GlobalPlaceholder`）
- 全局动作（如 `NOOP` / `IDR` / `DEGRADE_BITRATE` / `RECOVER_STEP`）
- 坏端比例
- 当前档位（bitrate / fps / resolution）

#### 拉流端列表

每个 subscriber 会展示：

- ID
- peer 地址
- 已发送数据量
- SRT latency
- Push→Relay / Relay→ACK / Push→ACK
- 队列深度
- 控制状态（`NORMAL / WARN / DROP / PLACEHOLDER`）
- ACK 新鲜度
- 空闲时间

#### 事件日志

本地前端连接日志，例如 WebSocket 是否已连接、是否断开等。

### 5.2 Dashboard 适合看什么

#### 场景 1：确认系统是否正常

重点看：

- `Push→ACK`
- `ACK 新鲜度`
- `队列丢包`
- `坏端比例`
- `全局动作`

如果：

- `Push→ACK` 稳定
- `ACK 新鲜度` 较小
- `队列丢包` 不增长
- `全局动作` 长期为 `NOOP`

说明链路基本正常。

#### 场景 2：判断是单个用户问题还是全局问题

- 某个 subscriber 的 `ctrl` 进入 `WARN / DROP / PLACEHOLDER`，但坏端比例很低：通常是局部问题。
- 坏端比例持续升高，且 `global_action` 触发降档：通常是全局链路恶化。

#### 场景 3：观察自动控制器是否生效

重点看：

- `global_action`
- `global_bitrate_kbps`
- `global_fps`
- `global_width / global_height`
- subscriber 的 `ctrl`

这几项一起看，能知道 relay 是先做 per-subscriber 隔离，还是升级成 pusher 全局控制。

---

## 6. 控制器设计概览

### 6.1 Subscriber 级控制

subscriber 控制器典型动作：

- `NORMAL`：正常发流
- `WARN`：轻度告警
- `DROP`：清空积压队列，丢旧保新
- `PLACEHOLDER`：不再发真实内容，改发占位 TS，超时后断开

适合处理：

- 某个拉流端链路过差
- 某个拉流端处理速度过慢
- 某个拉流端 ACK 过旧

### 6.2 Global 级控制

global 控制器典型动作：

- `IDR`
- `DEGRADE_BITRATE`
- `DEGRADE_FPS`
- `DEGRADE_RESOLUTION`
- `ENTER_PLACEHOLDER`
- `RECOVER_STEP`

适合处理：

- 多数 subscriber 同时变差
- Push→Relay 链路恶化
- publisher stall
- 恢复后逐级升档

### 6.3 动作优先级建议

推荐采用：

1. `IDR`
2. `SET_BITRATE`
3. `SET_FPS`
4. `SET_RESOLUTION`
5. `VIDEO_MODE BLACK / AUDIO_MODE SILENT`

恢复时顺序反过来，但升档应更慢。

---

## 7. 典型问题定位

### 7.1 Push→Relay 高

说明问题更靠近：

- 推流端上行
- relay 前网络
- pusher 本身编码/发送阻塞

### 7.2 Relay→ACK 高

说明问题更靠近：

- relay → subscriber 路径
- observer 接收与 ACK 回传
- 下游网络

### 7.3 Push→ACK 高但 Push→Relay 正常

说明大概率是 relay 之后的问题，而不是源头问题。

### 7.4 某个 subscriber 的 ctrl 经常进入 DROP

说明该 subscriber 长期跟不上：

- 下游网差
- 设备性能不足
- 该端无法持续消费最新数据

---

## 8. 下一步建议

当前项目已具备：

- telemetry 注入
- relay 指标统计
- subscriber 控制
- global 自适应决策
- pusher 参数调整
- dashboard 可视化

---

## 9. 相关文档

- `pusher/README.md`
- `relay/README.md`
- `player/README.md`

