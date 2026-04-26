cmake_minimum_required(VERSION 3.16)
project(srt_relay_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SRT REQUIRED srt)

add_executable(srt_relay_server
    src/main.cpp
    src/ws_util.hpp
    src/server_context.hpp
    src/ws_broadcaster.hpp
    src/ts_validator.hpp
    src/subscriber_controller.hpp
    src/global_controller.hpp
    src/pusher_control_client.hpp
    src/subscriber_session.hpp
    src/telemetry_relay.hpp
    src/relay_server.hpp
    src/cli.hpp
)

target_include_directories(srt_relay_server PRIVATE ${SRT_INCLUDE_DIRS})
target_link_directories(srt_relay_server PRIVATE ${SRT_LIBRARY_DIRS})
target_link_libraries(srt_relay_server PRIVATE ${SRT_LIBRARIES} pthread)
target_compile_options(srt_relay_server PRIVATE ${SRT_CFLAGS_OTHER})
