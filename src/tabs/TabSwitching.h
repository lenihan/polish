#pragma once

#include <optional>
#include <string>
#include <vector>

namespace polish {

// Alt+` switches between the *document tabs* of the foreground window --
// editor tabs, not windows (Alt+Tab already covers windows). Tabs are not
// operating-system objects: they exist only inside an app's own UI, and
// the only uniform way to reach them is UI Automation, which exposes them
// differently in every app. See docs/LIMITATIONS.md #17.

// How to find one app's document tabs in its UI Automation tree.
struct TabRule {
    // Executable file name only, no directory, matched case-insensitively.
    std::wstring executableName;
    // ClassNames of the UIA Tab containers that hold *document* tabs. Tab
    // items are then collected from the whole subtree under each match,
    // not just its direct children: in every XAML app probed the tabs sit
    // under an intermediate ListView, and in Edge deeper still.
    //
    // This is the discriminator that keeps non-document tabs out, and it
    // is why the rule is per-app data rather than one generic sweep. VS
    // Code exposes three Tab controls, all with TabItem descendants and
    // all indistinguishable by control type alone: the editor strip
    // ("tabs-container"), the activity bar and the bottom panel (both
    // "actions-container"). Only the first holds anything a user would
    // call a tab. Matching on the container's class rather than its Name
    // also survives localization -- the other two are both named "Active
    // View Switcher" in English only.
    //
    // More than one class per app because Edge nests two tab containers
    // inside each other, each reporting the same tabs; matching both and
    // de-duplicating by runtime id is more robust than guessing which one
    // survives a browser update.
    std::vector<std::wstring> containerClassNames;
    // Shown as the switcher panel's heading while this app's tabs are up.
    std::wstring displayName;
};

// Every app Alt+` knows how to read tabs from.
//
// Deliberately fails closed: an app that is not listed gets no tab
// switching at all. The alternative -- offering every TabItem in the
// tree -- would hand back ribbon tabs in Outlook and OneNote and sidebar
// icons in VS Code as if they were documents, which is worse than doing
// nothing. All three were confirmed live to expose exactly that.
const std::vector<TabRule>& TabRules();

// The rule for the app at `executablePath` (a full path; only its file
// name is compared), or nullopt when the app is not allowlisted. Pure --
// it touches no window and no process, so it is directly unit-testable.
std::optional<TabRule> FindTabRule(const std::wstring& executablePath);

// One switchable tab, as shown in the switcher panel. Deliberately holds
// no UI Automation handle: those live only on the worker thread that
// created them (see UiaWorker), and the rest of the app refers to a
// tab by its index in the enumerated list.
struct TabTarget {
    std::wstring title;
    bool selected = false;  // the tab that is currently frontmost
    // The tab's UIA runtime id -- plain data, safe to carry across
    // threads, and stable for as long as the tab exists. Used to identify
    // the same tab between one enumeration and the next, which is what
    // most-recently-used ordering is built on (see main.cpp's tab MRU),
    // and to de-duplicate tabs reported by two nested containers.
    std::vector<int> runtimeId;
};

// Fewest tabs worth opening a session for. One tab is not a session --
// there is nowhere to switch to, and unlike Alt+Tab there is no native
// behavior to fall back on, so the keystroke simply does nothing.
inline constexpr size_t kMinimumTabs = 2;

}  // namespace polish
