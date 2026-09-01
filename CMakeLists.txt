cmake_minimum_required(VERSION 3.28)

project(polish LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Polish targets Windows 11 and only builds on Windows.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Everything except main.cpp lives in this static library, so the same
# code can be linked into both the polish.exe app and the test binary
# (main.cpp's WinMain doesn't belong in a test binary).
add_library(polish_core STATIC
    src/windowtracking/RectUtils.cpp
    src/windowtracking/ActivationHistory.cpp
    src/windowtracking/GroupState.cpp
    src/windowtracking/GroupManager.cpp
    src/windowtracking/WindowFilters.cpp
    src/windowtracking/WindowZOrder.cpp
    src/tray/TrayIcon.cpp
    src/hook/AltTabHook.cpp
    src/hook/AltTabDimOverlay.cpp
    src/hook/AltTabHighlightBorder.cpp
    src/hook/GroupChromeWindow.cpp
    src/hook/GroupPickerWindow.cpp
    src/settings/Settings.cpp
)

target_include_directories(polish_core PUBLIC src resources)

target_compile_definitions(polish_core PUBLIC
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(polish_core PUBLIC
    user32
    dwmapi
    shell32
    comctl32
    gdi32
    gdiplus
)

if(MSVC)
    # /utf-8 matters beyond just warnings: without it, MSVC assumes a
    # source file with no BOM is in the current system codepage, so a
    # non-ASCII character inside a wide string literal (e.g. an em dash)
    # gets silently mis-decoded into garbage at compile time rather than
    # a build error -- confirmed the hard way via a garbled log line
    # (GroupPickerWindow's title) before this flag was added.
    target_compile_options(polish_core PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(polish WIN32
    src/main.cpp
    resources/app.rc
)

target_link_libraries(polish PRIVATE polish_core)

if(MSVC)
    target_compile_options(polish PRIVATE /W4 /permissive- /utf-8)
    # The linker's own auto-generated manifest (resource id 1) collides with
    # the one we embed via resources/app.rc (which carries our Per-Monitor-V2
    # DPI declaration) - disable it so ours is the only one.
    target_link_options(polish PRIVATE /MANIFEST:NO)
endif()

set_target_properties(polish PROPERTIES OUTPUT_NAME "polish")

enable_testing()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
