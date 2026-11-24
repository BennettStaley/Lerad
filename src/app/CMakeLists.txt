# SPDX-License-Identifier: MIT
# The exe: settings, setup (component downloads), capture card -> mpv.

add_library(app_core STATIC
    advice.cpp
    backdrop.cpp
    stream_window.cpp
    bug_report.cpp
    carry_over.cpp
    components.cpp
    display.cpp
    files.cpp
    frame_preview.cpp
    gpu.cpp
    hdr.cpp
    http.cpp
    input_program.cpp
    launcher_text.cpp
    mpv_launch.cpp
    reshade_config.cpp
    settings.cpp
    system_report.cpp
    vrr.cpp)
app_component(app_core)
target_include_directories(app_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# The frame generation add-on's pacing rules (a header): advice.cpp asks them
# what will be shown rather than repeating them.
target_include_directories(app_core PRIVATE "${PROJECT_SOURCE_DIR}/src/framegen")

# The tested versions of the downloaded programs (components.json), built in.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/components.json")
file(READ components.json APP_TESTED_COMPONENTS)
configure_file(tested_components.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/tested_components.h" @ONLY)
target_include_directories(app_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(app_core PUBLIC app_capture app_common app_version dxgi cfgmgr32 winhttp bcrypt wintrust crypt32 version user32 ole32 windowscodecs)

configure_file(app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
configure_file(app.manifest.in "${CMAKE_CURRENT_BINARY_DIR}/app.manifest" @ONLY)
add_executable(app WIN32 main.cpp launcher.cpp setup_ui.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/app.manifest" "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
set_target_properties(app PROPERTIES OUTPUT_NAME "${PROJECT_NAME}")
app_component(app)
target_link_libraries(app PRIVATE app_core app_imgui shell32 user32 comctl32 dwmapi d3d11)

install(TARGETS app RUNTIME DESTINATION .)
install(FILES mpv_config/mpv.conf mpv_config/input.conf DESTINATION engine/portable_config)
install(FILES mpv_config/scripts/scaling.lua mpv_config/scripts/window.lua
              mpv_config/scripts/screenshot.lua
        DESTINATION engine/portable_config/scripts)
install(FILES mpv_config/shaders/fsr1.glsl DESTINATION engine/portable_config/shaders)
# Third-party mpv shaders, unmodified (third_party/README.md): the cnn and
# anime4k scaling modes.
set(app_shaders "${PROJECT_SOURCE_DIR}/third_party")
install(FILES
    "${app_shaders}/artcnn/ArtCNN_C4F16.glsl"
    "${app_shaders}/anime4k/Anime4K_Clamp_Highlights.glsl"
    "${app_shaders}/anime4k/Anime4K_Restore_CNN_M.glsl"
    "${app_shaders}/anime4k/Anime4K_Upscale_CNN_x2_M.glsl"
    "${app_shaders}/anime4k/Anime4K_AutoDownscalePre_x2.glsl"
    "${app_shaders}/anime4k/Anime4K_AutoDownscalePre_x4.glsl"
    "${app_shaders}/anime4k/Anime4K_Upscale_CNN_x2_S.glsl"
    DESTINATION engine/portable_config/shaders)
