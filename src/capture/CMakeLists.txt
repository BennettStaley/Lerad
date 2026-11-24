# SPDX-License-Identifier: MIT
# Capture: finding cards, reading frames (Media Foundation), dedup.

add_library(app_capture STATIC
    audio_endpoint.cpp
    capture.cpp
    devices.cpp
    magewell.cpp
    log.cpp)
app_component(app_capture)
target_include_directories(app_capture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(app_capture PUBLIC
    mfplat mf mfreadwrite mfuuid strmiids ole32 oleaut32 cfgmgr32)
