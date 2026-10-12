#pragma once

#include <windows.h>

#include <vector>

namespace polish {

// A stack as the Alt+Tab list sees it: its members, and which of them stands
// in for the whole stack.
struct StackEntry {
    std::vector<HWND> members;
    // The member that represents the stack -- the active tab. Activating the
    // entry activates this window.
    HWND representative = nullptr;
};

// `ordered` is the Alt+Tab list as it would be without stacks (most recently
// used first). Returns it with every stack folded down to a single entry, so
// a stack of five windows is one row, not five.
//
// - Non-members keep their relative order exactly.
// - A stack's one entry sits where its *earliest* member sat in `ordered`,
//   not where its active member sits. Placing it by the active member would
//   make the list reorder itself every time you switched tabs inside the
//   stack, which reads as "the list jumps around".
// - The entry's window is the stack's representative, if that window is in
//   `ordered`; otherwise (it is minimized, say, or on another monitor) the
//   first member that is.
// - A stack with no member in `ordered` contributes nothing.
//
// This is only the Polish switcher. The real taskbar and native Alt+Tab
// still list every member -- Polish does not control those -- so the two
// will disagree, and that is accepted.
std::vector<HWND> CollapseStackMembers(const std::vector<HWND>& ordered, const std::vector<StackEntry>& stacks);

// Which stack (index into `stacks`) hwnd belongs to, or -1.
int StackIndexOf(const std::vector<StackEntry>& stacks, HWND hwnd);

// How the stack picker sections its list of available windows, in the order
// the sections appear.
enum class WindowStateGroup { Maximized = 0, Normal = 1, Minimized = 2 };

// A window that is minimized is Minimized whatever else it is (a minimized
// window that was maximized is still sitting in the taskbar).
WindowStateGroup StateGroupOf(bool minimized, bool maximized);

// The indices of `groups` ordered by group, keeping the original relative
// order within a group (a stable sort), so the picker's list stays in the
// order the user already knows inside each section.
std::vector<size_t> OrderByStateGroup(const std::vector<WindowStateGroup>& groups);

}  // namespace polish
