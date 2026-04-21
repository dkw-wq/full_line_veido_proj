```
编译运行示例：
./observer --url "srt://10.160.196.17:9001?mode=caller&latency=20&streamid=cam1" --ack-host 10.160.196.17


g++ -std=c++17 -O2 -o observer observer.cpp -lsrt -lpthread
```

# telemetry_observer

`telemetry_observer` 是一个轻量级的 **SRT 旁路观测端（observer）**。

它以普通 subscriber 的方式连接到 relay 的拉流端口，接收同一份 SRT/MPEG-TS 数据流，解析其中由 pusher 注入的 telemetry 私有 TS 包，然后把解析出的 `seq` 通过 UDP ACK 回传给 relay。relay 收到 ACK 后即可计算：

- `Push -> Relay`
- `Relay -> ACK`
- `Push -> ACK`

这些结果会被 relay 用于 dashboard 展示和全局控制器判定。

---

## 1. 作用定位

observer 不是播放器，也不是转发器。

它的职责只有三件事：

1. 连接 relay 的 SRT 拉流端口，像普通 subscriber 一样收流。
2. 在收到的 TS 数据中识别 telemetry 私有包。
3. 对每个新的 telemetry `seq` 发送一次 UDP ACK 给 relay。

因此它的价值在于：

- 给 relay 提供端到端链路测量闭环。
- 不需要改动主播放器逻辑。
- 可以独立部署、独立重连、独立测试 ACK 链路。

---

## 2. 工作原理

整体流程如下：

```text
pusher --(SRT MPEG-TS + telemetry packet)--> relay --(SRT)--> observer
observer --(UDP ACK seq)--> relay
relay -> 计算 push->relay / relay->ack / push->ack
```

运行细节：

- pusher 在 MPEG-TS 流里周期性插入 telemetry TS 包。
- relay 在 publisher 入口处记录 `seq -> t_push_us`。
- observer 从收到的数据里解析 telemetry TS 包。
- observer 对每个新 `seq` 只 ACK 一次，避免重复发送。
- relay 收到 ACK 后，根据保存的时间戳计算链路延迟。

---

## 3. 程序结构

主要组件：

- `parse_srt_url()`：解析 `srt://host:port?...`，提取 host、port、latency、streamid、passphrase、pbkeylen。
- `AckSender`：负责通过 UDP 向 relay 的 ACK 端口发送 telemetry ACK。
- `TsObserver`：从接收到的 TS 数据里解析 telemetry 包，并做去重 ACK。
- `SrtObserverClient`：负责 SRT 连接、接收、断线重连与主循环。

---

## 4. 命令行参数

程序帮助信息：

```text
Usage: telemetry_observer --url <srt://host:port?...> [options]
  --ack-host         <ip>   default: same as url host
  --ack-port         19902
  --latency-ms       40     override url latency
  --recv-timeout-ms  2000
  --reconnect-ms     500
  --streamid         <str>  override url streamid
  --passphrase       <str>
  --pbkeylen         16|24|32
```

### 必选参数

- `--url`
  - observer 要连接的 SRT 地址。
  - 一般填 relay 的 subscriber 端口，例如：
    - `srt://10.160.196.17:9001?latency=20`

### 可选参数

- `--ack-host`
  - ACK 的目标地址。
  - 默认取 `--url` 中的 host，也就是默认把 ACK 回给 relay 所在主机。

- `--ack-port`
  - ACK 的 UDP 端口。
  - 默认是 `TLMY_ACK_PORT`，当前帮助信息显示为 `19902`。

- `--latency-ms`
  - 覆盖 URL 中的 SRT latency。
  - 如果设置了这个值，优先使用该值。

- `--recv-timeout-ms`
  - `srt_recv()` 的接收超时。
  - 超时后不会退出，而是继续收流。

- `--reconnect-ms`
  - observer 断线重连间隔。

- `--streamid`
  - 覆盖 URL 中的 `streamid`。

- `--passphrase` / `--pbkeylen`
  - 如果 relay 启用了 SRT 加密，observer 需要配置相同参数才能连接成功。

---

## 5. 典型启动方式

### 最简单启动

```bash
./telemetry_observer \
  --url "srt://10.160.196.17:9001?latency=20"
```

### 显式指定 ACK 地址与端口

