#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace polish {

// A group's own opaque identifier -- distinct from HWND, since a
// group's chrome window IS a real HWND but a nested-group member
// (added in a later milestone) is not.
using GroupId = uint64_t;

// Which kind a member entry is. v1 only ever populates Window -- the
// NestedGroup case exists so a later milestone (groups nested inside
// groups) doesn't require reshaping this type, without implementing
// the recursive layout logic it would need yet.
enum class GroupMemberKind { Window, NestedGroup };

struct GroupMember {
    GroupMemberKind kind = GroupMemberKind::Window;
    HWND window = nullptr;       // valid when kind == Window
    GroupId nestedGroup = 0;     // valid when kind == NestedGroup (unused in v1)

    bool operator==(const GroupMember&) const = default;
};

enum class GroupMode { Tab, Tile };

// Horizontal (default): tabs across the top, tile grid biased wide.
// Vertical: tabs down the left edge, tile grid biased tall. Affects
// both GroupChromeWindow's tab-strip placement and
// GroupManager::ApplyTileLayout's grid shape -- kept as one setting
// rather than two so they can never disagree with each other.
enum class GroupAlignment { Horizontal, Vertical };

// Pure state for one group: its ordered membership, which member is
// "active" (tab mode: the one currently shown/promoted; tile mode:
// still tracked, e.g. for keyboard focus, even though every member is
// visible at once), and its display mode. No Win32 dependency beyond
// HWND itself, so this is directly unit-testable -- same pattern as
// ActivationHistory (and the deleted RectHistory before it).
class GroupState {
public:
    explicit GroupState(GroupId id, GroupMode mode = GroupMode::Tab);

    GroupId Id() const { return id_; }
    GroupMode Mode() const { return mode_; }
    GroupAlignment Alignment() const { return alignment_; }
    const std::wstring& Name() const { return name_; }

    // Changes the group's display mode at runtime (v1: Tab <-> Tile,
    // user-triggered from the chrome's context menu). Pure bookkeeping
    // -- membership/active-index are untouched; the caller is
    // responsible for re-applying layout (GroupManager::ApplyLayout)
    // and updating the chrome's own rendering afterward.
    void SetMode(GroupMode mode) { mode_ = mode; }

    // Changes tab-strip/tile-grid orientation. Same pure-bookkeeping
    // contract as SetMode -- the caller re-applies layout and updates
    // the chrome's rendering afterward.
    void SetAlignment(GroupAlignment alignment) { alignment_ = alignment; }

    // User-facing name (e.g. "projA"), shown in the chrome's title bar.
    // Defaults to "Group <id>" at construction so it's never empty
    // before the user renames it via the management dialog.
    void SetName(std::wstring name) { name_ = std::move(name); }

    // Appends hwnd as a new member. No-op if hwnd is already a member.
    // The newly added member becomes active if it's the first one added.
    void AddWindow(HWND hwnd);

    // Removes hwnd if present. If it was the active member, the member
    // that shifted into its slot becomes active (or, if it was last,
    // the new last member); the group has no active member if this was
    // its only one.
    void Remove(HWND hwnd);

    // Moves the member currently at fromIndex to toIndex, shifting
    // everything between them over by one (erase+insert semantics, not
    // a swap) -- used for drag-to-reorder in the chrome's tab strip.
    // activeIndex_ is re-derived by identity afterward, so it keeps
    // pointing at the same logical member regardless of which direction
    // things shifted. No-op if fromIndex == toIndex or either is out of
    // range.
    void Reorder(size_t fromIndex, size_t toIndex);

    bool Contains(HWND hwnd) const;
    size_t MemberCount() const { return members_.size(); }
    const std::vector<GroupMember>& Members() const { return members_; }

    // std::nullopt if the group currently has no members.
    std::optional<size_t> ActiveIndex() const { return activeIndex_; }
    std::optional<HWND> ActiveWindow() const;

    // Sets the active index directly (e.g. a tab was clicked). No-op if
    // index is out of range.
    void SetActiveIndex(size_t index);

    // Sets the active member by HWND -- used when a member becomes
    // foreground via some path other than the group's own tab strip
    // (e.g. Alt+Tab). No-op if hwnd isn't a member.
    void SetActiveWindow(HWND hwnd);

private:
    GroupId id_;
    GroupMode mode_;
    GroupAlignment alignment_ = GroupAlignment::Horizontal;
    std::wstring name_;
    std::vector<GroupMember> members_;
    std::optional<size_t> activeIndex_;
};

}  // namespace polish
