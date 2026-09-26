# SRT 一对多低延迟视频链路

树莓派上的 `pusher` 采集 USB 摄像头和音频，通过 SRT 推给 Linux 中继 `srt_server`；Windows 上的 `player` 从中继拉流。`dashboard.html` 通过 WebSocket 查看中继状态。`observer/` 目前没有可运行程序。

以下命令除特别说明外，都在**仓库根目录**执行。启动顺序：中继 → 推流端 → 播放端 / Dashboard。

## 1. Linux 中继：编译、前台运行

需要 CMake、C++17 编译器、`pkg-config` 和 libsrt 开发包。

```bash
cmake -S srt_server -B srt_server/build
cmake --build srt_server/build -j4
./srt_server/build/srt_relay_server --bind 0.0.0.0 --pub-port 9000 --sub-port 9001 --ws-port 8765
```

端口：`9000` 接收推流，`9001` 提供拉流，`8765` 提供 Dashboard WebSocket；默认启用对推流端 `10090` 端口的自动控制。仅调试时可加 `--no-auto-control`，不使用 Dashboard 时可加 `--no-ws`。查看全部参数：`./srt_server/build/srt_relay_server --help`。

### systemd 后台运行

仓库提供 [`srt_server/deploy/srt-server.service`](srt_server/deploy/srt-server.service)，其中程序路径固定为 `/opt/srt_server/build/srt_relay_server`。先将 `srt_server` 放到 `/opt/srt_server` 并在该目录编译，再安装、启动服务：

```bash
sudo cp srt_server/deploy/srt-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now srt-server
sudo systemctl status srt-server
journalctl -u srt-server -f
```

## 2. 树莓派推流端：编译、运行

需要 CMake、`pkg-config`、GStreamer 及其视频/SRT/编码插件、ALSA 开发包。先在 [`pusher/app_config.c`](pusher/app_config.c) 中确认摄像头设备路径和中继地址；当前默认推向 `jfznbx.cn:9000`，控制端口为 `10090`。

```bash
cmake -S pusher -B pusher/build
cmake --build pusher/build -j4
./pusher/build/srt_cam_push
```

## 3. Windows 播放端：编译、运行

在 PowerShell 中执行；需要 Visual Studio 2022、CMake、vcpkg。将 `$vcpkg` 改成实际安装目录。首次运行或更新 Qt/vcpkg 后执行 `windeployqt`。

```powershell
$vcpkg = 'E:\vcpkg'
& "$vcpkg\vcpkg.exe" install 'ffmpeg[srt]:x64-windows' 'qtmultimedia[ffmpeg,widgets]:x64-windows'
cmake -S player -B player/build -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_TOOLCHAIN_FILE=${vcpkg}/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build player/build --config Release
& "$vcpkg\installed\x64-windows\tools\Qt6\bin\windeployqt.exe" --release player\build\Release\srt_player.exe
.\player\build\Release\srt_player.exe 'srt://jfznbx.cn:9001?mode=caller&latency=20&streamid=cam1'
```

也可不传 URL 直接运行，程序默认使用上面的公网地址；使用其他中继时替换 URL 主机名。播放状态写入仓库根目录的 `player_status.log`。

## 4. Dashboard

直接用浏览器打开 [`dashboard.html`](dashboard.html)，将页面中的地址改为 `ws://<中继地址>:8765`，点击“连接”。

## 5. 不接摄像头时测试中继

在装有 FFmpeg 的机器上，用一个本地 `input.mp4` 代替推流端；另一终端用 ffplay 拉流。将 `<中继地址>` 替换成实际 IP 或域名。

```bash
ffmpeg -re -stream_loop -1 -i input.mp4 -c:v libx264 -preset veryfast -tune zerolatency -c:a aac -f mpegts 'srt://<中继地址>:9000?mode=caller&pkt_size=1316&latency=120'
ffplay 'srt://<中继地址>:9001?mode=caller&latency=120'
```

组件细节见 [`pusher/ReadMe.md`](pusher/ReadMe.md)、[`srt_server/ReadMe.md`](srt_server/ReadMe.md) 和 [`player/ReadMe.md`](player/ReadMe.md)。
