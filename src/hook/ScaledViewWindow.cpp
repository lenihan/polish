#include "hook/ScaledViewWindow.h"

#include "util/Logging.h"
#include "windowtracking/ScaleGeometry.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <string>
#include <utility>

namespace polish {

namespace {

constexpr wchar_t kClassName[] = L"PolishScaledView";

constexpr int kCornerZoneDip = 14;
constexpr int kBorderPx = 1;

constexpr UINT kMenuRestore = 1;
constexpr UINT kMenuPreset50 = 2;
constexpr UINT kMenuPreset33 = 3;
constexpr UINT kMenuPreset25 = 4;
constexpr UINT kMenuTopmost = 5;
constexpr UINT kMenuClose = 6;

void EnsureClassRegistered(HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ScaledViewWindow::WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    RegisterClassExW(&wc);
    registered = true;
}

COLORREF AccentColor() {
    DWORD color = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque))) {
        return RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
    }
    return GetSysColor(COLOR_HIGHLIGHT);
}

}  // namespace

LRESULT CALLBACK ScaledViewWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    ScaledViewWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<ScaledViewWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        // CreateWindowEx has not returned yet, so window_ is still null,
        // and the default handling of the creation messages needs it.
        self->window_ = hwnd;
    } else {
        self = reinterpret_cast<ScaledViewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->HandleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

ScaledViewWindow::ScaledViewWindow(HINSTANCE instance, HWND source, SIZE realSize, const RECT& viewRect,
                                   bool topmost, Callbacks callbacks)
    : source_(source), realSize_(realSize), rect_(viewRect), topmost_(topmost), callbacks_(std::move(callbacks)) {
    EnsureClassRegistered(instance);
    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (topmost_) {
        exStyle |= WS_EX_TOPMOST;
    }
    window_ = CreateWindowExW(exStyle, kClassName, L"", WS_POPUP, rect_.left, rect_.top, rect_.right - rect_.left,
                              rect_.bottom - rect_.top, nullptr, nullptr, instance, this);
    if (window_ == nullptr) {
        LogDebug(std::format(L"[Polish] Scale: view window creation failed, GetLastError={}", GetLastError()));
        return;
    }
    const HRESULT hr = DwmRegisterThumbnail(window_, source_, &thumbnail_);
    if (FAILED(hr)) {
        LogDebug(std::format(L"[Polish] Scale: DwmRegisterThumbnail failed, hr=0x{:08x}",
                             static_cast<uint32_t>(hr)));
        thumbnail_ = nullptr;
        return;
    }
    UpdateThumbnail();
}

ScaledViewWindow::~ScaledViewWindow() {
    if (thumbnail_ != nullptr) {
        DwmUnregisterThumbnail(thumbnail_);
    }
    if (window_ != nullptr) {
        SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        DestroyWindow(window_);
    }
}

void ScaledViewWindow::UpdateThumbnail() {
    if (thumbnail_ == nullptr || window_ == nullptr) {
        return;
    }
    // The visible frame inside the window's own rect: the thumbnail's
    // source is in the window's coordinates, shadow and resize border
    // included, and the view is of what the user sees.
    RECT windowRect{};
    RECT frame{};
    GetWindowRect(source_, &windowRect);
    if (FAILED(DwmGetWindowAttribute(source_, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        frame = windowRect;
    }
    RECT client{};
    GetClientRect(window_, &client);

    DWM_THUMBNAIL_PROPERTIES props{};
    props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
    props.rcDestination = {client.left + kBorderPx, client.top + kBorderPx, client.right - kBorderPx,
                           client.bottom - kBorderPx};
    props.rcSource = {frame.left - windowRect.left, frame.top - windowRect.top, frame.right - windowRect.left,
                      frame.bottom - windowRect.top};
    props.fVisible = TRUE;
    props.opacity = 255;
    DwmUpdateThumbnailProperties(thumbnail_, &props);
}

void ScaledViewWindow::ApplyRect(const RECT& rect) {
    rect_ = rect;
    if (window_ == nullptr) {
        return;
    }
    SetWindowPos(window_, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    UpdateThumbnail();
}

void ScaledViewWindow::SetRect(const RECT& viewRect) {
    ApplyRect(viewRect);
}

void ScaledViewWindow::SetRealSize(SIZE realSize) {
    realSize_ = realSize;
    UpdateThumbnail();
}

void ScaledViewWindow::SetTopmost(bool topmost) {
    topmost_ = topmost;
    if (window_ != nullptr) {
        SetWindowPos(window_, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void ScaledViewWindow::Show() {
    if (window_ == nullptr) {
        return;
    }
    SetWindowPos(window_, topmost_ ? HWND_TOPMOST : HWND_TOP, rect_.left, rect_.top, rect_.right - rect_.left,
                 rect_.bottom - rect_.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateThumbnail();
    visible_ = true;
}

void ScaledViewWindow::Hide() {
    if (window_ != nullptr) {
        ShowWindow(window_, SW_HIDE);
    }
    visible_ = false;
}

// Which corner zone a client point is in: 0..3 as Corner, or -1. Returned
// by value and through `cornerOut` only so callers that need both a test
// and the index read cleanly.
int ScaledViewWindow::CornerAt(POINT p, int* cornerOut) const {
    RECT client{};
    GetClientRect(window_, &client);
    const UINT dpi = GetDpiForWindow(window_);
    const int zone = std::max(8, MulDiv(kCornerZoneDip, static_cast<int>(dpi), 96));
    const bool left = p.x < client.left + zone;
    const bool right = p.x >= client.right - zone;
    const bool top = p.y < client.top + zone;
    const bool bottom = p.y >= client.bottom - zone;
    int corner = -1;
    if (top && left) corner = static_cast<int>(Corner::TopLeft);
    else if (top && right) corner = static_cast<int>(Corner::TopRight);
    else if (bottom && left) corner = static_cast<int>(Corner::BottomLeft);
    else if (bottom && right) corner = static_cast<int>(Corner::BottomRight);
    if (cornerOut != nullptr) {
        *cornerOut = corner;
    }
    return corner;
}

void ScaledViewWindow::ShowMenu() {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    const double scale = ScaleOf(rect_, realSize_);
    const std::wstring header = std::format(L"Scaled to {}%", static_cast<int>(scale * 100.0 + 0.5));
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, header.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuRestore, L"Restore to 100%");
    AppendMenuW(menu, MF_STRING, kMenuPreset50, L"50%");
    AppendMenuW(menu, MF_STRING, kMenuPreset33, L"33%");
    AppendMenuW(menu, MF_STRING, kMenuPreset25, L"25%");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (topmost_ ? MF_CHECKED : MF_UNCHECKED), kMenuTopmost, L"Always on top");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuClose, L"Close window");

    POINT cursor{};
    GetCursorPos(&cursor);
    // The menu needs a foreground owner or it will not dismiss on an
    // outside click. The view is no-activate, so this is the one place it
    // takes the foreground, and only for the menu's lifetime.
    SetForegroundWindow(window_);
    const UINT command = static_cast<UINT>(
        TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, cursor.x, cursor.y, window_, nullptr));
    DestroyMenu(menu);
    PostMessageW(window_, WM_NULL, 0, 0);

    switch (command) {
        case kMenuRestore:
            if (callbacks_.onRestore) callbacks_.onRestore();
            break;
        case kMenuPreset50:
            if (callbacks_.onSetScale) callbacks_.onSetScale(0.5);
            break;
        case kMenuPreset33:
            if (callbacks_.onSetScale) callbacks_.onSetScale(0.33);
            break;
        case kMenuPreset25:
            if (callbacks_.onSetScale) callbacks_.onSetScale(0.25);
            break;
        case kMenuTopmost:
            SetTopmost(!topmost_);
            if (callbacks_.onTopmostChanged) callbacks_.onTopmostChanged(topmost_);
            break;
        case kMenuClose:
            if (callbacks_.onClose) callbacks_.onClose();
            break;
        default:
            break;
    }
}

LRESULT ScaledViewWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            RECT client{};
            GetClientRect(window_, &client);
            HBRUSH brush = CreateSolidBrush(AccentColor());
            FillRect(dc, &client, brush);
            DeleteObject(brush);
            return 1;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_SETCURSOR: {
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(window_, &cursor);
            int corner = -1;
            CornerAt(cursor, &corner);
            LPCWSTR id = IDC_HAND;
            if (corner == static_cast<int>(Corner::TopLeft) || corner == static_cast<int>(Corner::BottomRight)) {
                id = IDC_SIZENWSE;
            } else if (corner == static_cast<int>(Corner::TopRight) ||
                       corner == static_cast<int>(Corner::BottomLeft)) {
                id = IDC_SIZENESW;
            }
            SetCursor(LoadCursorW(nullptr, id));
            return TRUE;
        }

        case WM_LBUTTONDOWN: {
            SetCapture(window_);
            GetCursorPos(&dragStartCursor_);
            dragStartRect_ = rect_;
            const POINT client{static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
                               static_cast<LONG>(static_cast<short>(HIWORD(lParam)))};
            int corner = -1;
            CornerAt(client, &corner);
            if (corner >= 0) {
                drag_ = Drag::Resize;
                dragCorner_ = corner;
            } else {
                drag_ = Drag::Pending;
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (drag_ == Drag::None) {
                return 0;
            }
            POINT cursor{};
            GetCursorPos(&cursor);
            if (drag_ == Drag::Pending) {
                const int slopX = GetSystemMetrics(SM_CXDRAG);
                const int slopY = GetSystemMetrics(SM_CYDRAG);
                if (std::abs(cursor.x - dragStartCursor_.x) <= slopX &&
                    std::abs(cursor.y - dragStartCursor_.y) <= slopY) {
                    return 0;
                }
                drag_ = Drag::Move;
            }
            if (drag_ == Drag::Move) {
                RECT moved = dragStartRect_;
                OffsetRect(&moved, cursor.x - dragStartCursor_.x, cursor.y - dragStartCursor_.y);
                ApplyRect(moved);
            } else if (drag_ == Drag::Resize) {
                ApplyRect(CornerDragAspectLocked(dragStartRect_, static_cast<Corner>(dragCorner_), cursor, realSize_));
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            const Drag finished = drag_;
            drag_ = Drag::None;
            if (GetCapture() == window_) {
                ReleaseCapture();
            }
            if (finished == Drag::Pending) {
                if (callbacks_.onRestore) callbacks_.onRestore();
            } else if (finished == Drag::Move) {
                if (callbacks_.onViewChanged) callbacks_.onViewChanged(rect_);
            } else if (finished == Drag::Resize) {
                const double snapped = SnapScale(ScaleOf(rect_, realSize_));
                if (snapped >= kMaxScale - 1e-6) {
                    // Dragged up to full size: that is a restore.
                    if (callbacks_.onRestore) callbacks_.onRestore();
                } else {
                    ApplyRect(RectAtScaleKeepingCorner(dragStartRect_, static_cast<Corner>(dragCorner_), snapped,
                                                       realSize_));
                    if (callbacks_.onViewChanged) callbacks_.onViewChanged(rect_);
                }
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            drag_ = Drag::None;
            return 0;

        case WM_RBUTTONUP:
            ShowMenu();
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

}  // namespace polish
