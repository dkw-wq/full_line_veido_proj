# Windows SRT 拉流压测

单进程创建多个无界面的 SRT 拉流连接，逐步加压并按秒输出连接数、总接收码率、每个客户端码率分布、SRT 丢包/过期丢弃和平均 RTT。可用 `--csv` 保存相同的逐秒统计。程序只读取并丢弃流，不解码。

在仓库根目录的 PowerShell 中编译（将 vcpkg 路径改为本机实际路径）：

```powershell
$vcpkg = 'E:\vcpkg'
& "$vcpkg\vcpkg.exe" install libsrt:x64-windows
cmake -S load_test -B load_test/build -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_TOOLCHAIN_FILE=${vcpkg}/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build load_test/build --config Release
```

先启动中继与一条持续推流（见根目录 README），再从本 PC 执行。压测记录统一放在 `load_test/results/`：

```powershell
New-Item -ItemType Directory -Force load_test\results | Out-Null
.\load_test\build\Release\srt_load_test.exe --host jfznbx.cn --port 9001 --clients 50 --ramp-ms 200 --duration-sec 120 --csv load_test\results\results.csv --trace load_test\results\trace.csv
```

`--duration-sec` 从最后一个客户端**开始连接**后计时。默认参数：`--clients 1 --ramp-ms 200 --duration-sec 60 --latency-ms 20 --connect-timeout-ms 5000 --idle-ms 3000 --stream-id cam1`。用 `--help` 查看全部参数。要模拟慢客户端，可加 `--slow-every 10 --slow-read-ms 500`，使每第 10 个客户端每 500 毫秒只读一个包。程序给每路 SRT Stream ID 追加 `~lt-进程号-客户端号`，中继连接日志记录此 ID，便于跨端关联。

## CSV 字段

CSV 每行是一次采样，通常间隔约 1 秒；计算码率时使用实际采样间隔。

| 字段 | 含义 |
| --- | --- |
| `elapsed_s` | 从程序启动到本次采样经过的秒数。 |
| `attempted` | 累计开始尝试建立的连接数，包括仍在连接中的客户端。 |
| `connected` | 本次采样时仍在线的连接数。 |
| `connect_fail` | 累计连接失败数，包括创建/配置套接字失败、握手失败和连接超时。 |
| `disconnect` | 成功连接后意外断开的累计数量；测试结束时正常关闭不计入。 |
| `idle` | 在线连接中，连续至少 `--idle-ms` 未收到数据的数量。 |
| `rx_mbps` | 本次采样间隔内，所有客户端通过 `srt_recv` 收到的总数据量换算成 Mbps；不含 SRT 协议与重传开销。 |
| `min_client_mbps` | 本次采样中，在线客户端接收码率的最小值。 |
| `p50_client_mbps` | 本次采样中，在线客户端接收码率的第 50 百分位（中位数）。 |
| `p95_client_mbps` | 本次采样中，在线客户端接收码率的第 95 百分位。 |
| `p95_connect_ms` | 从测试开始至本次采样，所有成功连接的握手耗时第 95 百分位，单位毫秒；包含后来断开的客户端。 |
| `loss` | 客户端累计检测到的 SRT 缺包数，部分缺包可能已通过重传恢复。 |
| `drop` | 客户端累计因数据到达过晚而丢弃的 SRT 包数；不同于中继的 `queue_drops`。 |
| `avg_rtt_ms` | 本次采样时所有在线客户端的平均 SRT 往返时延，单位毫秒；不是端到端画面延迟。 |
| `loop_gap_max_ms` | 截至本次采样，主事件循环两次迭代间隔的最大值；包括正常的最多 100 ms epoll 等待。 |
| `launch_lag_max_ms` | 实际发起连接比按测试起点和 `--ramp-ms` 计算的目标时间晚的最大值，反映累计加压调度延迟；程序仍保证相邻连接至少间隔指定时长。 |
| `start_call_max_ms` | 单路创建、配置、发起 SRT 连接所用时间的最大值。 |
| `event_batch_max_ms` | 一批 epoll 事件处理时间的最大值。 |
| `event_batch_interval_max_ms` | 本次约 1 秒采样间隔内，一批 epoll 事件处理时间的最大值。 |
| `max_rcvbuf_packets` | 本次采样中在线客户端的 SRT 接收缓冲占用包数最大值；不是主机 UDP 接收队列。 |
| `min_avail_rcvbuf_bytes` | 本次采样中在线客户端的 SRT 接收缓冲剩余字节数最小值；接近 0 表示接收缓冲可能被占满。 |

`--trace` 另存每路事件 CSV：`wall_time` 是本地时间，`elapsed_ms` 是测试启动后的毫秒数，`client` 是客户端序号，`event` 为 `connecting/connected/failed/finished`，`local_port` 是本机 UDP 端口，`srt_socket`/`srt_state` 是当时的 SRT 句柄和状态，`connect_ms` 是该路开始连接后的耗时，`launch_lag_ms` 是该路启动调度延迟，`loop_gap_max_ms` 是当时主循环最大间隔，`stream_id` 可在服务器 `sub#... started` 日志中检索，`detail` 是失败原因。无法取得本地端口时记为 `-1`；所有失败行也写到标准错误。

`attempted`、`connect_fail`、`disconnect`、`loss`、`drop`、`p95_connect_ms` 按测试开始以来的数据计算；`connected`、`idle` 和各客户端码率统计只反映当前在线连接。在加连接阶段，新连接尚未收到数据时，最小码率可能为 0。`--ramp-ms` 是目标连接间隔；可用 `attempted` 与 `elapsed_s` 检查实际加压速度，若明显落后，不能用该次测试推算中继容量。程序结束时可能补写与上一行时间几乎相同的一行，其中 `rx_mbps=0` 不代表真实流量突然归零，可忽略该行。

## 如何判断结果

无推流或未等到关键帧时，客户端可连接但没有数据。测试时同时观察 Dashboard 的 `queue_drops`、`egress_mbps`、`subscribers`、每路 SRT 指标以及主机网卡/CPU/内存；服务器周期日志也保存相同的关键指标。仅凭客户端 CSV 不能定位瓶颈：应使用固定码率的输入流，再对照两端的资源与网络指标；必要时增加另一台压测机器，以区分单台 PC 和网络路径的限制。
