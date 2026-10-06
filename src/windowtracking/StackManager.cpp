#include "windowtracking/StackManager.h"

#include <algorithm>

#include "windowtracking/WindowPlacement.h"
#include "windowtracking/WindowZOrder.h"

namespace polish {

StackId StackManager::CreateStack(const std::vector<HWND>& windows) {
    const StackId id = nextId_++;
    StackState state(id);
    for (HWND hwnd : windows) {
        state.AddWindow(hwnd);
    }
    stacks_.push_back(std::move(state));
    return id;
}

void StackManager::RemoveStack(StackId id) {
    stacks_.erase(std::remove_if(stacks_.begin(), stacks_.end(), [id](const StackState& s) { return s.Id() == id; }),
                  stacks_.end());
}

StackState* StackManager::FindStack(StackId id) {
    auto it = std::find_if(stacks_.begin(), stacks_.end(), [id](const StackState& s) { return s.Id() == id; });
    return it == stacks_.end() ? nullptr : &*it;
}

const StackState* StackManager::FindStack(StackId id) const {
    auto it = std::find_if(stacks_.begin(), stacks_.end(), [id](const StackState& s) { return s.Id() == id; });
    return it == stacks_.end() ? nullptr : &*it;
}

StackState* StackManager::FindStackContaining(HWND hwnd) {
    auto it = std::find_if(stacks_.begin(), stacks_.end(), [hwnd](const StackState& s) { return s.Contains(hwnd); });
    return it == stacks_.end() ? nullptr : &*it;
}

SIZE StackManager::MinimumContentSize(const StackState& stack) const {
    SIZE needed{0, 0};
    for (const StackMember& member : stack.Members()) {
        if (member.window == nullptr || !IsWindow(member.window) || IsIconic(member.window)) {
            continue;
        }
        const SIZE minimum = MinimumVisibleSizeFor(member.window);
        needed.cx = std::max(needed.cx, minimum.cx);
        needed.cy = std::max(needed.cy, minimum.cy);
    }
    return needed;
}

void StackManager::PlaceMembers(const StackState& stack, const RECT& content) const {
    for (const StackMember& member : stack.Members()) {
        if (member.window == nullptr || !IsWindow(member.window) || IsIconic(member.window)) {
            continue;
        }
        PlaceWindowVisible(member.window, content);
    }
    RaiseActive(stack);
}

void StackManager::RaiseActive(const StackState& stack) const {
    const auto active = stack.ActiveWindow();
    if (active.has_value() && IsWindow(*active) && !IsIconic(*active)) {
        PromoteWindowToFront(*active);
    }
}

}  // namespace polish
