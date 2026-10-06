#pragma once

#include <windows.h>

#include <cstdint>

struct IVirtualDesktopManager;

namespace polish {

// The two pure, Win32-free pieces of the rasterizer's math, exposed for
// unit testing (see tests/HaloFalloffTests.cpp) and used together by
// ActiveWindowHalo's renderer below.
namespace halo_math {

// Signed distance from a point to the boundary of an axis-aligned
// rounded rectangle centered on the origin: `px`/`py` are the point's
// coordinates relative to the rect's own center, `halfWidth`/`halfHeight`
// are the rect's half-extents, and `radius` is the corner radius
// (clamped internally to min(halfWidth, halfHeight), so a target smaller
// than 2x the requested radius never produces a nonsensical result).
// Negative inside the shape, zero on the boundary, positive outside --
// the standard rounded-box SDF:
//   q = abs(p) - (halfSize - r); d = length(max(q, 0)) + min(max(q.x,
//   q.y), 0) - r.
float RoundedRectDistance(float px, float py, float halfWidth, float halfHeight, float radius);

// The base distance -> alpha falloff, in isolation: `peak` at d <= 0,
// fading smoothly (squared, not linear or pow) to 0 at d >= halo.
// ActiveWindowHalo's renderer layers one more adjustment on top of this
// pure piece for the actual painted pixel (see the .cpp): a cutoff below
// d = -underlap, where the glow has passed out of sight under its own
// target. That's a bound on which pixels are worth painting at all, not
// part of the glow's shape, which is why it lives there and not here.
int DistanceToAlpha(float d, int halo, int peak);

// The contrasting outline ring's alpha at signed distance `d`: `alpha`
// across the ring, antialiased over the final pixel of its outer edge,
// and 0 everywhere the ring isn't (inside the target, or past
// `thickness`). A 0 return means "not on the ring" and is what the
// renderer keys off to fall through to the glow.
//
// The ring sits just *outside* the target's own edge -- d >= 0 only --
// deliberately. Inside that edge is the underlap (see kUnderlapDip in
// the .cpp), which exists to give the target's semi-transparent frame
// border a uniform backdrop; putting the opposite tone there would
// reopen the uneven-edge bug that constant was added to close.
int OutlineAlpha(float d, int thickness, int alpha);

// Perceived luminance, 0-255, of a colour in COLORREF's own 0x00BBGGRR
// layout (what GetAccentColor returns). The usual 0.299/0.587/0.114
// weighting.
int Luminance(uint32_t colorref);

// `colorref` with its luminance pulled into [minLum, maxLum], hue kept by
// scaling all three channels together.
//
// The glow is drawn in the user's accent colour, and an accent is allowed
// to be nearly white or nearly black -- at which point it is back to
// being invisible against exactly the backdrop this change exists to
// handle. Pulling it into a middle band guarantees it differs from both a
// white page and a black terminal. Scaling can clip a channel and shift
// the hue a little at the extremes; that is accepted, since a slightly
// off hue that can be seen beats a faithful one that cannot.
uint32_t ClampGlowLuminance(uint32_t colorref, int minLum, int maxLum);

}  // namespace halo_math

// A soft glow drawn just outside the currently active (foreground)
// window's own edge, fading to transparent over roughly a quarter inch,
// so the focused window visibly lifts off the desktop. Windows 11's own
// focus cues -- a subtly different title bar shade, the DWM drop shadow
// -- are both easy to miss on a busy multi-window desktop; this is a
// much harder-to-miss third cue.
//
// Drawn in the user's accent colour, not in a tone picked from the
// theme, and that distinction is the whole point. An earlier version was
// white in dark mode and black in light mode, which sounds right and is
// not: the thing the glow has to contrast with is whatever window happens
// to be *behind* this one, and that has nothing to do with the system
// theme. Reported with a screenshot -- a black terminal, active, on top
// of a white page, on a dark-mode desktop: the white glow was invisible
// against the white page, the one place it was needed. A saturated
// mid-luminance hue reads against both a white page and a black
// terminal, where white and black each fail against one of them. It also
// matches what Alt+Tab already does, so the two focus cues finally agree.
// GetAccentColor() is read fresh on every render and never cached, the
// convention DarkMode.h's other functions document, so changing the
// accent in Settings takes effect without a restart.
//
// Underneath it, hard against the target's edge, a one-DIP ring of the
// opposite luminance to the glow -- dark under a light accent, light
// under a dark one. It gives the glow a crisp inner boundary and is the
// fallback for the case the accent cannot cover on its own: a backdrop
// that happens to be the accent's own colour. Same two-tone trick
// BullseyeOverlay uses on its ring, and the same reason -- a single-toned
// mark disappears against some part of a real desktop.
// No halo on a maximized or full-screen window (see CoversWholeMonitor
// in main.cpp) -- there's no room outside such a window's edges to draw
// into, and it's already unambiguously the focused one.
//
// Deliberately a *separate* class from AltTabHighlightBorder, not a
// generalization of it. That class draws a thin, solid ring *inside* its
// target's own edge with GDI+, alive only for the life of an Alt+Tab
// session, sized exactly to its target's current
// rect. This one draws a soft gradient glow *outside* its target,
// continuously for this app's entire run, with its own rasterizer (a
// direct per-pixel signed-distance-field fill -- see ShowAroundTarget --
// not GDI+: a PathGradientBrush fades along rays from the shape's
// *center*, so on a large window the band width varies wildly per side,
// which is exactly why the earlier gradient version of
// AltTabHighlightBorder itself was tuned into a solid ring instead) and
// its own caching/perf regime (a grow-only DIB reused across renders,
// cache-keyed on (width, height, dpi, accent colour), not a fresh
// CreateDIBSection per call). Really generalizing the two would mean
// parameterizing over all of: inside vs. outside the target, solid vs.
// gradient fill, session-scoped vs. permanently alive, GDI+ vs. a raw
// pixel buffer, and one-shared-instance vs. a fresh buffer per render --
// and re-testing two already-shipped, load-bearing features to save one
// class. This is Polish's first *continuously* rendering overlay --
// every existing overlay is session-scoped (Alt+Tab) -- which is also
// why its perf regime has to be
// stricter than AltTabHighlightBorder's.
//
// Measured full-render cost: durationMs=0 (i.e. sub-tick, well under
// GetTickCount64's ~15ms granularity) live, even for a target around
// 1720x1392 -- roughly half of a 3440x1440 screen, close to the largest a
// real halo target ever gets (a bigger one is excluded by
// CoversWholeMonitor in main.cpp). Consistent with the design: cost is
// O(perimeter), not O(area) -- the band fast-fills and column-memcpys
// dominate, and only the four small (halo+r)^2 corner boxes ever run the
// real per-pixel sqrtf math. No sign of the strip-window split
// (splitting the halo into four independent thin windows) this class's
// own perf-risk note anticipated being necessary.
class ActiveWindowHalo {
public:
    explicit ActiveWindowHalo(HINSTANCE instance);
    ~ActiveWindowHalo();

