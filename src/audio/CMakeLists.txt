# SPDX-License-Identifier: MIT
# capture_audio.dll: the card's sound in mpv's process (an mpv C plugin).

add_library(app_audio SHARED audio_plugin.cpp)
# mpv names a script after its file: script-opts capture_audio-...
set_target_properties(app_audio PROPERTIES OUTPUT_NAME "capture_audio")
app_component(app_audio)
target_link_libraries(app_audio PRIVATE app_mpv_client ole32 avrt)
install(TARGETS app_audio RUNTIME DESTINATION engine/portable_config/scripts)
