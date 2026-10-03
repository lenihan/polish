#include "hook/MoveModeZoneOverlay.h"

#include <algorithm>

namespace polish {

namespace {

constexpr wchar_t kClassName[] = L"PolishMoveModeZoneOverlay";

// Alphas, all tuned by eye and all worth re-checking live. The fills stay
// low because this sits on top of a real window the user is trying to
// look at: it has to say "this zone" without hiding what is under it.
constexpr int kHoverFillAlpha = 70;
constexpr int kDragFillAlpha = 90;
// The boundary between the resize band and the move area. Visible enough
// to read as a line, faint enough not to look like window chrome.
constexpr int kMapLineAlpha = 120;
constexpr int kOutlineAlpha = 200;
constexpr int kActiveEdgeAlpha = 235;

// Line weights in DIPs. The active edge is deliberately much heavier than
// anything on the map: during a drag it is the only thing drawn, and it
// has to read at a glance from wherever the pointer happens to be.
constexpr int kMapLineDip = 1;
constexpr int kOutlineDip = 2;
constexpr int kActiveEdgeDip = 4;
constexpr int kFillBorderDip = 3;

void EnsureClassRegistered(HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    registered = true;
}

// Premultiplied top-down 32bpp BGRA, alpha in the high byte, from a
// COLORREF (0x00BBGGRR). Same shape as ActiveWindowHalo's own, and
// deliberately a separate copy: four lines with no state, next to the
// only code that uses it.
uint32_t Premultiplied(int alpha, COLORREF color) {
    const uint32_t a = static_cast<uint32_t>(std::clamp(alpha, 0, 255));
    const uint32_t r = ((color & 0xFFu) * a) / 255u;
    const uint32_t g = (((color >> 8) & 0xFFu) * a) / 255u;
    const uint32_t b = (((color >> 16) & 0xFFu) * a) / 255u;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

int Scaled(int dip, UINT dpi) {
    return std::max(1, MulDiv(dip, static_cast<int>(dpi), 96));
}

// A rect moved into the bitmap's own coordinate space, whose origin is
// the overlay window's top-left rather than the screen's.
RECT ToLocal(const RECT& r, const RECT& bounds) {
    return RECT{r.left - bounds.left, r.top - bounds.top, r.right - bounds.left, r.bottom - bounds.top};
}

}  // namespace

MoveModeZoneOverlay::MoveModeZoneOverlay(HINSTANCE instance) {
    EnsureClassRegistered(instance);
    const DWORD exStyle =
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    window_ = CreateWindowExW(exStyle, kClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance,
                              nullptr);
}

MoveModeZoneOverlay::~MoveModeZoneOverlay() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
    if (dib_ != nullptr) {
        DeleteObject(dib_);
    }
}

void MoveModeZoneOverlay::EnsureCapacity(int width, int height) {
    if (dib_ != nullptr && width <= capWidth_ && height <= capHeight_) {
        return;
    }
    const int newWidth = std::max(width, capWidth_);
    const int newHeight = std::max(height, capHeight_);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = newWidth;
    // Negative height: top-down, so row 0 is the top and the pointer
    // arithmetic below reads the way it looks.
    info.bmiHeader.biHeight = -newHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        return;
    }
    if (dib_ != nullptr) {
        DeleteObject(dib_);
    }
    dib_ = dib;
    bits_ = static_cast<uint32_t*>(bits);
    capWidth_ = newWidth;
    capHeight_ = newHeight;
}

void MoveModeZoneOverlay::FillPx(RECT r, uint32_t premultiplied) {
    if (bits_ == nullptr) {
        return;
    }
    const LONG left = std::max<LONG>(0, r.left);
    const LONG top = std::max<LONG>(0, r.top);
    const LONG right = std::min<LONG>(width_, r.right);
    const LONG bottom = std::min<LONG>(height_, r.bottom);
    if (right <= left || bottom <= top) {
        return;
    }
    for (LONG y = top; y < bottom; ++y) {
        std::fill_n(bits_ + static_cast<size_t>(y) * capWidth_ + left, right - left, premultiplied);
    }
}

void MoveModeZoneOverlay::StrokePx(RECT r, int thickness, uint32_t premultiplied) {
    const LONG t = static_cast<LONG>(std::max(1, thickness));
    FillPx(RECT{r.left, r.top, r.right, r.top + t}, premultiplied);
    FillPx(RECT{r.left, r.bottom - t, r.right, r.bottom}, premultiplied);
    FillPx(RECT{r.left, r.top + t, r.left + t, r.bottom - t}, premultiplied);
    FillPx(RECT{r.right - t, r.top + t, r.right, r.bottom - t}, premultiplied);
}

void MoveModeZoneOverlay::Present(const RECT& bounds) {
    if (window_ == nullptr || bits_ == nullptr) {
        return;
    }
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(memDC, dib_));
    POINT dst{bounds.left, bounds.top};
    SIZE size{width_, height_};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateLayeredWindow(window_, screenDC, &dst, &size, memDC, &src, 0, &blend, ULW_ALPHA);
    SelectObject(memDC, oldBitmap);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    visible_ = true;
}