    ActiveWindowHalo(const ActiveWindowHalo&) = delete;
    ActiveWindowHalo& operator=(const ActiveWindowHalo&) = delete;

    // Positions the glow around target's current screen rect, (re)renders
    // it if anything the rendered pixels depend on has changed since the
    // last render, and makes it visible. Always safe to call speculatively
    // -- callers don't need to pre-check target's state (see main.cpp's
    // UpdateActiveWindowHalo, which is the only real caller).
    void ShowAroundTarget(HWND target);

    // Hides the glow. Safe to call when already hidden.
    void Hide();

    // The halo's own window handle -- so callers can recognize and filter
    // out the WinEvent hooks this window's own SetWindowPos/
    // UpdateLayeredWindow calls generate (e.g. a LOCATIONCHANGE per
    // rendered frame), the same way the other overlays' Handle() lets
    // main.cpp recognize its own windows.
    HWND Handle() const { return window_; }

    // The target-window size (not the glow's own, larger, inflated
    // window size) the most recent real render's cached content is good
    // for -- {-1, -1} before the first render. Callers that need to
    // decide whether their next ShowAroundTarget call will take the
    // cheap move-only path or a full re-render (e.g. to debounce a burst
    // of resize events into one render -- see main.cpp's
    // EVENT_OBJECT_LOCATIONCHANGE handling for g_haloTarget) compare
    // their own freshly-queried target size against this.
    SIZE CachedTargetSize() const { return cachedTargetSize_; }

private:
    // Repaints the buffer's content for a target of `targetWidth` x
    // `targetHeight`, inflated outward by `inflate` on each side (0 on a
    // side flush against its monitor's own edge -- see ShowAroundTarget),
    // at `dpi`, in `glow`. Updates width_/height_/dpi_/glow_ and the
    // prevXBand_ rects to match when done; does not touch the window's
    // position or visibility -- ShowAroundTarget does that once, after
    // this returns, atomically together via UpdateLayeredWindow.
    void Render(int targetWidth, int targetHeight, RECT inflate, UINT dpi, COLORREF glow);
    void EnsureDibCapacity(int width, int height);
    void ClearRect(const RECT& rect);
    void FollowTargetVirtualDesktop(HWND target);
    // Takes a HaloPlacement (int, so the enum stays private to the .cpp).
    void NotePlacement(int placement, HWND target);

