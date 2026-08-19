cmake_minimum_required(VERSION 3.14)
project(udp-proxy CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(udp-proxy
    src/main.cpp
    src/udp_proxy.cpp
    src/udp_proxy.hpp
)

target_include_directories(udp-proxy PRIVATE ${CMAKE_SOURCE_DIR}/creek/tight/src)

find_package(Threads REQUIRED)

add_subdirectory(creek/tight)

# Opus 音频编码库（音频通道 1：PCM→Opus→tight→Opus→waveOut）
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_DOCS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/opus-1.5.2)

add_executable(tight-test src/tight_test.cpp)
target_link_libraries(tight-test PRIVATE tight)

add_executable(test-file-data src/test_file_data.cpp)
target_link_libraries(test-file-data PRIVATE tight)

add_executable(test-opus src/test_opus.cpp)
target_link_libraries(test-opus PRIVATE Opus::opus)

if(WIN32)
    target_link_libraries(udp-proxy ws2_32)

    # Media Foundation 视频 + ffmpeg.exe 子进程（QSV 硬编 / 音频解码）+ Opus 编码
    add_executable(video-sender src/video_sender.cpp src/opus_audio.cpp src/qsv_video.cpp)
    target_link_libraries(video-sender PRIVATE tight Opus::opus
        mfplat mfreadwrite mfuuid mf ole32 strmiids wmcodecdspuuid)

    add_executable(video-player src/video_player.cpp src/opus_audio.cpp)
    target_link_libraries(video-player PRIVATE tight Opus::opus mfplat mfuuid mf wmcodecdspuuid winmm ole32)
endif()
