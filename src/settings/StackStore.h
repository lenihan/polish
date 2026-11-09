#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace polish {

// What it takes to find a stack's window again after Polish restarts.
//
// The handle is tried first: when only Polish restarted, every app is still
// running and the handle is still the same window. After the apps restarted
// too (a new session, say) the handle means nothing, so the window is looked
// for by what it is: its program, its window class and, best of all, its
// title.
struct SavedMember {
    unsigned long long hwnd = 0;
    std::wstring exe;
    std::wstring windowClass;
    std::wstring title;
};

struct SavedStack {
    std::wstring name;
    int alignment = 0;  // StackAlignment
    RECT rect{};
    unsigned long long activeHwnd = 0;
    std::vector<SavedMember> members;
};

// Stacks live in one string value, HKCU\Software\Polish\Stacks. Whole-list
// replace, so a half-written list is not a state that can exist.
void SaveStacks(const std::vector<SavedStack>& stacks);
std::vector<SavedStack> LoadStacks();

SavedMember DescribeWindow(HWND hwnd);

// The live window a saved member refers to, or nullptr. `claimed` are windows
// already taken by another member, which are never returned twice.
HWND ResolveSavedMember(const SavedMember& member, const std::vector<HWND>& claimed);

}  // namespace polish
