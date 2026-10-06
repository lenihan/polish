#include "windowtracking/RectUtils.h"

#include <dwmapi.h>

#include <cstdlib>
#include <format>

#include "util/Logging.h"

namespace polish {

bool RectsApproximatelyEqual(const RECT& a, const RECT& b, int epsilonPixels) {
    return std::abs(a.left - b.left) <= epsilonPixels && std::abs(a.top - b.top) <= epsilonPixels &&
           std::abs(a.right - b.right) <= epsilonPixels && std::abs(a.bottom - b.bottom) <= epsilonPixels;
}

bool GetVisibleWindowRect(HWND hwnd, RECT& rect) {
    const HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect));
    if (SUCCEEDED(hr)) {
        return true;
    }
    LogDebug(std::format(L"[Polish] DwmGetWindowAttribute failed for hwnd={} hr=0x{:08X}; using the raw window rect",
                          reinterpret_cast<void*>(hwnd), static_cast<unsigned>(hr)));
    return GetWindowRect(hwnd, &rect) != FALSE;
}

}  // namespace polish
