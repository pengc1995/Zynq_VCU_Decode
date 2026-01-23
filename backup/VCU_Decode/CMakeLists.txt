cmake_minimum_required(VERSION 3.16)
project(VCU_DecodeMinimal CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Cross-compilation: set your sysroot path
set(SYSROOT /home/chelly/Work/chronos5-sdk/uby22_sysroot)

# Tell compiler and linker to use sysroot
set(CMAKE_SYSROOT ${SYSROOT})

# Add GStreamer include dirs from sysroot
include_directories(${SYSROOT}/usr/include/gstreamer-1.0)
include_directories(${SYSROOT}/usr/include/glib-2.0)
include_directories(${SYSROOT}/usr/lib/glib-2.0/include)

# Add source files
add_executable(vcu_decode_minimal
    main.cpp
    VCU_Decode.cpp
)

# Link GStreamer libraries from sysroot
target_link_libraries(vcu_decode_minimal
    ${SYSROOT}/usr/lib/aarch64-linux-gnu/libgstreamer-1.0.so
    ${SYSROOT}/usr/lib/aarch64-linux-gnu/libgobject-2.0.so
    ${SYSROOT}/usr/lib/aarch64-linux-gnu/libglib-2.0.so
    ${SYSROOT}/usr/lib/aarch64-linux-gnu/libgstapp-1.0.so
)

include_directories(
    ${SYSROOT}/usr/include/glib-2.0
    ${SYSROOT}/usr/lib/glib-2.0/include
    ${SYSROOT}/usr/include/gstreamer-1.0
    ${SYSROOT}/usr/lib/aarch64-linux-gnu/glib-2.0/include
)
