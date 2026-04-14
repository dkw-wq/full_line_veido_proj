方案二（旁路 observer）接入说明

1. 主播放器不改
   - 继续使用现在不卡的 main_player_no_telemetry.cpp / 你当前恢复后的版本。
   - 不再把 telemetry/custom AVIO/raw TS tap 放进主播放器进程。

2. 新增 observer 进程
   文件：srt_ts_observer.cpp
   作用：
   - 单独订阅同一条 SRT 播放流
   - 只读原始 TS 字节
   - 解析 telemetry.h 里的 private TS 包
   - 用旧的 TelemetryAck(seq) UDP ACK 到 relay 的 TLMY_ACK_PORT


   srt_ts_observer "srt://10.160.196.17:9001?mode=caller&latency=40&streamid=cam1"

   如果 relay_host 和 URL host 不同：
   srt_ts_observer "srt://10.160.196.17:9001?mode=caller&latency=40&streamid=cam1" 10.160.196.17

   运行示例：
   ./observer --url "srt://10.160.196.17:9001?mode=caller&latency=40&streamid=cam1" --ack-host 10.160.196.17


3. 中继端替换
   文件：srt_relay_observer_mode.cpp
   相比原版：
   - 在 publisher_read_loop() 里直接 observe_chunk() 扫描 push 过来的 telemetry TS 包
   - 保存 seq -> {t_push_us, relay_seen_wall_us, relay_seen_mono_us}
   - 监听旧的 TLMY_ACK_PORT
   - 收到 observer 的 TelemetryAck(seq) 后，计算：
       push_to_relay_ms
       relay_to_ack_ms
       push_to_ack_ms
   - 在 monitor_loop / WebSocket JSON 中展示：
       dash_push_to_relay_ms
       dash_relay_to_ack_ms
       dash_push_to_ack_ms

4. 注意事项
   - observer 也会占用一个 subscriber 连接，所以 relay 的 subscriber_count 会比真实播放器多 1。
   - 这套方案测到的是：
       推流端写时间戳 -> relay收到 -> observer所在机器收到并解析 -> ACK回到relay
     不是“真正视频渲染到屏幕的时刻”，但不会影响主播放器流畅度。

5. 什么时候需要进一步升级
   - 如果后续一定要测“显示级延迟”，再考虑更复杂方案。
   - 当前阶段，这套方案是最稳的工程折中。

