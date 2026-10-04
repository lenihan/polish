#pragma once

#include <windows.h>

#include "windowtracking/MoveSnap.h"

namespace polish {

// The move/resize mode's zone map: drawn on top of whichever window the
// pointer is over, it shows the inch-wide resize band around the edge,
// the move area inside it, and which of the nine zones the pointer is
// actually in -- so what a click is about to do is visible before the
// click happens.
//
// It exists because the zone model is invisible otherwise. A band you
// cannot see is a band you have to find by trial, and the whole point of
// making it an inch wide was that nothing should need aiming at. Once a
// drag starts the map gets out of the way and only the edges actually
// moving stay lit, which is the confirmation half: move and resize look
// different while they are happening, not just before.
//
// A fourth overlay class rather than a generalization of the three that
// already exist, for the reasons ActiveWindowHalo.h and
// BullseyeOverlay.h both set out at length. This one is unlike all of
// them again: it draws *inside* its target's rect (not outside like the
// halo, not over the whole target like the dim), its content changes on
// pointer movement rather than on target size, and it is a flat
// composition of rectangles with no gradient or curve anywhere -- which
// is why it rasterizes with plain fills into a DIB rather than reaching
// for GDI+ like AltTabHighlightBorder does.
//
// Topmost, unlike AltTabDimOverlay: it has to sit above its target to be
// seen at all, and during move mode nothing else on screen should be
// competing for that space anyway. Click-through (WS_EX_TRANSPARENT), so
// the low-level hook keeps seeing real coordinates and WindowFromPoint
// keeps reaching real windows underneath -- the same property the dim
// overlays depend on.
class MoveModeZoneOverlay {
public:
    explicit MoveModeZoneOverlay(HINSTANCE instance);
    ~MoveModeZoneOverlay();

    MoveModeZoneOverlay(const MoveModeZoneOverlay&) = delete;
    MoveModeZoneOverlay& operator=(const MoveModeZoneOverlay&) = delete;

    // The zone map over a window's visible rect.
    //
    // `hovered` is the zone the pointer is in, drawn filled. `dragging`
    // switches from the map (every boundary drawn, so the layout can be
    // read) to the committed look (map gone, only the edges this grip
    // actually moves drawn thick).
    //
    // Repeated calls with identical arguments are cheap: the content is
    // only re-rasterized when something it depends on changed, so this
    // can be called on every pointer move during a drag.
    void ShowZones(const RECT& visible, int borderPx, Grip hovered, bool dragging, COLORREF accent, UINT dpi);

    void Hide();

private:
    void EnsureCapacity(int width, int height);
    void Present(const RECT& bounds);
    // Repositions the window without touching a pixel of its content --
    // UpdateLayeredWindow with a null source does exactly that, and is
    // documented to. The point of it: a move drag changes the overlay's
    // position 60 times a second but never its content, since what is
    // drawn depends on the window's *size* and grip, not where it is.
    // Without this a full-window bitmap would be cleared and refilled
    // every frame of every drag. Z-order is deliberately left alone here,
    // the same reasoning ActiveWindowHalo's own cheap path gives.
    void PresentMoveOnly(const RECT& bounds);
    // Puts an already-correct bitmap back on screen after a Hide,
    // without re-rasterizing it. Hiding deliberately does not throw the
    // content away: a preview that is being shown, hidden and shown again
    // as the pointer crosses a boundary would otherwise repaint a
    // full-screen bitmap every time, which is what the flicker was.
    void PresentShowOnly(const RECT& bounds);
    bool ContentMatches(int width, int height, int borderPx, Grip grip, bool dragging, COLORREF accent,
                        UINT dpi) const;
    void FillPx(RECT r, uint32_t premultiplied);
    void StrokePx(RECT r, int thickness, uint32_t premultiplied);

    HWND window_ = nullptr;
    HBITMAP dib_ = nullptr;
    uint32_t* bits_ = nullptr;
    int capWidth_ = 0;
    int capHeight_ = 0;
    // The bitmap's current logical size, which is what Present hands to
    // UpdateLayeredWindow -- never capWidth_/capHeight_, which only grow.
    int width_ = 0;
    int height_ = 0;

    // Everything the rendered content depends on, so an unchanged call
    // can skip the rasterize entirely. A drag calls this at pointer rate
    // and the grip only changes once at the start of it.
    RECT lastRect_{};
    int lastBorderPx_ = -1;
    Grip lastGrip_ = Grip::Move;
    bool lastDragging_ = false;
    COLORREF lastAccent_ = 0;
    UINT lastDpi_ = 0;
    // Whether the bitmap holds what the last Show* call asked for.
    // Deliberately separate from visible_: content survives a Hide, so
    // showing the same thing again costs nothing.
    bool contentValid_ = false;
    bool visible_ = false;
};

}  // namespace polish
