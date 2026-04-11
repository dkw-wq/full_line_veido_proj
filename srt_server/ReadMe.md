
```

在树莓派上重新编译运行：

cd ~/dir1/vedio_proj/srt
cmake -S . -B build
cmake --build build -j4
./build/srt_relay_server --bind 0.0.0.0 --pub-port 9000 --sub-port 9001


代码运行检查实验:
推流/拉流命令继续用这组：

# 推流到 9000
ffmpeg -re -stream_loop -1 -i input.mp4 \
  -c:v libx264 -preset veryfast -tune zerolatency \
  -c:a aac -f mpegts \
  "srt://<server-ip>:9000?mode=caller&pkt_size=1316&latency=120"

# 拉流自 9001
ffplay "srt://<server-ip>:9001?mo

```