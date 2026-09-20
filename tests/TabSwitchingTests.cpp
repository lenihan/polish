#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "tabs/TabSwitching.h"
#include "util/SwitcherCycle.h"

TEST_CASE("FindTabRule: matches an allowlisted app by executable name") {
    const auto rule = polish::FindTabRule(L"C:\\Users\\me\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe");
    REQUIRE(rule.has_value());
    CHECK(rule->containerClassNames == std::vector<std::wstring>{L"tabs-container"});
}

TEST_CASE("FindTabRule: the match ignores case, as Windows paths do") {
    CHECK(polish::FindTabRule(L"C:\\vscode\\CODE.EXE").has_value());
    CHECK(polish::FindTabRule(L"c:\\vscode\\code.exe").has_value());
}

TEST_CASE("FindTabRule: the match ignores the directory, so a portable install still works") {
    CHECK(polish::FindTabRule(L"D:\\portable\\Code.exe").has_value());
    CHECK(polish::FindTabRule(L"Code.exe").has_value());
}

TEST_CASE("FindTabRule: fails closed for an app that isn't allowlisted") {
    // The central safety property: an unlisted app gets no tab switching
    // rather than a generic sweep. Outlook and OneNote were both confirmed
    // live to expose their *ribbon* tabs (Home/Insert/View) and nothing
    // else, so a generic sweep would offer those as switch targets. See
    // TabRules().
    CHECK_FALSE(polish::FindTabRule(L"C:\\Program Files\\Microsoft Office\\root\\Office16\\olk.exe").has_value());
    CHECK_FALSE(polish::FindTabRule(L"C:\\Program Files\\Microsoft Office\\root\\Office16\\ONENOTE.EXE").has_value());
    CHECK_FALSE(polish::FindTabRule(L"C:\\Windows\\System32\\mspaint.exe").has_value());
}

TEST_CASE("FindTabRule: an unknown executable path matches nothing") {
    // GetWindowProcessImagePath returns nullopt for an elevated app, which
    // reaches here as an empty string -- it must not match a rule.
    CHECK_FALSE(polish::FindTabRule(L"").has_value());
}

TEST_CASE("FindTabRule: a name that merely contains an allowlisted one is not a match") {
    CHECK_FALSE(polish::FindTabRule(L"C:\\x\\NotCode.exe").has_value());
    CHECK_FALSE(polish::FindTabRule(L"C:\\x\\Code.exe.bak").has_value());
}

TEST_CASE("FindTabRule: the XAML apps share one container class") {
    // Explorer, Terminal and Notepad are all XAML and expose the same
    // TabView container -- the tabs sit below it rather than directly
    // under it, which is why tab items are gathered from the whole
    // subtree rather than from the container's direct children.
    for (const wchar_t* path : {L"C:\\Windows\\explorer.exe",
                                L"C:\\Program Files\\WindowsApps\\x\\WindowsTerminal.exe",
                                L"C:\\Program Files\\WindowsApps\\y\\Notepad.exe"}) {
        const auto rule = polish::FindTabRule(path);
        REQUIRE(rule.has_value());
        CHECK(rule->containerClassNames == std::vector<std::wstring>{L"Microsoft.UI.Xaml.Controls.TabView"});
    }
}

TEST_CASE("FindTabRule: Edge lists both of its nested container classes") {
    // Edge reports the same tabs through two nested containers, so the
    // worker de-duplicates by runtime id. Matching both is deliberate --
    // see TabRule::containerClassNames.
    const auto rule =
        polish::FindTabRule(L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe");
    REQUIRE(rule.has_value());
    CHECK(rule->containerClassNames.size() == 2);
}

TEST_CASE("TabRules: every rule is usable -- no blank fields") {
    for (const polish::TabRule& rule : polish::TabRules()) {
        CHECK_FALSE(rule.executableName.empty());
        CHECK_FALSE(rule.containerClassNames.empty());
        CHECK_FALSE(rule.displayName.empty());
        for (const std::wstring& className : rule.containerClassNames) {
            CHECK_FALSE(className.empty());
        }
    }
}

TEST_CASE("AdvanceHighlight: wraps forward and backward") {
    CHECK(polish::AdvanceHighlight(0, 3, false) == 1);
    CHECK(polish::AdvanceHighlight(2, 3, false) == 0);
    CHECK(polish::AdvanceHighlight(0, 3, true) == 2);
    CHECK(polish::AdvanceHighlight(2, 3, true) == 1);
}

TEST_CASE("AdvanceHighlight: index 1 of an MRU-ordered list is the previous tab") {
    // How a single Alt+` tap toggles: index 0 is the current tab, so one
    // step forward lands on the one used before it. Two tabs means the
    // step is its own inverse, which is what makes the toggle a toggle.
    CHECK(polish::AdvanceHighlight(0, 2, false) == 1);
    CHECK(polish::AdvanceHighlight(1, 2, false) == 0);
}

TEST_CASE("AdvanceHighlight: a single-entry list stays on its only entry") {
    CHECK(polish::AdvanceHighlight(0, 1, false) == 0);
    CHECK(polish::AdvanceHighlight(0, 1, true) == 0);
}

TEST_CASE("AdvanceHighlight: an empty list returns 0 rather than dividing by zero") {
    CHECK(polish::AdvanceHighlight(0, 0, false) == 0);
    CHECK(polish::AdvanceHighlight(0, 0, true) == 0);
}