```bash
./telemetry_observer \
  --url "srt://10.160.196.17:9001?latency=20" \
  --ack-host 10.160.196.17 \
  --ack-port 19902
```

### 带 streamid / passphrase 的启动方式

```bash
./telemetry_observer \
  --url "srt://10.160.196.17:9001?latency=20&streamid=obs001" \
  --passphrase your_secret \
  --pbkeylen 16
```

---

## 6. 与 relay 的配合关系

observer 需要和 relay 配套使用，关键点如下：

### relay 侧要求

- relay 必须已经启动并监听 subscriber 端口。
- relay 必须开启 telemetry ACK 接收端口。
- relay 的 dashboard / monitor loop 才能展示并使用 observer 回传的 ACK 结果。

### observer 连接目标

observer 连接的是：

- relay 的 `sub-port`

不是：

- pusher 的 `pub-port`
- dashboard 的 WebSocket 端口

### observer 在系统中的身份

observer 本质上就是一个特殊的 subscriber。

因此在 relay 端：

- 它会占用一个 subscriber 连接。
- 它可能出现在 dashboard 的拉流端列表中。
- 它的 IP 会用于 relay 将 ACK 匹配回对应 subscriber telemetry。

---

## 7. ACK 去重机制

observer 不会对同一个 telemetry `seq` 重复 ACK。

实现方式：

- `recent_`：保存最近已 ACK 的 `seq`
- `order_`：维护一个有限长度 FIFO
- 窗口大小：`8192`

这样可以避免：

- 重复 TS 包导致重复 ACK
- 长时间运行后内存无限增长

---

## 8. 断线与重连

observer 的运行策略是“持续在线”：

- 连接失败：等待 `reconnect_ms` 后重试。
- 接收异常：关闭 socket，重新连接。
- `SRT_ETIMEOUT`：不退出，仅视为一次接收超时。
- 收到 `SIGINT` / `SIGTERM`：优雅退出。

这意味着 observer 很适合长期后台运行。

---

## 9. 日志说明

运行时常见输出：

- `[OBS] connected to ...`
  - 成功连接到 relay。

- `[OBS] recv failed: ...`
  - 当前 SRT 接收失败，程序会走重连流程。

- `[OBS] reconnecting...`
  - observer 正在等待下一次重连。

- `[OBS] bad ack host: ...`
  - ACK 回传目标地址非法。

- `[OBS] invalid srt url: ...`
  - `--url` 格式不正确。

---

## 10. 常见问题

### 10.1 observer 连接不上 relay

先检查：

- relay 的 `sub-port` 是否正确。
- `--url` 是否写成了 subscriber 端口，而不是 publisher 端口。
- 是否配置了正确的 `passphrase/pbkeylen`。
- relay 是否只绑定在某个特定网卡地址上。

### 10.2 observer 连接成功，但 dashboard 没有 ACK 指标

先检查：

- pusher 是否真的在 TS 中注入 telemetry。
- observer 的 `--ack-host` 是否正确指向 relay。
- relay 是否开放了 telemetry ACK 的 UDP 端口。
- observer 与 relay 之间是否有防火墙拦截 UDP ACK。

### 10.3 observer 出现在拉流端列表里，这正常吗

正常。

因为 observer 就是以 subscriber 身份接入 relay，所以它本来就会被 relay 统计为一个拉流端。

### 10.4 一个 seq 会不会重复 ACK

正常情况下不会。

observer 内部会对最近的 telemetry `seq` 做去重，只对新序号发送一次 ACK。

---

## 11. 推荐部署方式

推荐把 observer 与 dashboard / relay 一起部署在同一内网环境，保证：

- ACK 回传链路短且稳定。
- 便于排查 push->relay / relay->ack / push->ack 的差异。
- 可以独立于正式 player 做链路观测。

常见做法：

- relay 一台机器
- observer 同机或同网段部署
- player 作为真实业务消费端单独运行

---

## 12. 与其他模块的关系

- `pusher`
  - 负责把 telemetry TS 包注入到 MPEG-TS 流中。

- `relay`
  - 负责记录 publisher 入口时间戳，接收 ACK，并计算链路指标。

- `observer`
  - 负责从拉流副本中解析 telemetry 并回 ACK。

- `player`
  - 负责实际播放，不承担 telemetry ACK 职责。

因此 observer 是你整个链路测量体系里的“测量探针”。
