# SPDX-License-Identifier: MPL-2.0
# Frame generation: the ReShade add-on (zz-framegen.addon64; "zz-" so
# ReShade loads it after the DLSS 5 add-ons) and its offline evaluator.

include(shaders.cmake)

# fxc (APP_FXC) is found by the root CMakeLists.txt.

set(_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${_shader_dir}")
set(_shader_headers)
foreach(_item IN LISTS APP_FRAMEGEN_SHADERS)
    string(REPLACE "|" ";" _shader "${_item}")
    list(GET _shader 0 _profile)
    list(GET _shader 1 _entry)
    list(GET _shader 2 _define)
    list(GET _shader 3 _variable)
    list(GET _shader 4 _header)
    list(GET _shader 5 _source)
    set(_define_arg)
    if(NOT _define STREQUAL "-")
        set(_define_arg /D ${_define})
    endif()
    add_custom_command(
        OUTPUT "${_shader_dir}/${_header}"
        COMMAND "${APP_FXC}" /nologo /T ${_profile} /E ${_entry} ${_define_arg} /O3 /Vn ${_variable}
                /Fh "${_shader_dir}/${_header}" "${CMAKE_CURRENT_SOURCE_DIR}/${_source}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_source}"
        COMMENT "fxc ${_source} ${_entry}"
        VERBATIM)
    list(APPEND _shader_headers "${_shader_dir}/${_header}")
endforeach()
add_custom_target(app_framegen_shaders DEPENDS ${_shader_headers})

# What the add-on and the evaluator share: fg_interp.h and the shaders.
add_library(app_framegen_interp INTERFACE)
target_include_directories(app_framegen_interp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}" "${_shader_dir}")
# AMD's FidelityFX API headers, for fg_fsr.h (third_party/README.md).
target_include_directories(app_framegen_interp SYSTEM INTERFACE
    "${PROJECT_SOURCE_DIR}/third_party/fidelityfx/Kits/FidelityFX")
add_dependencies(app_framegen_interp app_framegen_shaders)

add_library(app_framegen SHARED framegen_addon.cpp reshade_ui.cpp)
set_target_properties(app_framegen PROPERTIES OUTPUT_NAME "zz-framegen" SUFFIX ".addon64")
app_component(app_framegen)
target_include_directories(app_framegen PRIVATE "${PROJECT_SOURCE_DIR}/src/common")
target_link_libraries(app_framegen PRIVATE app_framegen_interp app_reshade_api app_overlay
    d3d11 dxgi dxguid advapi32 user32 ole32 windowscodecs)
install(TARGETS app_framegen RUNTIME DESTINATION engine)

# Offline evaluator (scores generated frames against true ones) and the
# steady-rate frame feeder for pipeline tests. Developer tools, not shipped.
add_executable(fg_eval eval/fg_eval.cpp)
app_component(fg_eval)
target_include_directories(fg_eval PRIVATE "${PROJECT_SOURCE_DIR}/src/common")  # json.h: --hud's boxes
target_link_libraries(fg_eval PRIVATE app_framegen_interp app_common d3d11 dxguid advapi32 windowscodecs ole32)
target_compile_definitions(fg_eval PRIVATE _CRT_SECURE_NO_WARNINGS)  # getenv for its test switches

add_executable(raw_pace eval/raw_pace.cpp)
app_component(raw_pace)
