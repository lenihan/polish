#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace polish {

// A stack's own opaque identifier. Distinct from HWND: a stack has no
// window of its own that identifies it -- its tab strip is a separate
// window that comes and goes with it.
using StackId = uint64_t;

// One member of a stack: an ordinary top-level window. (It used to carry a
// kind, with a NestedStack case reserved for stacks inside stacks. That was
// never populated, and the comment promising it was written for a design
// -- members reparented into a container -- that no longer exists.)
struct StackMember {
    HWND window = nullptr;

    bool operator==(const StackMember&) const = default;
};

// There is no display mode any more. A stack used to be able to show its
// members as a tile grid (Tile/Stack) as well as one at a time (Tab), with
// splitters, per-column fractions and a per-tile maximize to go with it.
// All of that is gone: tiling a fixed number of windows is its own feature
// now (windowtracking/WindowLayout.h), and it does the job better by not
// needing the windows to belong to anything first. What is left here is
// the one-member-at-a-time behaviour, which is the part people used.

// Which edge the tab strip sits on: Horizontal (default) puts it across
// the top, Vertical down the left. One setting rather than two so the
// strip and anything positioned relative to it can never disagree.
enum class StackAlignment { Horizontal, Vertical };

// Pure state for one stack: its ordered membership, which member is
// "active" (tab mode: the one currently shown/promoted; tile mode:
// still tracked, e.g. for keyboard focus, even though every member is
// visible at once), and its display mode. No Win32 dependency beyond
// HWND itself, so this is directly unit-testable -- same pattern as
// ActivationHistory (and the deleted RectHistory before it).
class StackState {
public:
    explicit StackState(StackId id);

    StackId Id() const { return id_; }
    StackAlignment Alignment() const { return alignment_; }
    const std::wstring& Name() const { return name_; }

    // Changes which edge the tab strip sits on. Pure bookkeeping --
    // membership and the active index are untouched, and the caller
    // re-applies layout (StackManager::ApplyLayout) and updates the
    // strip's rendering afterward.
    void SetAlignment(StackAlignment alignment) { alignment_ = alignment; }

    // User-facing name (e.g. "projA"), shown in the stack's Alt+Tab entry.
    // Defaults to "Stack <id>" at construction so it's never empty
    // before the user renames it via the management dialog.
    void SetName(std::wstring name) { name_ = std::move(name); }

    // Appends hwnd as a new member. No-op if hwnd is already a member.
    // The newly added member becomes active if it's the first one added.
    void AddWindow(HWND hwnd);

    // Removes hwnd if present. If it was the active member, the member
    // that shifted into its slot becomes active (or, if it was last,
    // the new last member); the stack has no active member if this was
    // its only one.
    void Remove(HWND hwnd);

    // Moves the member currently at fromIndex to toIndex, shifting
    // everything between them over by one (erase+insert semantics, not
    // a swap) -- used for drag-to-reorder in the strip.
    // activeIndex_ is re-derived by identity afterward, so it keeps
    // pointing at the same logical member regardless of which direction
    // things shifted. No-op if fromIndex == toIndex or either is out of
    // range.
    void Reorder(size_t fromIndex, size_t toIndex);

    // Replaces the entire membership list in one call: drops members no
    // longer present, appends new ones, and reorders survivors to match
    // `windows`'s order -- used by the management dialog's confirmed
    // Stack-list order (add/remove/drag-reorder all collapse into one
    // call here rather than being diffed against the old membership).
    // The previously active member stays active if it's still present
    // (by identity, regardless of its new index); otherwise the first
    // member in `windows` becomes active, same as AddWindow's own
    // first-member rule. Safe for an empty `windows` (no active member
    // afterward).
    void SetMembers(const std::vector<HWND>& windows);

    bool Contains(HWND hwnd) const;
    size_t MemberCount() const { return members_.size(); }
    const std::vector<StackMember>& Members() const { return members_; }

    // std::nullopt if the stack currently has no members.
    std::optional<size_t> ActiveIndex() const { return activeIndex_; }
    std::optional<HWND> ActiveWindow() const;

    // Sets the active index directly (e.g. a tab was clicked). No-op if
    // index is out of range.
    void SetActiveIndex(size_t index);

    // Sets the active member by HWND -- used when a member becomes
    // foreground via some path other than the stack's own tab strip
    // (e.g. Alt+Tab). No-op if hwnd isn't a member.
    void SetActiveWindow(HWND hwnd);

    // The one rectangle that defines where this stack is, in screen pixels,
    // including the strip's band (see StackLayout.h for how it divides into
    // strip and content). Empty until the stack is first placed. Lives here
    // rather than in the manager so the arithmetic that moves and resizes
    // it is testable like the rest of the state.
    const RECT& Rect() const { return rect_; }
    void SetRect(const RECT& rect) { rect_ = rect; }
    bool HasRect() const { return rect_.right > rect_.left && rect_.bottom > rect_.top; }
    // Slides the whole stack by (dx, dy): what dragging the strip does.
    void Offset(int dx, int dy) { OffsetRect(&rect_, dx, dy); }

private:
    StackId id_;
    StackAlignment alignment_ = StackAlignment::Horizontal;
    std::wstring name_;
    std::vector<StackMember> members_;
    std::optional<size_t> activeIndex_;
    RECT rect_{};
};

}  // namespace polish
