cmake_minimum_required(VERSION 3.21)
project(srt_load_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_path(SRT_INCLUDE_DIR NAMES srt/srt.h REQUIRED)
find_library(SRT_LIBRARY NAMES srt REQUIRED)

add_executable(srt_load_test main.cpp)
target_include_directories(srt_load_test PRIVATE ${SRT_INCLUDE_DIR})
target_link_libraries(srt_load_test PRIVATE ${SRT_LIBRARY})
if(WIN32)
    target_link_libraries(srt_load_test PRIVATE ws2_32)
    target_compile_definitions(srt_load_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# vcpkg's x64-windows triplet uses a DLL. Keep it beside the executable.
get_filename_component(SRT_LIB_DIR "${SRT_LIBRARY}" DIRECTORY)
get_filename_component(SRT_TRIPLET_DIR "${SRT_LIB_DIR}" DIRECTORY)
if(WIN32 AND EXISTS "${SRT_TRIPLET_DIR}/bin/srt.dll")
    add_custom_command(TARGET srt_load_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SRT_TRIPLET_DIR}/bin/srt.dll"
            "$<TARGET_FILE_DIR:srt_load_test>")
endif()
