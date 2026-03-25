#!/bin/bash

if [ $# -ne 2 ]; then
    echo "Usage: ./convert.sh width height"
fi

width=$1
height=$2

ffmpeg -f rawvideo -pixel_format rgb0 -video_size ${width}x${height} -i frame_rgb.data frame_rgb.png
ffmpeg -f rawvideo -pixel_format nv12 -video_size ${width}x${height} -i frame_yuv.data frame_yuv.png
