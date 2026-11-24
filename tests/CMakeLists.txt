# SPDX-License-Identifier: MIT

# Checks the toolchain, the generated version header and that the vendored
# third-party headers compile with our settings.
add_executable(build_smoke build_smoke.cpp)
app_component(build_smoke)
target_link_libraries(build_smoke PRIVATE app_reshade_api app_mpv_client)
add_test(NAME build_smoke COMMAND build_smoke)

add_executable(test_dedup test_dedup.cpp)
app_component(test_dedup)
target_link_libraries(test_dedup PRIVATE app_capture)
add_test(NAME dedup COMMAND test_dedup)

# Frame generation's pacing arithmetic and the menu's words for it: header
# only, no Direct3D.
add_executable(test_fg_pacing test_fg_pacing.cpp)
app_component(test_fg_pacing)
target_include_directories(test_fg_pacing PRIVATE "${PROJECT_SOURCE_DIR}/src/framegen"
    "${PROJECT_SOURCE_DIR}/src/common" "${PROJECT_SOURCE_DIR}/src/overlay")
add_test(NAME fg_pacing COMMAND test_fg_pacing)

# Which screen frame generation goes by (the one the window is on), what it
# believes about it and the count of its refreshes: header only, no Windows.
add_executable(test_fg_screen test_fg_screen.cpp)
app_component(test_fg_screen)
target_include_directories(test_fg_screen PRIVATE "${PROJECT_SOURCE_DIR}/src/framegen"
    "${PROJECT_SOURCE_DIR}/src/common")
add_test(NAME fg_screen COMMAND test_fg_screen)

# The mouse on the areas frame generation leaves alone (the menu's editor):
# header only, no Dear ImGui.
add_executable(test_keep_edit test_keep_edit.cpp)
app_component(test_keep_edit)
target_include_directories(test_keep_edit PRIVATE "${PROJECT_SOURCE_DIR}/src/common" "${PROJECT_SOURCE_DIR}/src/overlay")
add_test(NAME keep_edit COMMAND test_keep_edit)

add_executable(test_launch test_launch.cpp)
app_component(test_launch)
target_link_libraries(test_launch PRIVATE app_core shell32)
add_test(NAME launch COMMAND test_launch)

add_executable(test_hdr test_hdr.cpp)
app_component(test_hdr)
target_link_libraries(test_hdr PRIVATE app_core)
add_test(NAME hdr COMMAND test_hdr)

add_executable(test_display test_display.cpp)
app_component(test_display)
target_link_libraries(test_display PRIVATE app_core)
add_test(NAME display COMMAND test_display)

add_executable(test_advice test_advice.cpp)
app_component(test_advice)
target_link_libraries(test_advice PRIVATE app_core)
add_test(NAME advice COMMAND test_advice)

# The launcher's words and what it saves (no window).
add_executable(test_launcher test_launcher.cpp)
app_component(test_launcher)
target_link_libraries(test_launcher PRIVATE app_core)
add_test(NAME launcher COMMAND test_launcher)

# A folder of the app under its former name, carried over at start.
add_executable(test_carry_over test_carry_over.cpp)
app_component(test_carry_over)
target_link_libraries(test_carry_over PRIVATE app_core)
add_test(NAME carry_over COMMAND test_carry_over)

add_executable(test_audio test_audio.cpp)
app_component(test_audio)
target_link_libraries(test_audio PRIVATE app_capture)
add_test(NAME audio COMMAND test_audio)

add_executable(test_view_shift test_view_shift.cpp)
app_component(test_view_shift)
target_include_directories(test_view_shift PRIVATE "${PROJECT_SOURCE_DIR}/src/common")
add_test(NAME view_shift COMMAND test_view_shift)
