#include "windowtracking/TaskbarButtons.h"

#include <doctest/doctest.h>

#include "util/AppResolver.h"

using namespace polish;

namespace {
// Rects here use the real geometry measured off a live Win11 26200 taskbar:
// 88x96 buttons on an 88px pitch, starting at x=1177, y=1824.
TaskbarButton Button(const wchar_t* appId, int x, HMONITOR taskbar = nullptr) {
    TaskbarButton b;
    b.appId = appId;
    b.rect = RECT{x, 1824, x + 88, 1920};
    b.taskbar = taskbar;
    return b;
}
HMONITOR AsMonitor(uintptr_t value) { return reinterpret_cast<HMONITOR>(value); }
}  // namespace

TEST_CASE("HitTestTaskbarButton: an empty list matches nothing") {
    CHECK_FALSE(HitTestTaskbarButton({}, POINT{1200, 1870}).has_value());
}

TEST_CASE("HitTestTaskbarButton: a point inside a button matches it") {
    const std::vector<TaskbarButton> buttons{Button(L"Microsoft.VisualStudioCode", 1265)};
    const auto hit = HitTestTaskbarButton(buttons, POINT{1309, 1872});
    REQUIRE(hit.has_value());
    CHECK(hit->appId == L"Microsoft.VisualStudioCode");
}

TEST_CASE("HitTestTaskbarButton: a point above the taskbar matches nothing") {
    const std::vector<TaskbarButton> buttons{Button(L"MSEdge", 1265)};
    CHECK_FALSE(HitTestTaskbarButton(buttons, POINT{1309, 1700}).has_value());
}

TEST_CASE("HitTestTaskbarButton: picks the right button among several") {
    const std::vector<TaskbarButton> buttons{
        Button(L"Outlook", 1177), Button(L"Microsoft.VisualStudioCode", 1265), Button(L"MSEdge", 1353)};
    CHECK(HitTestTaskbarButton(buttons, POINT{1180, 1830})->appId == L"Outlook");
    CHECK(HitTestTaskbarButton(buttons, POINT{1300, 1870})->appId == L"Microsoft.VisualStudioCode");
    CHECK(HitTestTaskbarButton(buttons, POINT{1440, 1919})->appId == L"MSEdge");
}

TEST_CASE("HitTestTaskbarButton: a shared edge belongs to exactly one button") {
    // Adjacent buttons touch: 1177+88 == 1265. PtInRect is right-exclusive,
    // so x=1265 must be the second button's, never both.
    const std::vector<TaskbarButton> buttons{Button(L"Outlook", 1177), Button(L"Code", 1265)};
    const auto hit = HitTestTaskbarButton(buttons, POINT{1265, 1870});
    REQUIRE(hit.has_value());
    CHECK(hit->appId == L"Code");
}

TEST_CASE("HitTestTaskbarButton: the bottom edge of the screen is still inside the button") {
    const std::vector<TaskbarButton> buttons{Button(L"Code", 1265)};
    // y=1920 is exclusive, y=1919 is the last row of real pixels.
    CHECK(HitTestTaskbarButton(buttons, POINT{1300, 1919}).has_value());
    CHECK_FALSE(HitTestTaskbarButton(buttons, POINT{1300, 1920}).has_value());
}

TEST_CASE("TaskbarButtonsEqual: identical snapshots compare equal") {
    const std::vector<TaskbarButton> a{Button(L"Outlook", 1177), Button(L"Code", 1265)};
    const std::vector<TaskbarButton> b{Button(L"Outlook", 1177), Button(L"Code", 1265)};
    CHECK(TaskbarButtonsEqual(a, b));
}

TEST_CASE("TaskbarButtonsEqual: a moved button is a change") {
    // The common real case: an app closes and everything shifts left.
    const std::vector<TaskbarButton> a{Button(L"Code", 1265)};
    const std::vector<TaskbarButton> b{Button(L"Code", 1177)};
    CHECK_FALSE(TaskbarButtonsEqual(a, b));
}

TEST_CASE("TaskbarButtonsEqual: a different count is a change") {
    const std::vector<TaskbarButton> a{Button(L"Outlook", 1177)};
    const std::vector<TaskbarButton> b{Button(L"Outlook", 1177), Button(L"Code", 1265)};
    CHECK_FALSE(TaskbarButtonsEqual(a, b));
}

TEST_CASE("TaskbarButtonsEqual: the same app on two taskbars is not the same button") {
    // AutomationId repeats across monitors, so appId alone cannot identify a
    // button -- the taskbar it belongs to is part of its identity.
    const std::vector<TaskbarButton> a{Button(L"Code", 1265, AsMonitor(1))};
    const std::vector<TaskbarButton> b{Button(L"Code", 1265, AsMonitor(2))};
    CHECK_FALSE(TaskbarButtonsEqual(a, b));
}

TEST_CASE("TaskbarButtonsEqual: a window opening within one app is a change") {
    // The strip does not move when an app's second window opens -- only
    // the button's name does, since the running-window count lives in it.
    // Comparing names is what makes that count as a change, which it must:
    // the button now stands for a different set of windows.
    TaskbarButton one = Button(L"Code", 1265);
    one.name = L"Visual Studio Code";
    TaskbarButton two = Button(L"Code", 1265);
    two.name = L"Visual Studio Code - 2 running windows";
    CHECK_FALSE(TaskbarButtonsEqual({one}, {two}));
    CHECK(TaskbarButtonsEqual({one}, {one}));
}

TEST_CASE("StripAppIdPrefix: removes the taskbar's Appid prefix") {
    CHECK(StripAppIdPrefix(L"Appid: Microsoft.VisualStudioCode") == L"Microsoft.VisualStudioCode");
    CHECK(StripAppIdPrefix(L"Appid: Claude_pzs8sxrjxfjjc!Claude") == L"Claude_pzs8sxrjxfjjc!Claude");
}

TEST_CASE("StripAppIdPrefix: leaves an unprefixed id alone") {
    // If a future build stops prefixing, degrade to still matching rather
    // than to matching nothing.
    CHECK(StripAppIdPrefix(L"Microsoft.VisualStudioCode") == L"Microsoft.VisualStudioCode");
    CHECK(StripAppIdPrefix(L"") == L"");
}

TEST_CASE("StripAppIdPrefix: only strips a real prefix, not a lookalike") {
    CHECK(StripAppIdPrefix(L"Appid:NoSpace") == L"Appid:NoSpace");
    CHECK(StripAppIdPrefix(L"Appid") == L"Appid");
    CHECK(StripAppIdPrefix(L"MyApp.Appid: thing") == L"MyApp.Appid: thing");
}
