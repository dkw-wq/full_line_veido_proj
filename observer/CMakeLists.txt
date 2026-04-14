cmake_minimum_required(VERSION 3.16)
project(observer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(observer observer.cpp)

target_include_directories(observer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_library(SRT_LIB srt REQUIRED)
if (WIN32)
  target_link_libraries(observer PRIVATE ${SRT_LIB} ws2_32)
else()
  target_link_libraries(observer PRIVATE ${SRT_LIB} pthread)
endif()
