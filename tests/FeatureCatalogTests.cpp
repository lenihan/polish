#include "settings/FeatureCatalog.h"

#include <doctest/doctest.h>

#include <set>
#include <string>

using namespace polish;

TEST_CASE("FeatureCatalog: every Feature enumerator has exactly one entry") {
    // The last enumerator; adding one to the enum without a table row makes
    // the count below disagree.
    constexpr int kFeatureCount = static_cast<int>(Feature::ScaledTopmost) + 1;
    CHECK(static_cast<int>(AllFeatures().size()) == kFeatureCount);

    std::set<int> seen;
    for (const FeatureInfo& info : AllFeatures()) {
        CHECK(seen.insert(static_cast<int>(info.id)).second);  // no duplicates
    }
    for (int i = 0; i < kFeatureCount; ++i) {
        CHECK(seen.count(i) == 1);
    }
}

TEST_CASE("FeatureCatalog: every entry has a name and a real description") {
    for (const FeatureInfo& info : AllFeatures()) {
        CHECK(std::wstring(info.name).size() > 3);
        // A description is for someone who has never seen the feature, so a
        // few words is not enough.
        CHECK(std::wstring(info.description).size() > 40);
        CHECK(info.member != nullptr);
    }
}

TEST_CASE("FeatureCatalog: each feature has its own Settings member") {
    // Two features sharing a switch would toggle together.
    for (const FeatureInfo& a : AllFeatures()) {
        for (const FeatureInfo& b : AllFeatures()) {
            if (a.id != b.id) {
                CHECK(a.member != b.member);
            }
        }
    }
}

TEST_CASE("FeatureCatalog: get and set go through the right member") {
    Settings settings;
    for (const FeatureInfo& info : AllFeatures()) {
        SetFeatureEnabled(settings, info.id, false);
        CHECK_FALSE(IsFeatureEnabled(settings, info.id));
        // Setting one feature leaves every other alone.
        for (const FeatureInfo& other : AllFeatures()) {
            if (other.id != info.id) {
                CHECK(IsFeatureEnabled(settings, other.id) ==
                      IsFeatureEnabled(Settings{}, other.id));
            }
        }
        SetFeatureEnabled(settings, info.id, true);
        CHECK(IsFeatureEnabled(settings, info.id));
    }
}

TEST_CASE("FeatureCatalog: lookups reach the members they name") {
    Settings settings;
    SetFeatureEnabled(settings, Feature::Halo, false);
    CHECK_FALSE(settings.haloEnabled);
    SetFeatureEnabled(settings, Feature::KeepClipboard, false);
    CHECK_FALSE(settings.keepClipboardOnEmptyCopy);
    SetFeatureEnabled(settings, Feature::PolishButton, false);
    CHECK_FALSE(settings.polishButtonEnabled);
    CHECK(settings.altTabEnabled);  // untouched
}

TEST_CASE("HotkeyCatalog: every Hotkey enumerator has exactly one entry") {
    constexpr int kHotkeyCount = static_cast<int>(Hotkey::ScaleReset) + 1;
    CHECK(static_cast<int>(AllHotkeys().size()) == kHotkeyCount);
    std::set<int> seen;
    for (const HotkeyInfo& info : AllHotkeys()) {
        CHECK(seen.insert(static_cast<int>(info.id)).second);
        CHECK(std::wstring(info.name).size() > 3);
    }
    CHECK(static_cast<int>(seen.size()) == kHotkeyCount);
}

TEST_CASE("HotkeyCatalog: no two hotkeys share a member, and the defaults are distinct") {
    const Settings defaults;
    for (const HotkeyInfo& a : AllHotkeys()) {
        for (const HotkeyInfo& b : AllHotkeys()) {
            if (a.id == b.id) {
                continue;
            }
            CHECK(a.modifiers != b.modifiers);
            CHECK(a.virtualKey != b.virtualKey);
            const HotkeyCombination ca = GetHotkey(defaults, a.id);
            const HotkeyCombination cb = GetHotkey(defaults, b.id);
            CHECK_FALSE((ca.modifiers == cb.modifiers && ca.virtualKey == cb.virtualKey));
        }
    }
}

TEST_CASE("HotkeyCatalog: set changes only the one hotkey") {
    Settings settings;
    const Settings before = settings;
    SetHotkey(settings, Hotkey::TileThreeWay, {MOD_WIN | MOD_SHIFT, 'Q'});
    const HotkeyCombination changed = GetHotkey(settings, Hotkey::TileThreeWay);
    CHECK(changed.modifiers == (MOD_WIN | MOD_SHIFT));
    CHECK(changed.virtualKey == 'Q');
    for (const HotkeyInfo& info : AllHotkeys()) {
        if (info.id != Hotkey::TileThreeWay) {
            const HotkeyCombination now = GetHotkey(settings, info.id);
            const HotkeyCombination was = GetHotkey(before, info.id);
            CHECK(now.modifiers == was.modifiers);
            CHECK(now.virtualKey == was.virtualKey);
        }
    }
}

TEST_CASE("NumberCatalog: set clamps to the entry's range") {
    Settings settings;
    SetNumber(settings, Number::ScalePercent, 5);
    CHECK(GetNumber(settings, Number::ScalePercent) == 10);
    SetNumber(settings, Number::ScalePercent, 500);
    CHECK(GetNumber(settings, Number::ScalePercent) == 90);
    SetNumber(settings, Number::ScalePercent, 33);
    CHECK(GetNumber(settings, Number::ScalePercent) == 33);
    // The other number is untouched.
    CHECK(GetNumber(settings, Number::SnapDistance) == GetNumber(Settings{}, Number::SnapDistance));
}

TEST_CASE("NumberCatalog: every default sits inside its own range") {
    const Settings defaults;
    for (const NumberInfo& info : AllNumbers()) {
        CHECK(std::wstring(info.name).size() > 3);
        CHECK(info.min < info.max);
        CHECK(GetNumber(defaults, info.id) >= info.min);
        CHECK(GetNumber(defaults, info.id) <= info.max);
    }
}
