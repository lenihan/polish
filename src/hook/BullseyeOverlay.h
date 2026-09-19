#pragma once

#include <windows.h>

#include <cstdint>

namespace polish {

// The pure, Win32-free pieces of the bullseye animation's math, exposed for
// unit testing (see tests/BullseyeMathTests.cpp) and used together by
// BullseyeOverlay's renderer below -- the same split halo_math makes for
// ActiveWindowHalo.
namespace bullseye_math {

// Progress `t` (clamped to [0, 1]) -> eased progress: fast at first,
// settling gently at the end. 0 -> 0, 1 -> 1, monotonic in between.
float EaseOutCubic(float t);

// The ring's centre-line radius at progress `t`. Expanding (paste) grows
// 0 -> maxRadius; collapsing (copy) shrinks maxRadius -> 0. Both run
// through EaseOutCubic, so a collapse rushes in and then settles onto the
// copy point, and an expansion bursts out and coasts.
float RadiusAt(float t, float maxRadius, bool expanding);

// The ring's overall opacity at progress `t`: 0 at t=0, ramping to `peak`
// over the first fifth, holding, then easing back to 0 over the last two
// fifths. Shared by both phases -- only the radius direction differs.
int AlphaEnvelope(float t, int peak);

// Distance from a point (relative to the ring's centre) to the ring's
// centre line: 0 exactly on the circle, growing either side of it.
float RingDistance(float px, float py, float radius);

// Distance-to-centre-line -> alpha: `peak` at d = 0, fading smoothly
// (squared, the same shape as halo_math::DistanceToAlpha) to 0 at
// d >= halfStroke.
int DistanceToAlpha(float d, float halfStroke, int peak);

}  // namespace bullseye_math

// Which way the ring moves: a copy collapses onto the point, a paste
// expands out of it.
enum class BullseyePhase { Copy, Paste };

// A brief, large, soft ring that plays on top of every window at the spot
// where the user just copied or pasted -- collapsing onto the point for a
// copy, expanding out of it for a paste. White in dark mode, black in
// light mode (IsDarkModeEnabled() is read fresh every frame, never
// cached -- the same codebase-wide convention ActiveWindowHalo follows).
//
// Deliberately a *separate* class from ActiveWindowHalo, not a
// generalization of it, for the same reason ActiveWindowHalo isn't one of
// AltTabHighlightBorder (see its class comment). Bullseye shares the
// halo's rendering regime -- a layered window, a grow-only DIB, a hand-
// written per-pixel rasterizer with a pure-white/black single-store
// premultiply -- but differs on every axis that matters: a circle instead
// of a rounded rectangle; transient (a few hundred milliseconds) instead
// of permanently visible; animated (a repeating frame tick) instead of
// re-rendered once per event; WS_EX_TOPMOST instead of HWND_TOP; and
// centred on a bare screen point instead of sized to a target window. Its
// SDF is a single ring rather than a rounded-box, and its cache story is
// simpler (fixed geometry for the whole animation), so folding it into
// ActiveWindowHalo would mean parameterizing nearly everything to share
// almost nothing.
//
// WS_EX_TOPMOST is a deliberate divergence from the halo (see
// docs/LIMITATIONS.md #8, where the halo's own HWND_TOP is explained):
// the halo is permanent, so being topmost would mean it paints over every
// app forever. This is a transient the user explicitly wants to see on
// top of everything, taskbar included.
//
// Geometry is fixed for a whole animation: Start() resolves DPI once from
// the monitor under the point, sizes a square buffer to fit the largest
// ring, and centres the window on the point. Each frame then only changes
// the pixels in that buffer -- position and size never change, so every
// frame is a single UpdateLayeredWindow.
//
// Driven by the caller's own timer: Start() begins (or restarts) an
// animation, and the caller then calls AdvanceFrame() on each tick until it
// returns false. Progress is computed from QueryPerformanceCounter, not a
// frame count, so a dropped or late tick shortens nothing -- the animation
// always lasts the same wall-clock time.
class BullseyeOverlay {
public:
    explicit BullseyeOverlay(HINSTANCE instance);
    ~BullseyeOverlay();

    BullseyeOverlay(const BullseyeOverlay&) = delete;
    BullseyeOverlay& operator=(const BullseyeOverlay&) = delete;

    // Begins an animation centred on `screenPoint` (raw screen pixels).
    // Safe to call mid-animation -- it simply restarts from the beginning
    // at the new point. Renders nothing itself; the first AdvanceFrame
    // does.
    void Start(BullseyePhase phase, POINT screenPoint);

    // Renders the frame for "now" and shows the window. Returns true while
    // the animation is still running; returns false (after hiding the
    // window) once it has finished, or if none is running -- the caller
    // should stop its timer on false.
    bool AdvanceFrame();

    // Cancels any running animation immediately and hides the window.
    // Safe to call when already hidden.
    void Hide();

    // Whether an animation is currently running.
    bool IsActive() const { return active_; }

    // The overlay's own window handle -- so main.cpp's OnWinEvent can
    // ignore the LOCATIONCHANGE events this topmost, per-frame-updated
    // window generates, the same way it already does for the halo.
    HWND Handle() const { return window_; }

private:
    void EnsureDibCapacity(int width, int height);
    void ClearRect(const RECT& rect);
    void RenderFrame(float progress, bool isDark);

    HWND window_ = nullptr;

    // Grow-only, same regime as ActiveWindowHalo's: `bits_` always points
    // at a `capWidth_ * capHeight_` top-down 32bpp BGRA buffer (row stride
    // is always capWidth_), and a given animation only ever uses its own
    // top-left `size_ * size_` corner.
    HBITMAP dib_ = nullptr;
    uint32_t* bits_ = nullptr;
    int capWidth_ = 0;
    int capHeight_ = 0;

    bool active_ = false;
    bool needsShow_ = false;  // first frame of an animation asserts TOPMOST + visibility
    bool visible_ = false;
    BullseyePhase phase_ = BullseyePhase::Copy;

    // Per-animation geometry, all in raw screen pixels, fixed by Start().
    int size_ = 0;         // square buffer/window edge
    float maxRadius_ = 0;  // ring centre-line radius at its largest
    float halfStroke_ = 0;
    POINT origin_{};       // window's top-left, so the ring centres on the requested point

    LARGE_INTEGER startCounter_{};
    LARGE_INTEGER frequency_{};

    // Bounding box (buffer coordinates) the most recent frame painted
    // into. The buffer is reused rather than zero-filled per frame, so the
    // next frame must clear exactly this before drawing, or stale
    // premultiplied pixels outside the new ring composite as a visible
    // haze under ULW_ALPHA -- the same hazard ActiveWindowHalo documents
    // for its own band rects. Deliberately survives across animations: a
    // restart mid-animation leaves a ring in the buffer that the next
    // frame must still clear.
    RECT prevDirty_{};
};

}  // namespace polish
