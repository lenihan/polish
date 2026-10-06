#include "windowtracking/StackState.h"

#include <algorithm>
#include <format>

namespace polish {

namespace {
bool IsWindowMember(const StackMember& member, HWND hwnd) {
    return member.window == hwnd;
}
}  // namespace

StackState::StackState(StackId id) : id_(id), name_(std::format(L"Stack {}", id)) {}

void StackState::AddWindow(HWND hwnd) {
    if (Contains(hwnd)) {
        return;
    }
    members_.push_back(StackMember{hwnd});
    if (members_.size() == 1) {
        activeIndex_ = 0;
    }
}

void StackState::Remove(HWND hwnd) {
    const auto it =
        std::find_if(members_.begin(), members_.end(), [hwnd](const StackMember& m) { return IsWindowMember(m, hwnd); });
    if (it == members_.end()) {
        return;
    }
    const size_t removedIndex = static_cast<size_t>(it - members_.begin());
    members_.erase(it);

    if (members_.empty()) {
        activeIndex_.reset();
        return;
    }
    if (!activeIndex_.has_value()) {
        return;  // defensive -- shouldn't happen if members_ was non-empty before this call
    }
    if (removedIndex < *activeIndex_) {
        *activeIndex_ -= 1;
    } else if (removedIndex == *activeIndex_ && *activeIndex_ >= members_.size()) {
        // The active member itself was removed and was last -- the new
        // last member takes over. If it wasn't last, activeIndex_ is
        // left unchanged: the member that shifted into the removed slot
        // is exactly what should become active now.
        activeIndex_ = members_.size() - 1;
    }
}

void StackState::Reorder(size_t fromIndex, size_t toIndex) {
    if (fromIndex == toIndex || fromIndex >= members_.size() || toIndex >= members_.size()) {
        return;
    }

    // Captured by identity (not index) so it can be re-derived correctly
    // below regardless of which direction the move shifts everything.
    std::optional<StackMember> activeMember;
    if (activeIndex_.has_value()) {
        activeMember = members_[*activeIndex_];
    }

    const StackMember moved = members_[fromIndex];
    members_.erase(members_.begin() + static_cast<std::ptrdiff_t>(fromIndex));
    members_.insert(members_.begin() + static_cast<std::ptrdiff_t>(toIndex), moved);

    if (activeMember.has_value()) {
        for (size_t i = 0; i < members_.size(); ++i) {
            if (members_[i] == *activeMember) {
                activeIndex_ = i;
                break;
            }
        }
    }
}

void StackState::SetMembers(const std::vector<HWND>& windows) {
    const std::optional<HWND> activeWindow = ActiveWindow();  // by identity, survives reordering

    std::vector<StackMember> newMembers;
    newMembers.reserve(windows.size());
    for (HWND hwnd : windows) {
        newMembers.push_back(StackMember{hwnd});
    }
    members_ = std::move(newMembers);

    if (members_.empty()) {
        activeIndex_.reset();
        return;
    }
    if (activeWindow.has_value()) {
        for (size_t i = 0; i < members_.size(); ++i) {
            if (IsWindowMember(members_[i], *activeWindow)) {
                activeIndex_ = i;
                return;
            }
        }
    }
    // Previously-active member isn't in the new list (or there wasn't
    // one) -- fall back to the first member, mirroring AddWindow's
    // first-member rule.
    activeIndex_ = 0;
}

bool StackState::Contains(HWND hwnd) const {
    return std::any_of(members_.begin(), members_.end(), [hwnd](const StackMember& m) { return IsWindowMember(m, hwnd); });
}

std::optional<HWND> StackState::ActiveWindow() const {
    if (!activeIndex_.has_value()) {
        return std::nullopt;
    }
return members_[*activeIndex_].window;
}

void StackState::SetActiveIndex(size_t index) {
    if (index < members_.size()) {
        activeIndex_ = index;
    }
}

void StackState::SetActiveWindow(HWND hwnd) {
    for (size_t i = 0; i < members_.size(); ++i) {
        if (IsWindowMember(members_[i], hwnd)) {
            activeIndex_ = i;
            return;
        }
    }
}

}  // namespace polish
