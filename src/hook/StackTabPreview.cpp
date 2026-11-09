#include "hook/StackTabPreview.h"

#include <algorithm>

#include "util/Logging.h"

namespace polish {

namespace {

constexpr wchar_t kClassName[] = L"PolishStackTabPreview";
constexpr UINT_PTR kDwellTimerId = 1;
constexpr UINT kDwellMs = 450;

// The longest each side of the preview may be, in DIPs. The window's own
// aspect ratio is kept inside this.
constexpr int kMaxWidthDip = 320;
constexpr int kMaxHeightDip = 220;
// Space between the tab and the preview, and the inset border around the
// thumbnail, in DIPs.
constexpr int kGapDip = 6;
constexpr int kBorderDip = 1;

int Scale(int dip, UINT dpi) { return MulDiv(dip, static_cast<int>(dpi), 96); }

void EnsureClassRegistered(HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = StackTabPreview::WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    // The 1px frame around the thumbnail, so it reads against any backdrop.
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH));
    registered = RegisterClassExW(&wc) != 0;
}

}  // namespace

StackTabPreview::StackTabPreview(HINSTANCE instance) : instance_(instance) {
    EnsureClassRegistered(instance);
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT, kClassName,
                              L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
}

StackTabPreview::~StackTabPreview() {
    Unregister();
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
}

LRESULT CALLBACK StackTabPreview::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return 0;
    }
    auto* self = reinterpret_cast<StackTabPreview*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_TIMER && wParam == kDwellTimerId && self != nullptr) {
        KillTimer(hwnd, kDwellTimerId);
        self->ShowNow();
        return 0;
    }
    if (message == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void StackTabPreview::Unregister() {
    if (thumbnail_ != nullptr) {
        DwmUnregisterThumbnail(thumbnail_);
        thumbnail_ = nullptr;
    }
}

void StackTabPreview::Request(HWND source, const RECT& tabRect, bool beside, UINT dpi) {
    if (window_ == nullptr || source == nullptr) {
        return;
    }
    pendingTab_ = tabRect;
    pendingBeside_ = beside;
    pendingDpi_ = dpi;
    if (source == pendingSource_ && (thumbnail_ != nullptr || IsWindowVisible(window_))) {
        return;  // already showing this one
    }
    Cancel();
    pendingSource_ = source;
    SetTimer(window_, kDwellTimerId, kDwellMs, nullptr);
}

void StackTabPreview::Cancel() {
    if (window_ == nullptr) {
        return;
    }
    KillTimer(window_, kDwellTimerId);
    ShowWindow(window_, SW_HIDE);
    Unregister();
    pendingSource_ = nullptr;
}

void StackTabPreview::ShowNow() {
    if (pendingSource_ == nullptr || !IsWindow(pendingSource_)) {
        Cancel();
        return;
    }
    // The visible frame inside the window's own rect: the thumbnail's source
    // is in the window's coordinates, shadow and resize border included.
    RECT windowRect{};
    RECT frame{};
    if (!GetWindowRect(pendingSource_, &windowRect)) {
        Cancel();
        return;
    }
    if (FAILED(DwmGetWindowAttribute(pendingSource_, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        frame = windowRect;
    }
    int sourceWidth = frame.right - frame.left;
    int sourceHeight = frame.bottom - frame.top;
    if (sourceWidth <= 0 || sourceHeight <= 0) {
        // Minimized: DWM's own size for the window is the best available.
        SIZE size{};
        Unregister();
        if (FAILED(DwmRegisterThumbnail(window_, pendingSource_, &thumbnail_)) ||
            FAILED(DwmQueryThumbnailSourceSize(thumbnail_, &size)) || size.cx <= 0 || size.cy <= 0) {
            Cancel();
            return;
        }
        sourceWidth = size.cx;
        sourceHeight = size.cy;
        frame = RECT{windowRect.left, windowRect.top, windowRect.left + sourceWidth, windowRect.top + sourceHeight};
    }

    const UINT dpi = pendingDpi_;
    const int border = std::max(1, Scale(kBorderDip, dpi));
    const double fit = std::min({1.0, static_cast<double>(Scale(kMaxWidthDip, dpi)) / sourceWidth,
                                 static_cast<double>(Scale(kMaxHeightDip, dpi)) / sourceHeight});
    const int width = std::max(1, static_cast<int>(sourceWidth * fit)) + 2 * border;
    const int height = std::max(1, static_cast<int>(sourceHeight * fit)) + 2 * border;

    // Below the tab, or to the right of it for a strip down the left side.
    const int gap = Scale(kGapDip, dpi);
    int left = pendingBeside_ ? pendingTab_.right + gap : pendingTab_.left;
    int top = pendingBeside_ ? pendingTab_.top : pendingTab_.bottom + gap;
    // Kept on the monitor the tab is on.
    const HMONITOR monitor = MonitorFromRect(&pendingTab_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        left = std::clamp<int>(left, info.rcWork.left, std::max<int>(info.rcWork.left, info.rcWork.right - width));
        top = std::clamp<int>(top, info.rcWork.top, std::max<int>(info.rcWork.top, info.rcWork.bottom - height));
    }
    SetWindowPos(window_, HWND_TOPMOST, left, top, width, height, SWP_NOACTIVATE);

    if (thumbnail_ == nullptr && FAILED(DwmRegisterThumbnail(window_, pendingSource_, &thumbnail_))) {
        LogDebug(L"[Polish] Stack: tab preview could not register a thumbnail");
        thumbnail_ = nullptr;
        Cancel();
        return;
    }
    DWM_THUMBNAIL_PROPERTIES props{};
    props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
    props.rcDestination = {border, border, width - border, height - border};
    props.rcSource = {frame.left - windowRect.left, frame.top - windowRect.top, frame.right - windowRect.left,
                      frame.bottom - windowRect.top};
    props.fVisible = TRUE;
    props.opacity = 255;
    DwmUpdateThumbnailProperties(thumbnail_, &props);
    ShowWindow(window_, SW_SHOWNOACTIVATE);
}

}  // namespace polish
