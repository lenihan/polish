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

// Tab: one member shown at a time, switched via the chrome's tab strip.
// Tile: every member shown at once in a grid, sized to roughly fill a
// square (biased wide/tall by GroupAlignment below).
// Stack: every member shown at once like Tile, but forced to a single
// row or column instead of a grid -- see GroupAlignment for which.
enum class GroupMode { Tab, Tile, Stack };

// Tile and Stack are both grids of simultaneously-visible members --
// Stack is just Tile with the grid forced to a single row/column --
// so anything that means "no tab strip, has a tile grid" (splitters,
// tile fractions, tile-maximize) applies to both, and Tab is the only
// mode that's actually different.
constexpr bool IsTiledMode(GroupMode mode) { return mode == GroupMode::Tile || mode == GroupMode::Stack; }

// Horizontal (default): tabs across the top; Tile's grid biased wide;
// Stack forced to a single row. Vertical: tabs down the left edge;
// Tile's grid biased tall; Stack forced to a single column. Affects
// both GroupChromeWindow's tab-strip placement and
// GroupManager::ApplyTileLayout's/ComputeGridShape's grid shape --
// kept as one setting rather than two so they can never disagree with
// each other.
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

    // Changes the group's display mode at runtime (Tab/Tile/Stack,
    // user-triggered from the title bar's mode button or the chrome's
    // context menu). Pure bookkeeping -- membership/active-index are
    // untouched; the caller is responsible for re-applying layout
    // (GroupManager::ApplyLayout) and updating the chrome's own
    // rendering afterward. Leaving both tiled modes (see IsTiledMode)
    // also clears tileMaximized_ -- "maximized" only means anything
    // relative to the tile grid it was maximized out of; it wouldn't
    // ever get shown in Tab mode, but silently carrying it forward
    // would ambush the next tiled-mode switch with a maximize the user
    // never (re-)asked for this time. Switching between Tile and Stack
    // keeps it, same as it already keeps tile column/row fractions
    // whenever the grid's shape happens not to change.
    void SetMode(GroupMode mode) {
        mode_ = mode;
        if (!IsTiledMode(mode_)) {
            tileMaximized_ = false;
        }
    }

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

    // Replaces the entire membership list in one call: drops members no
    // longer present, appends new ones, and reorders survivors to match
    // `windows`'s order -- used by the management dialog's confirmed
    // Group-list order (add/remove/drag-reorder all collapse into one
    // call here rather than being diffed against the old membership).
    // The previously active member stays active if it's still present
    // (by identity, regardless of its new index); otherwise the first
    // member in `windows` becomes active, same as AddWindow's own
    // first-member rule. Safe for an empty `windows` (no active member
    // afterward).
    void SetMembers(const std::vector<HWND>& windows);

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

    // Tile/Stack only: whether the active member is currently expanded
    // to fill the whole content area instead of sharing the grid with
    // every other member (a per-tile analog of the *window's* own
    // maximize/restore, scoped to just one tile). Same pure-bookkeeping
    // contract as SetMode -- the caller re-applies layout and updates
    // the chrome's own rendering afterward. Defaults false; also reset
    // to false whenever membership drops to <=1 (see Remove/SetMembers)
    // since a single tile already fills the whole area on its own --
    // "maximized" wouldn't mean anything different from the normal
    // state, and silently carrying a stale true through to whenever a
    // second member gets added again would ambush the user with an
    // unexpected maximized tile they never asked for this time.
    bool IsTileMaximized() const { return tileMaximized_; }
    void SetTileMaximized(bool maximized) { tileMaximized_ = maximized; }

    // Tile/Stack only: user-adjustable column widths / row heights, each
    // a fraction of the content area's total width/height (a vector
    // sums to 1.0). Empty means "not yet customized" --
    // GroupManager::ApplyTileLayout falls back to an equal split, and
    // is also what resets these back to empty whenever the grid's
    // column/row *count* changes (a member added/removed reshapes the
    // grid, so fractions sized for the old shape don't carry over).
    const std::vector<double>& TileColumnFractions() const { return tileColumnFractions_; }
    const std::vector<double>& TileRowFractions() const { return tileRowFractions_; }
    void SetTileColumnFractions(std::vector<double> fractions) { tileColumnFractions_ = std::move(fractions); }
    void SetTileRowFractions(std::vector<double> fractions) { tileRowFractions_ = std::move(fractions); }

private:
    GroupId id_;
    GroupMode mode_;
    GroupAlignment alignment_ = GroupAlignment::Horizontal;
    std::wstring name_;
    std::vector<GroupMember> members_;
    std::optional<size_t> activeIndex_;
    std::vector<double> tileColumnFractions_;
    std::vector<double> tileRowFractions_;
    bool tileMaximized_ = false;
};

}  // namespace polish
