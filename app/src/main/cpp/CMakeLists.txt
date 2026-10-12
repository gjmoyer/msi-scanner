cmake_minimum_required(VERSION 3.22.1)

project(msi_scanner CXX)

add_library(
    msi_scanner
    SHARED
    msi_decoder.cpp
    jni_bridge.cpp
)

target_compile_features(msi_scanner PRIVATE cxx_std_17)

target_compile_options(
    msi_scanner
    PRIVATE
    -O3
    -Wall
    -Wextra
    -fvisibility=hidden
    -ffunction-sections
    -fdata-sections
)

target_link_options(msi_scanner PRIVATE -Wl,--gc-sections)
