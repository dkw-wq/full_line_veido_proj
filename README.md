# full_line_veido_proj

pusher 和 srt_server 运行在树莓派上，树莓派需要接一个USB摄像头，通过注册为 /dev/vedio0 采集数据。
srt_server 是一个srt 服务器用于用于中中转数据。

player端运行在Windows， 编译运行用的是vcpkg,ffmpeg和Qt

详细运行指令可参考各文件夹下的ReadMe.md