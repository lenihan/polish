#include "tabs/TabSwitching.h"

#include <algorithm>
#include <cwctype>

namespace polish {
namespace {

// The file name component of a path, or the whole string if it has no
// separator. Handles both separators -- a process image path comes back
// with backslashes, but a hand-written rule or test may not.
std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
               return std::towlower(x) == std::towlower(y);
           });
}

}  // namespace

const std::vector<TabRule>& TabRules() {
    // Probed live on this project's own machine; see docs/LIMITATIONS.md
    // #17 for what each app was found to expose.
    //
    // Explorer, Terminal and Notepad are all XAML apps and share one
    // container class -- the tabs are ListViewItems nested under it, not
    // direct children, which is why tab items are collected from the
    // whole subtree.
    //
    // Not here, and why:
    //   - Outlook and OneNote expose only ribbon tabs (Home/Insert/View),
    //     which must never be offered as switch targets.
    static const std::vector<TabRule> rules{
        TabRule{L"Code.exe", {L"tabs-container"}, L"Visual Studio Code"},
        TabRule{L"explorer.exe", {L"Microsoft.UI.Xaml.Controls.TabView"}, L"File Explorer"},
        TabRule{L"WindowsTerminal.exe", {L"Microsoft.UI.Xaml.Controls.TabView"}, L"Terminal"},
        TabRule{L"Notepad.exe", {L"Microsoft.UI.Xaml.Controls.TabView"}, L"Notepad"},
        // Both classes deliberately: Edge nests one inside the other and
        // reports the same tabs through each. See TabRule's comment, and
        // docs/LIMITATIONS.md #18 for the tabs it does not report at all.
        TabRule{L"msedge.exe", {L"EdgeTabContainerImpl", L"EdgeVerticalTabContainerView"}, L"Microsoft Edge"},
    };
    return rules;
}

std::optional<TabRule> FindTabRule(const std::wstring& executablePath) {
    if (executablePath.empty()) {
        return std::nullopt;
    }
    const std::wstring name = FileNameOf(executablePath);
    for (const TabRule& rule : TabRules()) {
        if (EqualsIgnoreCase(name, rule.executableName)) {
            return rule;
        }
    }
    return std::nullopt;
}

}  // namespace polish
