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
    // Not here, and why:
    //   - File Explorer exposes no TabItem at all despite having visible
    //     tabs, so there is nothing to enumerate.
    //   - Edge with vertical tabs exposed only one of six open tabs.
    //   - Outlook's only tabs are ribbon tabs, which must never be
    //     offered as switch targets.
    static const std::vector<TabRule> rules{
        TabRule{L"Code.exe", L"tabs-container", L"Visual Studio Code"},
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
