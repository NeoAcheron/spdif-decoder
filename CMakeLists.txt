cmake_minimum_required (VERSION 2.8.11)
project (spdif-decoder)

add_executable (spdif-decoder 
    codechandler.c 
    helper.c
    myspdif.c
    myspdifdec.c
    resample.c
    spdif-loop.c
)

SET(FFMPEG ${CMAKE_CURRENT_SOURCE_DIR}/../ffmpeg-4.3.1)

target_include_directories (spdif-decoder
    PUBLIC ${FFMPEG}
)

FIND_LIBRARY(libavcodec avcodec)
FIND_LIBRARY(libavformat avformat)
FIND_LIBRARY(libavdevice avdevice)
FIND_LIBRARY(libavutil avutil)
FIND_LIBRARY(libswresample swresample)
FIND_LIBRARY(libavfilter avfilter)
TARGET_LINK_LIBRARIES(spdif-decoder 
    ${libavcodec}
    ${libavformat}
    ${libavdevice}
    ${libavutil}
    ${libswresample}
    ${libavfilter}
    ao
    m
)
