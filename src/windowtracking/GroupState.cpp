#include "windowtracking/GroupState.h"

#include <algorithm>

namespace polish {

namespace {
bool IsWindowMember(const GroupMember& member, HWND hwnd) {
    return member.kind == GroupMemberKind::Window && member.window == hwnd;
}
}  // namespace

GroupState::GroupState(GroupId id, GroupMode mode) : id_(id), mode_(mode) {}

void GroupState::AddWindow(HWND hwnd) {
    if (Contains(hwnd)) {
        return;
    }
    members_.push_back(GroupMember{GroupMemberKind::Window, hwnd, 0});
    if (members_.size() == 1) {
        activeIndex_ = 0;
    }
}

void GroupState::Remove(HWND hwnd) {
    const auto it =
        std::find_if(members_.begin(), members_.end(), [hwnd](const GroupMember& m) { return IsWindowMember(m, hwnd); });
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

void GroupState::Reorder(size_t fromIndex, size_t toIndex) {
    if (fromIndex == toIndex || fromIndex >= members_.size() || toIndex >= members_.size()) {
        return;
    }

    // Captured by identity (not index) so it can be re-derived correctly
    // below regardless of which direction the move shifts everything.
    std::optional<GroupMember> activeMember;
    if (activeIndex_.has_value()) {
        activeMember = members_[*activeIndex_];
    }

    const GroupMember moved = members_[fromIndex];
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

bool GroupState::Contains(HWND hwnd) const {
    return std::any_of(members_.begin(), members_.end(), [hwnd](const GroupMember& m) { return IsWindowMember(m, hwnd); });
}

std::optional<HWND> GroupState::ActiveWindow() const {
    if (!activeIndex_.has_value()) {
        return std::nullopt;
    }
    const GroupMember& active = members_[*activeIndex_];
    if (active.kind == GroupMemberKind::Window) {
        return active.window;
    }
    return std::nullopt;  // nested-group case -- v1 never populates this
}

void GroupState::SetActiveIndex(size_t index) {
    if (index < members_.size()) {
        activeIndex_ = index;
    }
}

void GroupState::SetActiveWindow(HWND hwnd) {
    for (size_t i = 0; i < members_.size(); ++i) {
        if (IsWindowMember(members_[i], hwnd)) {
            activeIndex_ = i;
            return;
        }
    }
}

}  // namespace polish
