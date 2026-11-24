# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.25)

# The product name and version live here and nowhere else; code gets them
# from the generated app_version.h (APP_NAME and what is made from it).
project(Lerad
    VERSION 0.2.0
    DESCRIPTION "Modern PC graphics for game consoles through a capture card"
    HOMEPAGE_URL "https://github.com/BennettStaley/Lerad"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "${PROJECT_NAME} builds on Windows only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "${PROJECT_NAME} builds for 64-bit Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT: the app folder must run without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(cmake/Common.cmake)

# Version header (name, version, git commit) for every component. The name
# in small letters is what files and pipes are called (APP_ID); in capitals
# with an underscore it starts the environment variables (APP_ENV_PREFIX).
app_git_describe(APP_GIT_DESCRIBE)
# The project this one is based on, for the credit in the launcher's About
# tab (APP_BASED_ON, APP_BASED_ON_URL): named here, not in the code.
set(APP_BASED_ON "Glasir")
set(APP_BASED_ON_URL "https://github.com/Abhito/Glasir")
string(TOLOWER "${PROJECT_NAME}" APP_ID)
string(TOUPPER "${PROJECT_NAME}_" APP_ENV_PREFIX)
configure_file(cmake/app_version.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/app_version.h" @ONLY)
add_library(app_version INTERFACE)
target_include_directories(app_version INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Vendored third-party headers (see third_party/README.md).
add_library(app_reshade_api INTERFACE)
target_include_directories(app_reshade_api SYSTEM INTERFACE third_party/reshade/include)
add_library(app_mpv_client INTERFACE)
target_include_directories(app_mpv_client SYSTEM INTERFACE third_party/mpv/include)

# fxc from the Windows SDK the build uses (shaders of the add-ons).
set(_sdk_bin "C:/Program Files (x86)/Windows Kits/10/bin")
file(GLOB _sdk_versions LIST_DIRECTORIES true "${_sdk_bin}/10.*")
list(SORT _sdk_versions ORDER DESCENDING)
set(_fxc_hints "${_sdk_bin}/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
foreach(_v IN LISTS _sdk_versions)
    list(APPEND _fxc_hints "${_v}/x64")
endforeach()
find_program(APP_FXC fxc HINTS ${_fxc_hints} REQUIRED)

# Dear ImGui (third_party/imgui, MIT): the in-game menu and the launcher.
# Built as it comes (its own warning level).
add_library(app_imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp)
target_include_directories(app_imgui SYSTEM PUBLIC third_party/imgui third_party/imgui/backends)
target_compile_definitions(app_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS UNICODE)
target_link_libraries(app_imgui PUBLIC d3d11 d3dcompiler dwmapi)

# Code shared by the exe and the add-ons (src/common).
add_library(app_common STATIC src/common/ini_edit.cpp src/common/json.cpp)
app_component(app_common)
target_include_directories(app_common PUBLIC src/common)

# Components are added as they are ported (docs/PLAN.md, "Porting order").
foreach(component IN ITEMS capture app audio overlay framegen)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/${component}/CMakeLists.txt")
        add_subdirectory(src/${component})
    endif()
endforeach()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# The portable app folder: `cmake --install` puts everything here.
install(FILES LICENSE THIRD-PARTY-NOTICES.md DESTINATION .)
install(DIRECTORY LICENSES DESTINATION .)
install(FILES docs/README-release.txt DESTINATION . RENAME README.txt)

# The release zip (`build.cmd package`): the same folder, our files only.
# The app downloads mpv, ReShade and DLSS 5 on the user's PC.
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/out")
include(CPack)
