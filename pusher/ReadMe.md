
```

gcc srt_cam_push.c -o srt_cam_push \
  $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-video-1.0 glib-2.0) \
  -lasound -lpthread

./srt_cam_push 


```