    HWND window_ = nullptr;

    // Grow-only: reallocated only when a render needs more pixels than
    // the current capacity holds (naturally bounded by monitor size), not
    // freed/recreated per render -- see the class comment on why this
    // matters for a continuously-rendering overlay. `bits_` always points
    // at a `capWidth_ * capHeight_` top-down 32bpp BGRA buffer; a given
    // render only ever writes/reads its own top-left `width_ * height_`
    // corner of it (row stride is always capWidth_, never width_).
    HBITMAP dib_ = nullptr;
    uint32_t* bits_ = nullptr;
    int capWidth_ = 0;
    int capHeight_ = 0;

    // The glow window's own current content size (target size inflated by
    // the per-side halo margin, so this alone already reflects a flush-
    // edge change even when the target's own size didn't change) --
    // distinct from capWidth_/capHeight_ (the buffer's allocated
    // capacity, which only ever grows) and from cachedTargetSize_ (the
    // target's own size, exposed to callers). Doubles as most of the
    // full-render cache key, alongside dpi_/glow_ below --
    // ShowAroundTarget takes the cheap move-only path whenever a fresh
    // render would produce identical pixels to what's already in the
    // buffer. -1 before the first render (never a real size), so the
    // first call always renders.
    int width_ = -1;
    int height_ = -1;
    UINT dpi_ = 0;
    // The accent colour the cached content was rendered in, part of the
    // full-render cache key: the user can change it in Settings at any
    // time and the glow has to follow without a restart.
    COLORREF glow_ = 0;

    // The *target's* own size as of the last full render -- what
    // CachedTargetSize() reports. Deliberately separate from width_/
    // height_ above: two renders can share the same target size but
    // still need a full re-render if the target crossed a monitor's
    // flush edge (changing per-side inflation, and therefore width_/
    // height_, without changing the target's own size).
    SIZE cachedTargetSize_{-1, -1};

    // The four band rects (local buffer coordinates: top, bottom, left,
    // right -- see the .cpp's Render for what each covers) the most
    // recent full render actually painted non-transparent content into.
    // Because the buffer is reused rather than zero-filled per render,
    // the next full render must explicitly clear exactly these before
    // drawing its own new bands, or stale premultiplied pixels left
    // outside the new bands (e.g. after the target shrinks, or moves to a
    // monitor with a different flush-edge combination) composite as a
    // visible haze under ULW_ALPHA.
    RECT prevTopBand_{};
    RECT prevBottomBand_{};
    RECT prevLeftBand_{};
    RECT prevRightBand_{};
    bool hasPreviousBands_ = false;

    bool visible_ = false;
    HWND lastTarget_ = nullptr;

    // The virtual desktop id the halo window was last explicitly moved
    // to follow (see FollowTargetVirtualDesktop) -- GUID_NULL before the
    // first check. A top-level window belongs to whichever desktop it was
    // created on, so without this a halo created once at startup would
    // simply stop appearing after the user switches away from that
    // desktop.
    GUID lastKnownDesktopId_{};
    // The last HaloPlacement reported, so NotePlacement logs a change
    // rather than a line per frame. -1 is "nothing reported yet".
    int placementLogged_ = -1;

    // Created once in the constructor (COM STA is already initialized by
    // then -- see wWinMain); nullptr if CoCreateInstance failed, in which
    // case FollowTargetVirtualDesktop is a silent no-op rather than
    // retrying on every call.
    IVirtualDesktopManager* virtualDesktopManager_ = nullptr;
};

}  // namespace polish
