# full_line_veido_proj

pusher 和 srt_server 运行在树莓派上，树莓派需要接一个USB摄像头采集数据。
srt_server 是一个srt 服务器用于用于中转数据(接收推流端数据后推送给播放端，播放端可有多个)。

player端运行在Windows， 编译运行用的是vcpkg,ffmpeg和Qt
player下的build不用看，是系统生成的。

observer用于不准确地计算延迟

dashboard.html可直接连接用于观察中继上传的各种数据。

链路的各个节点徐注意更改为自己树莓派或其他设备的IP

详细运行指令可参考各文件夹下的ReadMe.md