bool MoveModeZoneOverlay::ContentMatches(int width, int height, int borderPx, Grip grip, bool dragging,
                                         bool isFill, COLORREF accent, UINT dpi) const {
    // Position is deliberately not part of this. What gets drawn depends
    // on the window's size, not on where it sits, which is what lets a
    // move drag reuse the bitmap it already has.
    return visible_ && width_ == width && height_ == height && lastBorderPx_ == borderPx &&
           lastGrip_ == grip && lastDragging_ == dragging && lastWasFill_ == isFill &&
           lastAccent_ == accent && lastDpi_ == dpi;
}

void MoveModeZoneOverlay::ShowZones(const RECT& visible, int borderPx, Grip hovered, bool dragging,
                                     COLORREF accent, UINT dpi) {
    const int width = static_cast<int>(visible.right - visible.left);
    const int height = static_cast<int>(visible.bottom - visible.top);
    if (width <= 0 || height <= 0) {
        Hide();
        return;
    }
    if (ContentMatches(width, height, borderPx, hovered, dragging, /*isFill=*/false, accent, dpi)) {
        if (lastRect_.left != visible.left || lastRect_.top != visible.top) {
            lastRect_ = visible;
            PresentMoveOnly(visible);
        }
        return;
    }

    EnsureCapacity(width, height);
    if (bits_ == nullptr) {
        return;
    }
    width_ = width;
    height_ = height;
    for (int y = 0; y < height_; ++y) {
        std::fill_n(bits_ + static_cast<size_t>(y) * capWidth_, width_, 0u);
    }

    const RECT local = ToLocal(visible, visible);

    if (dragging) {
        // The committed look: the map is gone -- it has nothing left to
        // say once the choice is made -- and only the edges this grip
        // actually moves are drawn, heavily. That is what makes a move
        // and a resize look different while they are happening.
        // Only for a move, where the fill is the whole middle and reads
        // as "this window is travelling". A resize draws bars alone: the
        // fill would be a band the length of the window, repainted every
        // frame as the size changes, and the bars already say which edge
        // is moving.
        if (hovered == Grip::Move) {
            FillPx(ToLocal(ZoneRect(hovered, visible, borderPx), visible),
                   Premultiplied(kDragFillAlpha, accent));
        }
        const uint32_t edge = Premultiplied(kActiveEdgeAlpha, accent);
        const int t = Scaled(kActiveEdgeDip, dpi);
        if (hovered == Grip::Move) {
            StrokePx(local, t, edge);
        } else {
            if (GripMovesLeft(hovered)) {
                FillPx(RECT{local.left, local.top, local.left + t, local.bottom}, edge);
            }
            if (GripMovesRight(hovered)) {
                FillPx(RECT{local.right - t, local.top, local.right, local.bottom}, edge);
            }
            if (GripMovesTop(hovered)) {
                FillPx(RECT{local.left, local.top, local.right, local.top + t}, edge);
            }
            if (GripMovesBottom(hovered)) {
                FillPx(RECT{local.left, local.bottom - t, local.right, local.bottom}, edge);
            }
        }
    } else {
        // The map: the hovered zone filled, the band/move boundary drawn
        // so the layout can be read, and the window's own edge outlined.
        FillPx(ToLocal(ZoneRect(hovered, visible, borderPx), visible),
               Premultiplied(kHoverFillAlpha, accent));
        if (borderPx > 0) {
            StrokePx(ToLocal(ZoneRect(Grip::Move, visible, borderPx), visible), Scaled(kMapLineDip, dpi),
                     Premultiplied(kMapLineAlpha, accent));
        }
        StrokePx(local, Scaled(kOutlineDip, dpi), Premultiplied(kOutlineAlpha, accent));
    }

    lastRect_ = visible;
    lastBorderPx_ = borderPx;
    lastGrip_ = hovered;
    lastDragging_ = dragging;
    lastWasFill_ = false;
    lastAccent_ = accent;
    lastDpi_ = dpi;
    Present(visible);
}

void MoveModeZoneOverlay::ShowFill(const RECT& rect, COLORREF accent, UINT dpi) {
    const int width = static_cast<int>(rect.right - rect.left);
    const int height = static_cast<int>(rect.bottom - rect.top);
    if (width <= 0 || height <= 0) {
        Hide();
        return;
    }
    if (ContentMatches(width, height, lastBorderPx_, lastGrip_, lastDragging_, /*isFill=*/true, accent,
                       dpi)) {
        if (lastRect_.left != rect.left || lastRect_.top != rect.top) {
            lastRect_ = rect;
            PresentMoveOnly(rect);
        }
        return;
    }

    EnsureCapacity(width, height);
    if (bits_ == nullptr) {
        return;
    }
    width_ = width;
    height_ = height;
    const RECT local{0, 0, width, height};
    FillPx(local, Premultiplied(kHoverFillAlpha, accent));
    StrokePx(local, Scaled(kFillBorderDip, dpi), Premultiplied(kActiveEdgeAlpha, accent));

    lastRect_ = rect;
    lastWasFill_ = true;
    lastAccent_ = accent;
    lastDpi_ = dpi;
    Present(rect);
}

void MoveModeZoneOverlay::PresentMoveOnly(const RECT& bounds) {
    if (window_ == nullptr) {
        return;
    }
    POINT dst{bounds.left, bounds.top};
    UpdateLayeredWindow(window_, nullptr, &dst, nullptr, nullptr, nullptr, 0, nullptr, 0);
}

void MoveModeZoneOverlay::Hide() {
    if (window_ != nullptr && visible_) {
        ShowWindow(window_, SW_HIDE);
    }
    visible_ = false;
    lastBorderPx_ = -1;
}

}  // namespace polish
