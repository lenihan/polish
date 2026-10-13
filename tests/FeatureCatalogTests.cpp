#include "settings/FeatureCatalog.h"

#include <doctest/doctest.h>

#include <set>
#include <string>

using namespace polish;

TEST_CASE("FeatureCatalog: every Feature enumerator has exactly one entry") {
    // The last enumerator; adding one to the enum without a table row makes
    // the count below disagree.
    constexpr int kFeatureCount = static_cast<int>(Feature::PolishButton) + 1;
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
