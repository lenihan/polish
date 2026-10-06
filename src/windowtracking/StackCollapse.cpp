#include "windowtracking/StackCollapse.h"

#include <algorithm>

namespace polish {

int StackIndexOf(const std::vector<StackEntry>& stacks, HWND hwnd) {
    for (size_t i = 0; i < stacks.size(); ++i) {
        const auto& members = stacks[i].members;
        if (std::find(members.begin(), members.end(), hwnd) != members.end()) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<HWND> CollapseStackMembers(const std::vector<HWND>& ordered, const std::vector<StackEntry>& stacks) {
    if (stacks.empty()) {
        return ordered;
    }
    std::vector<HWND> result;
    result.reserve(ordered.size());
    std::vector<bool> emitted(stacks.size(), false);
    for (HWND hwnd : ordered) {
        const int index = StackIndexOf(stacks, hwnd);
        if (index < 0) {
            result.push_back(hwnd);
            continue;
        }
        if (emitted[static_cast<size_t>(index)]) {
            continue;  // already stood in for by this stack's one entry
        }
        emitted[static_cast<size_t>(index)] = true;
        // First time this stack turns up in the list: this is its earliest
        // member, which is where its entry goes. The representative stands in
        // if it is in the list; otherwise this member does.
        const HWND representative = stacks[static_cast<size_t>(index)].representative;
        const bool representativePresent =
            representative != nullptr && std::find(ordered.begin(), ordered.end(), representative) != ordered.end();
        result.push_back(representativePresent ? representative : hwnd);
    }
    return result;
}

}  // namespace polish
