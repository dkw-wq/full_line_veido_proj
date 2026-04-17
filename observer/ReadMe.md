运行示例：
./observer --url "srt://10.160.196.17:9001?mode=caller&latency=20&streamid=cam1" --ack-host 10.160.196.17


g++ -std=c++17 -O2 -o observer observer.cpp -lsrt -lpthread