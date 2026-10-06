include(FetchContent)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
)
FetchContent_MakeAvailable(doctest)

add_executable(polish_tests
    TestMain.cpp
    RectUtilsTests.cpp
    MoveSnapTests.cpp
    ArrangeSelectionTests.cpp
    WindowLayoutTests.cpp
    ActivationHistoryTests.cpp
    TabSwitchingTests.cpp
    StackCollapseTests.cpp
    StackLayoutTests.cpp
    StackStateTests.cpp
    StackStripLayoutTests.cpp
    StackManagerTests.cpp
    HaloFalloffTests.cpp
    BullseyeMathTests.cpp
    TaskbarButtonsTests.cpp
    TaskbarReadPolicyTests.cpp
)

target_link_libraries(polish_tests PRIVATE polish_core doctest::doctest)

if(MSVC)
    target_compile_options(polish_tests PRIVATE /W4 /permissive-)
endif()

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(polish_tests)
