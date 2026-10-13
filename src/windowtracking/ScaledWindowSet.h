#pragma once

#include <windows.h>

#include <optional>
#include <vector>

namespace polish {

// What had to be changed on a foreign window to hide it, so it can be put
// back exactly. Recorded before the change is made; the journal in
// hook/WindowParking persists the same fields.
struct ParkRecord {
    LONG_PTR originalExStyle = 0;
    bool hadLayered = false;  // already WS_EX_LAYERED, with these attributes
    BYTE alpha = 255;
    COLORREF colorKey = 0;
    DWORD layeredFlags = 0;
    bool wasTopmost = false;
    RECT originalRect{};
};

struct ScaledEntry {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    double scale = 1.0;
    RECT viewRect{};
    SIZE realSize{};  // the window's visible size at 1:1
    bool wasMaximized = false;
    bool minimized = false;  // real window minimized: the view is hidden
    ParkRecord park;
};

// Where a window's view was the last time it was restored, so scaling it
// back down lands in the same spot at the same size.
struct RememberedView {
    RECT viewRect{};
    double scale = 1.0;
};

// Which windows are currently scaled, and the bookkeeping around them.
// Like StackManager it holds no Win32 state beyond the handle types: the
// views, the parking and the activation hooks are other code's job, so the
// transitions can be tested without a single window existing.
class ScaledWindowSet {
public:
    // Registers a window as scaled. False if it already is.
    bool Scale(const ScaledEntry& entry);

    // Removes the window from the set and remembers where its view was.
    // Returns the entry as it was, or nullopt if it was not scaled.
    std::optional<ScaledEntry> Unscale(HWND hwnd);

    // The view was moved or resized by the user. The scale is recomputed
    // from the new rect against the window's real size.
    bool SetViewRect(HWND hwnd, const RECT& viewRect);

    // Changes the scale keeping the view's top-left (a preset or a
    // shortcut step, as opposed to a corner drag).
    bool Rescale(HWND hwnd, double scale);

    // The app resized itself while scaled: the view follows at the same
    // scale from the same top-left.
    bool OnRealResized(HWND hwnd, SIZE newRealSize);

    bool SetMinimized(HWND hwnd, bool minimized);

    // The window is gone for good: forget it and its remembered view.
    void OnRealDestroyed(HWND hwnd);

    bool Contains(HWND hwnd) const;
    const ScaledEntry* Find(HWND hwnd) const;

    // The most recently scaled or adjusted window, or null if none.
    HWND MostRecent() const;

    const std::vector<ScaledEntry>& Entries() const { return entries_; }
    bool Empty() const { return entries_.empty(); }

    std::optional<RememberedView> Remembered(HWND hwnd) const;

private:
    ScaledEntry* FindMutable(HWND hwnd);
    // Oldest first; the most recent is last.
    void Touch(HWND hwnd);

    std::vector<ScaledEntry> entries_;
    struct RememberedSlot {
        HWND hwnd;
        RememberedView view;
    };
    std::vector<RememberedSlot> remembered_;
};

}  // namespace polish
