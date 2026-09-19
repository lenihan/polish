cmake_minimum_required(VERSION 3.28)

project(polish LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Polish targets Windows 11 and only builds on Windows.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Statically link the MSVC CRT (/MT, /MTd) rather than the default DLL CRT
# (/MD, /MDd) -- polish.exe otherwise depends on MSVCP140.dll/
# VCRUNTIME140.dll at runtime, which aren't part of the OS itself (unlike
# the api-ms-win-crt-*.dll forwarders) and require the separately-
# installed VC++ Redistributable. Confirmed as a real, silent failure
# mode: a bare Windows image without it (a fresh Windows Sandbox instance,
# and by extension any end user who's never installed Visual Studio or a
# redistributable) can't run the dynamically-linked build at all --
# CreateProcess still returns a valid PID (the failure happens inside the
# new process's own loader, moments later), which combined with PID reuse
# can even look like "it started fine" to a script checking Get-Process
# shortly after. Static linking removes the dependency entirely.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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
    src/windowtracking/WindowReparenting.cpp
    src/tray/TrayIcon.cpp
    src/hook/AltTabHook.cpp
    src/hook/AltTabDimOverlay.cpp
    src/hook/AltTabHighlightBorder.cpp
    src/hook/AltTabListWindow.cpp
    src/hook/GroupChromeWindow.cpp
    src/hook/GroupPickerWindow.cpp
    src/hook/GroupPickerListWindow.cpp
    src/hook/GroupPickerSelectedListWindow.cpp
    src/hook/GroupHotkeyDialog.cpp
    src/hook/GroupTabThumbnail.cpp
    src/settings/Settings.cpp
    src/util/AppIdentity.cpp
    src/util/DarkMode.cpp
    src/util/DialogKeyboard.cpp
    src/util/UiFont.cpp
    src/util/WindowIcon.cpp
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
    uxtheme
    shcore
    ole32
    uuid
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
