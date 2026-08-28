cmake_minimum_required(VERSION 3.28)

project(polish LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Polish targets Windows 11 and only builds on Windows.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(polish WIN32
    src/main.cpp
    resources/app.rc
)

target_compile_definitions(polish PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(polish PRIVATE
    user32
    gdi32
    gdiplus
    dwmapi
    shell32
    comctl32
)

if(MSVC)
    target_compile_options(polish PRIVATE /W4 /permissive-)
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
