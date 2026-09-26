# Windows SRT 拉流压测

单进程创建多个无界面的 SRT 拉流连接，逐步加压并按秒输出连接数、总接收码率、每个客户端码率分布、SRT 丢包/过期丢弃和平均 RTT。可用 `--csv` 保存相同的逐秒统计。程序只读取并丢弃流，不解码。

在仓库根目录的 PowerShell 中编译（将 vcpkg 路径改为本机实际路径）：

```powershell
$vcpkg = 'E:\vcpkg'
& "$vcpkg\vcpkg.exe" install libsrt:x64-windows
cmake -S load_test -B load_test/build -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_TOOLCHAIN_FILE=${vcpkg}/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build load_test/build --config Release
```

先启动中继与一条持续推流（见根目录 README），再从本 PC 执行：

```powershell
.\load_test\build\Release\srt_load_test.exe --host jfznbx.cn --port 9001 --clients 50 --ramp-ms 200 --duration-sec 120 --csv load_test\results.csv
```

`--duration-sec` 从最后一个客户端**开始连接**后计时。默认参数：`--clients 1 --ramp-ms 200 --duration-sec 60 --latency-ms 20 --connect-timeout-ms 5000 --idle-ms 3000 --stream-id cam1`。用 `--help` 查看全部参数。要模拟慢客户端，可加 `--slow-every 10 --slow-read-ms 500`，使每第 10 个客户端每 500 毫秒只读一个包。

`connect_fail` 表示握手失败；`disconnect` 表示已连接后断开；`idle` 表示连接超过指定时长未收到数据。`loss` 是 SRT 检测到的缺包数，其中部分可能已重传恢复；`drop` 才是接收端因过期而丢弃的包数。无推流或未等到关键帧时，客户端可连接但没有数据。中继侧同时观察 Dashboard 的 `queue_drops`、`egress_mbps`、`subscribers`，以及服务器 CPU、内存和出口带宽。端到端画面延迟不由此程序测量。
