cmake_minimum_required(VERSION 3.28)

# Stays below 1.0 until the Store submission, which takes 1.0.0 -- so the
# build people can download today is never confused with the one the Store
# listing later claims.
project(polish VERSION 0.3.0 LANGUAGES CXX)

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
    src/windowtracking/MoveSnap.cpp
    src/windowtracking/ArrangeSelection.cpp
    src/windowtracking/StackCollapse.cpp
    src/windowtracking/StackLayout.cpp
    src/windowtracking/StackStripLayout.cpp
    src/windowtracking/WindowLayout.cpp
    src/windowtracking/WindowPlacement.cpp
    src/windowtracking/ActivationHistory.cpp
    src/windowtracking/StackState.cpp
    src/windowtracking/StackManager.cpp
    src/windowtracking/WindowFilters.cpp
    src/windowtracking/WindowZOrder.cpp
    src/windowtracking/TaskbarButtons.cpp
    src/windowtracking/TaskbarReadPolicy.cpp
    src/tray/TrayIcon.cpp
    src/hook/ActiveWindowHalo.cpp
    src/hook/BullseyeOverlay.cpp
    src/hook/AltTabHook.cpp
    src/hook/MoveModeHook.cpp
    src/hook/MoveModeLayoutPreview.cpp
    src/hook/MoveModeZoneOverlay.cpp
    src/hook/AltTabDimOverlay.cpp
    src/hook/AltTabHighlightBorder.cpp
    src/hook/AltTabListWindow.cpp
    src/hook/StackPickerWindow.cpp
    src/hook/StackPickerListWindow.cpp
    src/hook/StackPickerSelectedListWindow.cpp
    src/hook/StackHotkeyDialog.cpp
    src/hook/StackStripWindow.cpp
    src/hook/TaskbarShield.cpp
    src/hook/TaskbarHook.cpp
    src/settings/Settings.cpp
    src/tabs/TabSwitching.cpp
    src/tabs/UiaWorker.cpp
    src/util/AnchorPoint.cpp
    src/util/AppIdentity.cpp
    src/util/AppResolver.cpp
    src/util/SwitcherCycle.cpp
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
    oleaut32  # BSTR/VARIANT helpers used by the UI Automation tab worker
    oleacc    # MSAA, for the text caret the bullseye aims at
    uuid
    version
)

if(MSVC)
    # /utf-8 matters beyond just warnings: without it, MSVC assumes a
    # source file with no BOM is in the current system codepage, so a
    # non-ASCII character inside a wide string literal (e.g. an em dash)
    # gets silently mis-decoded into garbage at compile time rather than
    # a build error -- confirmed the hard way via a garbled log line
    # (StackPickerWindow's title) before this flag was added.
    target_compile_options(polish_core PRIVATE /W4 /permissive- /utf-8)
endif()

# The VERSIONINFO resource, generated so the version lives only in
# project(). Without it Explorer's Details tab is blank and the installer
# has no ProductVersion to read.
configure_file(resources/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)

# The build stamp: commit, dirty flag and build time, so a running binary
# can say which build it is and not merely which version. See
# cmake/BuildStamp.cmake for why it is regenerated before every build
# rather than only at configure time, and why only the executables link
# it.
set(POLISH_BUILDINFO_CPP "${CMAKE_CURRENT_BINARY_DIR}/BuildInfo.cpp")
set(POLISH_BUILDSTAMP_ARGS
    -D "SRC=${CMAKE_CURRENT_SOURCE_DIR}"
    -D "TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/src/util/BuildInfo.cpp.in"
    -D "OUT=${POLISH_BUILDINFO_CPP}"
    -D "POLISH_VERSION=${PROJECT_VERSION}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildStamp.cmake")

# Once now, so the file exists for the generators that want to see it at
# configure time...
execute_process(COMMAND "${CMAKE_COMMAND}" ${POLISH_BUILDSTAMP_ARGS})
# ...and again before each build, which is the part that keeps it honest.
add_custom_target(polish_build_stamp ALL
    COMMAND "${CMAKE_COMMAND}" ${POLISH_BUILDSTAMP_ARGS}
    COMMENT "Stamping build info (commit, tree state, time)"
    VERBATIM)
set_source_files_properties("${POLISH_BUILDINFO_CPP}" PROPERTIES GENERATED TRUE)

add_executable(polish WIN32
    src/main.cpp
    "${POLISH_BUILDINFO_CPP}"
    resources/app.rc
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
)

target_link_libraries(polish PRIVATE polish_core)
add_dependencies(polish polish_build_stamp)

if(MSVC)
    target_compile_options(polish PRIVATE /W4 /permissive- /utf-8)
    # The linker's own auto-generated manifest (resource id 1) collides with
    # the one we embed via resources/app.rc (which carries our Per-Monitor-V2
    # DPI declaration) - disable it so ours is the only one.
    target_link_options(polish PRIVATE /MANIFEST:NO)
endif()

set_target_properties(polish PROPERTIES OUTPUT_NAME "polish")

# Identical sources, manifest asks for uiAccess. Must be signed and installed
# under Program Files to launch: see tools/uiaccess/ and docs/UIACCESS.md.
add_executable(polish_uia WIN32
    src/main.cpp
    "${POLISH_BUILDINFO_CPP}"
    resources/app_uiaccess.rc
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
)
target_link_libraries(polish_uia PRIVATE polish_core)
add_dependencies(polish_uia polish_build_stamp)
if(MSVC)
    target_compile_options(polish_uia PRIVATE /W4 /permissive- /utf-8)
    target_link_options(polish_uia PRIVATE /MANIFEST:NO)
endif()

enable_testing()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
