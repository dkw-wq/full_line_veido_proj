
```
编译运行指令：

cd ~/dir1/vedio_proj/srt
cmake -S . -B build
cmake --build build -j4
./build/srt_relay_server --bind 0.0.0.0 --pub-port 9000 --sub-port 9001


srt代码运行检查实验指令:
推流/拉流命令继续用这组：

# 推流到 9000
ffmpeg -re -stream_loop -1 -i input.mp4 \
  -c:v libx264 -preset veryfast -tune zerolatency \
  -c:a aac -f mpegts \
  "srt://<server-ip>:9000?mode=caller&pkt_size=1316&latency=120"

# 拉流自 9001
ffplay "srt://<server-ip>:9001?mo
```

# Relay README

## 1. 组件职责

Relay 是整个系统的枢纽，负责：

- 接收来自 pusher 的 SRT 流
- 将同一份流分发给多个 subscriber
- 校验 TS 基本连续性与内容合法性
- 观察 telemetry private packet
- 接收 observer ACK 并计算链路时延
- 输出 WebSocket 指标给 dashboard
- 基于 subscriber 和全局状态做自动控制，并向 pusher 下发命令

即：

> Relay = 分发面 + 观测面 + 决策面。

---

## 2. 端口与服务

默认端口：

- `9000`：publisher 接入端口
- `9001`：subscriber / player / observer 接入端口
- `8765`：dashboard WebSocket 端口
- `10090`：pusher 控制端口（由 relay 主动连接 pusher）

---

## 3. 核心线程

Relay 主要包含以下线程：

- publisher accept loop
- publisher read loop
- subscriber accept loop
- monitor loop
- telemetry ACK loop
- WebSocket accept / handshake / broadcast

其中：

- `publisher_read_loop()` 负责接收原始 TS chunk
- `monitor_loop()` 负责计算统计、执行控制器、向 dashboard 推送 JSON

---

## 4. subscriber 会话模型

每个 subscriber 都有独立的 `SubscriberSession`：

- 独立队列
- 独立发送线程
- 独立 `telemetry_*` 数据
- 独立控制状态：`NORMAL / WARN / DROP / PLACEHOLDER`

这意味着 relay 的一对多不是“单队列广播”，而是**每个 subscriber 各自消费自己的发送队列**。

优点：

- 某个慢用户不会立即拖死所有人
- 可以做 per-subscriber 控制
- 可以把局部问题和全局问题区分开

---

## 5. Telemetry 与 observer

Relay 通过两步完成端到端时延统计：

### 第一步：观察原始 TS

在 `publisher_read_loop()` 中，relay 直接检查每个 TS chunk，提取 telemetry packet 的：

- `seq`
- `t_push_us`

并记录为：

- `seq -> relay_seen_wall_us`
- `seq -> relay_seen_mono_us`

### 第二步：等待 observer ACK

observer 作为一个额外 subscriber 拉同一份流，解析 telemetry 后发送 UDP ACK 给 relay。

relay 收到 ACK 后计算：

- `Push→Relay`
- `Relay→ACK`
- `Push→ACK`

这三项是本项目最有价值的链路指标。

---

## 6. SubscriberController

subscriber 控制器负责单个拉流端的局部控制。

### 状态定义

- `NORMAL`
- `WARN`
- `DROP`
- `PLACEHOLDER`

### 典型判断依据

- 队列是否持续增长
- `idle_ms`
- `telemetry_last_ack_age_ms`
- `telemetry_relay_to_ack_ms`

### 各状态含义

#### NORMAL
正常服务。

#### WARN
轻度异常，主要用于标记风险与上报状态。

#### DROP
清空积压队列，丢旧保新，继续发真实内容。

#### PLACEHOLDER
不再继续发真实内容，改发 null TS 占位包；持续严重异常时可断开该 subscriber。

---

## 7. GlobalController

global 控制器负责从“局部问题”升级到“全局问题”的判断。

### 输入指标

- `bad_subscriber_ratio`
- `dash_push_to_relay_ms`
- `dash_relay_to_ack_ms`
- `dash_push_to_ack_ms`
- `dash_last_ack_ms`
- `queue_drops`
- `pub_idle_ms`

### 输出动作

- `NOOP`
- `IDR`
- `DEGRADE_BITRATE`
- `DEGRADE_FPS`
- `DEGRADE_RESOLUTION`
- `ENTER_PLACEHOLDER`
- `RECOVER_STEP`

### 设计目标

- 少数坏用户：优先局部隔离
- 多数坏用户：升级为全局降档
- 源头 stall：进入占位保活
- 网络恢复：逐级恢复档位

---

## 8. relay → pusher 动作映射

Relay 会将全局决策转换成 pusher 控制命令：

- `IDR -> IDR`
- `DEGRADE_BITRATE -> SET_BITRATE <kbps>`
- `DEGRADE_FPS -> SET_FPS <fps>`
- `DEGRADE_RESOLUTION -> SET_RESOLUTION <w> <h>`
- `ENTER_PLACEHOLDER -> VIDEO_MODE BLACK + AUDIO_MODE SILENT`
- `RECOVER_STEP -> 退出占位 / 恢复档位 / 可补 IDR`

这是当前闭环控制的关键。

---

## 9. WebSocket 输出字段

monitor loop 每个周期向 dashboard 推送 JSON，包含：

### 全局字段

- 时间戳 / wall_time
- `pub_online`
- `ingress_mbps / egress_mbps`
- `total_ingress_mb / total_egress_mb`
- `ts_disc`
- `queue_drops`
- `pub_idle_ms`
- `dash_push_to_relay_ms`
- `dash_relay_to_ack_ms`
- `dash_push_to_ack_ms`
- `dash_last_ack_ms`
- `bad_subscriber_ratio`
- `global_state`
- `global_action`
- `global_bitrate_kbps`
- `global_fps`
- `global_width / global_height`
- 历史带宽曲线

### per-subscriber 字段

- `id`
- `peer`
- `sent_mb`
- `q`
- `latency_ms`
- `telemetry_*`
- `idle_ms`
- `ctrl`

---

## 10. 启动示例

```bash
./build/srt_relay_server \
  --bind 0.0.0.0 \
  --pub-port 9000 \
  --sub-port 9001 \
  --ws-port 8765 \
  --pusher-ctrl-port 10090
```

关闭自动控制：

```bash
./build/srt_relay_server --no-auto-control
```

关闭 dashboard WebSocket：

```bash
./build/srt_relay_server --no-ws
```

---
