#include <doctest/doctest.h>

#include "tabs/TabSwitching.h"
#include "util/SwitcherCycle.h"

TEST_CASE("FindTabRule: matches an allowlisted app by executable name") {
    const auto rule = polish::FindTabRule(L"C:\\Users\\me\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe");
    REQUIRE(rule.has_value());
    CHECK(rule->containerClassName == L"tabs-container");
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
    // rather than a generic sweep, which would offer Outlook's ribbon
    // tabs as switch targets. See TabRules().
    CHECK_FALSE(polish::FindTabRule(L"C:\\Program Files\\Microsoft Office\\OUTLOOK.EXE").has_value());
    CHECK_FALSE(polish::FindTabRule(L"C:\\Windows\\explorer.exe").has_value());
    CHECK_FALSE(polish::FindTabRule(L"C:\\Windows\\System32\\notepad.exe").has_value());
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

TEST_CASE("TabRules: every rule is usable -- no blank fields") {
    for (const polish::TabRule& rule : polish::TabRules()) {
        CHECK_FALSE(rule.executableName.empty());
        CHECK_FALSE(rule.containerClassName.empty());
        CHECK_FALSE(rule.displayName.empty());
    }
}

TEST_CASE("AdvanceHighlight: wraps forward and backward") {
    CHECK(polish::AdvanceHighlight(0, 3, false) == 1);
    CHECK(polish::AdvanceHighlight(2, 3, false) == 0);
    CHECK(polish::AdvanceHighlight(0, 3, true) == 2);
    CHECK(polish::AdvanceHighlight(2, 3, true) == 1);
}

TEST_CASE("AdvanceHighlight: a single-entry list stays on its only entry") {
    CHECK(polish::AdvanceHighlight(0, 1, false) == 0);
    CHECK(polish::AdvanceHighlight(0, 1, true) == 0);
}

TEST_CASE("AdvanceHighlight: an empty list returns 0 rather than dividing by zero") {
    CHECK(polish::AdvanceHighlight(0, 0, false) == 0);
    CHECK(polish::AdvanceHighlight(0, 0, true) == 0);
}
