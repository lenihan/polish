#pragma once

#include <windows.h>

namespace polish {

// The translucent panel showing where a half/quarter/maximize drop would
// put the window.
//
// Deliberately NOT MoveModeZoneOverlay, which is what this started as.
// That class rasterizes per-pixel alpha into a DIB and pushes it with
// UpdateLayeredWindow, which is right for the zone map -- different
// alphas in different places, and only ever the size of one window. For
// this it was the wrong tool and measurably so: a maximize preview is
// the whole work area, so every appearance meant filling 5.25 million
// pixels and uploading 21MB to the compositor. Reported as the preview
// usually not showing at all -- it was rendering, but it arrived so late
// that a normal drag was already over. The drop still maximized, because
// that is settled from the cursor at release, which is exactly the
// "works even though it never went blue" that was described.
//
// A flat SetLayeredWindowAttributes alpha costs nothing by comparison:
// no bitmap, no upload, just a solid fill the compositor blends. The
// panel is one uniform colour, so there is nothing per-pixel alpha was
// buying. Same technique AltTabDimOverlay uses, for the same reason.
//
// The border is a second, more opaque colour rather than a second alpha,
// since LWA_ALPHA applies one value to the whole window -- so it is drawn
// as a lighter shade of the accent instead, which reads as an edge
// without needing per-pixel control.
class MoveModeLayoutPreview {
public:
    explicit MoveModeLayoutPreview(HINSTANCE instance);
    ~MoveModeLayoutPreview();

    MoveModeLayoutPreview(const MoveModeLayoutPreview&) = delete;
    MoveModeLayoutPreview& operator=(const MoveModeLayoutPreview&) = delete;

    // Shows the panel filling `rect`. Cheap enough to call on every
    // pointer move: an unchanged rect and colour is a no-op, and even a
    // changed one is a SetWindowPos and a solid fill.
    void Show(const RECT& rect, COLORREF accent, UINT dpi);

    void Hide();

private:
    HWND window_ = nullptr;
    RECT lastRect_{};
    COLORREF lastAccent_ = 0;
    UINT lastDpi_ = 0;
    bool visible_ = false;
};

}  // namespace polish
