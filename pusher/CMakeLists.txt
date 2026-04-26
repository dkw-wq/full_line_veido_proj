cmake_minimum_required(VERSION 3.16)
project(srt_cam_push LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-video-1.0 glib-2.0)
pkg_check_modules(ALSA REQUIRED alsa)

add_executable(srt_cam_push
    srt_cam_push.c
    app_config.c
    audio_device.c
    control_server.c
    gst_runtime.c
    telemetry_injector.c
)

target_include_directories(srt_cam_push PRIVATE
    ${GSTREAMER_INCLUDE_DIRS}
    ${ALSA_INCLUDE_DIRS}
)
target_link_directories(srt_cam_push PRIVATE
    ${GSTREAMER_LIBRARY_DIRS}
    ${ALSA_LIBRARY_DIRS}
)
target_link_libraries(srt_cam_push PRIVATE
    ${GSTREAMER_LIBRARIES}
    ${ALSA_LIBRARIES}
    pthread
)
target_compile_options(srt_cam_push PRIVATE
    ${GSTREAMER_CFLAGS_OTHER}
    ${ALSA_CFLAGS_OTHER}
)
