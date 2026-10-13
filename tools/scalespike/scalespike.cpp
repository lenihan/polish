// Phase 0 spike for scaled windows: can a window be "parked" (invisible,
// click-through) while a DWM thumbnail of it keeps updating live?
//
// Standalone on purpose -- links only user32/dwmapi/gdi32/advapi32, never
// polish_core -- so the answer cannot be contaminated by Polish itself.
//
//   scalespike --title notepad --strategy ghost-topmost --scale 0.33 --seconds 60 [--cover]
//   scalespike --pick ...         (targets the foreground window after 3 s)
//   scalespike --hwnd 0x1234 ...
//   scalespike --restore          (replays the journal after a crash)
//
// Strategies:
//   none          baseline control: window untouched, view beside it
//   offscreen     move the window past the virtual screen's right edge
//   ghost         WS_EX_LAYERED|WS_EX_TRANSPARENT, alpha 1, left in place
//   ghost-topmost ghost + HWND_TOPMOST (cannot be covered by anything)
//   printwindow   offscreen + PrintWindow(PW_RENDERFULLCONTENT) ~1/s, no thumbnail
//
// The measurement is taken from the *screen* (BitBlt of the view's area),
// not from DWM's own bookkeeping, so "live" means a user would see it move.
// The view's background is magenta; "coverage" is the share of sampled pixels
// that are not magenta, i.e. whether the thumbnail painted at all (an alpha-1
// source might composite as near-invisible).
//
// Right-click the view, or Ctrl+C in the console, to stop early. The window's
// original style/rect is journalled to HKCU\Software\Polish\Spike before any
// change, and restored on exit, Ctrl+C, crash filter, or --restore.

#include <windows.h>

#include <dwmapi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kRegKey[] = L"Software\\Polish\\Spike";
constexpr wchar_t kRegValue[] = L"Parked";
constexpr COLORREF kMagenta = RGB(255, 0, 255);
constexpr UINT_PTR kSampleTimer = 1;
constexpr UINT_PTR kEndTimer = 2;
constexpr UINT_PTR kSnapTimer = 3;

enum class Strategy { None, Offscreen, Ghost, GhostTopmost, PrintWindow };

struct Saved {
    uint64_t hwnd = 0;
    LONG_PTR exStyle = 0;
    int hadLayered = 0;
    int alpha = 255;
    COLORREF colorKey = 0;
    DWORD flags = 0;
    int topmost = 0;
    RECT rect{};
    int valid = 0;
};

Saved g_saved;
Strategy g_strategy = Strategy::None;
bool g_cover = false;
HWND g_target = nullptr;
HWND g_view = nullptr;
HWND g_coverWindow = nullptr;
HTHUMBNAIL g_thumb = nullptr;
SIZE g_viewSize{};
RECT g_sourceRect{};  // visible frame within the window's own rect

// Printwindow strategy: the latest snapshot.
HDC g_snapDc = nullptr;
HBITMAP g_snapBmp = nullptr;
SIZE g_snapSize{};

// Measurement.
int g_samples = 0;
int g_changed = 0;
double g_coverageSum = 0;
double g_coverageMin = 100;
uint64_t g_lastHash = 0;
ULONGLONG g_startTick = 0;
FILE* g_logFile = nullptr;

void Log(const wchar_t* fmt, ...) {
    wchar_t buffer[512];
    va_list args;
    va_start(args, fmt);
    vswprintf(buffer, 512, fmt, args);
    va_end(args);
    wprintf(L"%s\n", buffer);
    if (g_logFile != nullptr) {
        fwprintf(g_logFile, L"%s\n", buffer);
        fflush(g_logFile);
    }
}

// ---- journal / restore -----------------------------------------------------

void WriteJournal() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
        RegSetValueExW(key, kRegValue, 0, REG_BINARY, reinterpret_cast<const BYTE*>(&g_saved), sizeof(g_saved));
        RegCloseKey(key);
    }
}

void ClearJournal() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, kRegValue);
        RegCloseKey(key);
    }
}

bool ReadJournal(Saved& out) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD size = sizeof(out);
    const bool ok = RegQueryValueExW(key, kRegValue, nullptr, nullptr, reinterpret_cast<BYTE*>(&out), &size) ==
                        ERROR_SUCCESS &&
                    size == sizeof(out);
    RegCloseKey(key);
    return ok && out.valid;
}

void RestoreFrom(const Saved& saved) {
    HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(saved.hwnd));
    if (!IsWindow(hwnd)) {
        return;
    }
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, saved.exStyle);
    if (saved.hadLayered) {
        SetLayeredWindowAttributes(hwnd, saved.colorKey, static_cast<BYTE>(saved.alpha), saved.flags);
    }
    SetWindowPos(hwnd, saved.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, saved.rect.left, saved.rect.top,
                 saved.rect.right - saved.rect.left, saved.rect.bottom - saved.rect.top,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void RestoreTarget() {
    if (g_saved.valid) {
        RestoreFrom(g_saved);
        g_saved.valid = 0;
        ClearJournal();
    }
}

BOOL WINAPI ConsoleHandler(DWORD) {
    RestoreTarget();
    ExitProcess(2);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS*) {
    RestoreTarget();
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- target selection ------------------------------------------------------

struct FindState {
    std::wstring needle;
    HWND found = nullptr;
};

BOOL CALLBACK FindProc(HWND hwnd, LPARAM lParam) {
    auto* state = reinterpret_cast<FindState*>(lParam);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }
    wchar_t title[512] = {};
    GetWindowTextW(hwnd, title, 512);
    std::wstring lower = title;
    for (wchar_t& c : lower) {
        c = static_cast<wchar_t>(towlower(c));
    }
    if (lower.find(state->needle) != std::wstring::npos && hwnd != GetConsoleWindow()) {
        state->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

// ---- parking ---------------------------------------------------------------

bool Park(HWND hwnd) {
    g_saved = {};
    g_saved.hwnd = reinterpret_cast<uintptr_t>(hwnd);
    g_saved.exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    g_saved.topmost = (g_saved.exStyle & WS_EX_TOPMOST) != 0;
    GetWindowRect(hwnd, &g_saved.rect);
    if (g_saved.exStyle & WS_EX_LAYERED) {
        BYTE alpha = 255;
        COLORREF key = 0;
        DWORD flags = 0;
        if (!GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags)) {
            Log(L"REFUSED: window is per-pixel layered (UpdateLayeredWindow); cannot ghost it");
            return false;
        }
        g_saved.hadLayered = 1;
        g_saved.alpha = alpha;
        g_saved.colorKey = key;
        g_saved.flags = flags;
    }
    g_saved.valid = 1;
    WriteJournal();  // before touching anything

    const int width = g_saved.rect.right - g_saved.rect.left;
    const int height = g_saved.rect.bottom - g_saved.rect.top;
    switch (g_strategy) {
        case Strategy::None:
            break;
        case Strategy::Offscreen:
        case Strategy::PrintWindow: {
            const int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 100;
            SetWindowPos(hwnd, nullptr, x, g_saved.rect.top, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
            break;
        }
        case Strategy::Ghost:
        case Strategy::GhostTopmost:
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, g_saved.exStyle | WS_EX_LAYERED | WS_EX_TRANSPARENT);
            SetLayeredWindowAttributes(hwnd, 0, 1, LWA_ALPHA);
            if (g_strategy == Strategy::GhostTopmost) {
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            break;
    }
    return true;
}

// ---- measurement -----------------------------------------------------------

void Sample() {
    RECT client;
    GetClientRect(g_view, &client);
    POINT origin{0, 0};
    ClientToScreen(g_view, &origin);
    const int width = client.right;
    const int height = client.bottom;
    if (width <= 0 || height <= 0) {
        return;
    }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    BitBlt(mem, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT);

    uint64_t hash = 1469598103934665603ULL;
    int total = 0;
    int painted = 0;
    const auto* px = static_cast<const uint8_t*>(bits);
    for (int y = 0; y < height; y += 6) {
        for (int x = 0; x < width; x += 6) {
            const uint8_t* p = px + (static_cast<size_t>(y) * width + x) * 4;  // B G R X
            hash = (hash ^ (p[0] | (p[1] << 8) | (p[2] << 16))) * 1099511628211ULL;
            ++total;
            if (!(p[0] == 255 && p[1] == 0 && p[2] == 255)) {
                ++painted;
            }
        }
    }
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    const double coverage = total > 0 ? 100.0 * painted / total : 0;
    ++g_samples;
    if (g_samples > 1 && hash != g_lastHash) {
        ++g_changed;
    }
    g_lastHash = hash;
    g_coverageSum += coverage;
    g_coverageMin = coverage < g_coverageMin ? coverage : g_coverageMin;
    if (g_samples % 10 == 0) {
        Log(L"  t=%3llus samples=%d changed=%d coverage=%.0f%%", (GetTickCount64() - g_startTick) / 1000, g_samples,
            g_changed, coverage);
    }
}

void TakeSnapshot() {
    if (!IsWindow(g_target) || g_snapDc == nullptr) {
        return;
    }
    RECT rect;
    GetWindowRect(g_target, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (g_snapBmp == nullptr || g_snapSize.cx != width || g_snapSize.cy != height) {
        HDC screen = GetDC(nullptr);
        HBITMAP fresh = CreateCompatibleBitmap(screen, width, height);
        ReleaseDC(nullptr, screen);
        SelectObject(g_snapDc, fresh);
        if (g_snapBmp != nullptr) {
            DeleteObject(g_snapBmp);
        }
        g_snapBmp = fresh;
        g_snapSize = {width, height};
    }
    PrintWindow(g_target, g_snapDc, 2 /*PW_RENDERFULLCONTENT*/);
    InvalidateRect(g_view, nullptr, FALSE);
}

// ---- windows ---------------------------------------------------------------

LRESULT CALLBACK ViewProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            RECT rect;
            GetClientRect(hwnd, &rect);
            HBRUSH brush = CreateSolidBrush(kMagenta);
            FillRect(dc, &rect, brush);
            DeleteObject(brush);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (g_strategy == Strategy::PrintWindow && g_snapBmp != nullptr) {
                SetStretchBltMode(dc, HALFTONE);
                StretchBlt(dc, 0, 0, g_viewSize.cx, g_viewSize.cy, g_snapDc, g_sourceRect.left, g_sourceRect.top,
                           g_sourceRect.right - g_sourceRect.left, g_sourceRect.bottom - g_sourceRect.top, SRCCOPY);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wParam == kSampleTimer) {
                Sample();
            } else if (wParam == kSnapTimer) {
                TakeSnapshot();
            } else if (wParam == kEndTimer) {
                PostQuitMessage(0);
            }
            return 0;
        case WM_RBUTTONUP:
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK CoverProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_KEYDOWN && wParam == VK_ESCAPE) {
        PostQuitMessage(0);
        return 0;
    }
    if (message == WM_ERASEBKGND) {
        HDC dc = reinterpret_cast<HDC>(wParam);
        RECT rect;
        GetClientRect(hwnd, &rect);
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        return 1;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool ParseStrategy(const std::wstring& name) {
    if (name == L"none") g_strategy = Strategy::None;
    else if (name == L"offscreen") g_strategy = Strategy::Offscreen;
    else if (name == L"ghost") g_strategy = Strategy::Ghost;
    else if (name == L"ghost-topmost") g_strategy = Strategy::GhostTopmost;
    else if (name == L"printwindow") g_strategy = Strategy::PrintWindow;
    else return false;
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring title;
    std::wstring strategyName = L"none";
    double scale = 0.33;
    int seconds = 60;
    bool pick = false;
    bool restoreOnly = false;
    HWND explicitHwnd = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool hasNext = i + 1 < argc;
        if (arg == L"--title" && hasNext) {
            title = argv[++i];
            for (wchar_t& c : title) c = static_cast<wchar_t>(towlower(c));
        } else if (arg == L"--hwnd" && hasNext) {
            explicitHwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(wcstoull(argv[++i], nullptr, 0)));
        } else if (arg == L"--strategy" && hasNext) {
            strategyName = argv[++i];
        } else if (arg == L"--scale" && hasNext) {
            scale = wcstod(argv[++i], nullptr);
        } else if (arg == L"--seconds" && hasNext) {
            seconds = _wtoi(argv[++i]);
        } else if (arg == L"--pick") {
            pick = true;
        } else if (arg == L"--cover") {
            g_cover = true;
        } else if (arg == L"--restore") {
            restoreOnly = true;
        } else {
            wprintf(L"unknown/incomplete argument: %s\n", arg.c_str());
            return 1;
        }
    }

    if (restoreOnly) {
        Saved saved;
        if (!ReadJournal(saved)) {
            wprintf(L"nothing journalled\n");
            return 0;
        }
        RestoreFrom(saved);
        ClearJournal();
        wprintf(L"restored window 0x%llx\n", static_cast<unsigned long long>(saved.hwnd));
        return 0;
    }

    if (!ParseStrategy(strategyName) || scale <= 0.05 || scale > 1.0) {
        wprintf(L"bad --strategy (none|offscreen|ghost|ghost-topmost|printwindow) or --scale (0.05..1)\n");
        return 1;
    }

    wchar_t logPath[MAX_PATH];
    GetTempPathW(MAX_PATH, logPath);
    wcscat_s(logPath, L"scalespike.log");
    g_logFile = _wfopen(logPath, L"a, ccs=UTF-8");

    // Anything left over from a previous crash goes back first.
    {
        Saved stale;
        if (ReadJournal(stale)) {
            Log(L"replaying stale journal for 0x%llx", static_cast<unsigned long long>(stale.hwnd));
            RestoreFrom(stale);
            ClearJournal();
        }
    }

    if (pick) {
        wprintf(L"focus the target window now; picking in 3 s...\n");
        Sleep(3000);
        g_target = GetForegroundWindow();
    } else if (explicitHwnd != nullptr) {
        g_target = explicitHwnd;
    } else if (!title.empty()) {
        FindState state;
        state.needle = title;
        EnumWindows(FindProc, reinterpret_cast<LPARAM>(&state));
        g_target = state.found;
    }
    if (g_target == nullptr || !IsWindow(g_target) || g_target == GetConsoleWindow()) {
        wprintf(L"no target window (use --title, --hwnd or --pick)\n");
        return 1;
    }
    if (IsIconic(g_target)) {
        ShowWindow(g_target, SW_RESTORE);
        Sleep(500);
    }

    wchar_t targetTitle[256] = {};
    GetWindowTextW(g_target, targetTitle, 256);
    wchar_t targetClass[128] = {};
    GetClassNameW(g_target, targetClass, 128);

    // Visible frame inside the window rect, measured before any parking.
    RECT windowRect;
    RECT frame;
    GetWindowRect(g_target, &windowRect);
    if (FAILED(DwmGetWindowAttribute(g_target, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        frame = windowRect;
    }
    g_sourceRect = {frame.left - windowRect.left, frame.top - windowRect.top, frame.right - windowRect.left,
                    frame.bottom - windowRect.top};
    const int visibleWidth = frame.right - frame.left;
    const int visibleHeight = frame.bottom - frame.top;
    g_viewSize = {static_cast<LONG>(visibleWidth * scale), static_cast<LONG>(visibleHeight * scale)};

    Log(L"=== target 0x%llx \"%s\" [%s] %dx%d  strategy=%s scale=%.2f cover=%d seconds=%d",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_target)), targetTitle, targetClass,
        visibleWidth, visibleHeight, strategyName.c_str(), scale, g_cover ? 1 : 0, seconds);

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    SetUnhandledExceptionFilter(CrashFilter);

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW viewClass{};
    viewClass.lpfnWndProc = ViewProc;
    viewClass.hInstance = instance;
    viewClass.lpszClassName = L"ScaleSpikeView";
    viewClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&viewClass);
    WNDCLASSW coverClass{};
    coverClass.lpfnWndProc = CoverProc;
    coverClass.hInstance = instance;
    coverClass.lpszClassName = L"ScaleSpikeCover";
    RegisterClassW(&coverClass);

    // The view goes bottom-right of the target's monitor, topmost, so it is
    // never the thing being covered.
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(g_target, MONITOR_DEFAULTTONEAREST), &monitor);
    const int viewX = monitor.rcWork.right - g_viewSize.cx - 16;
    const int viewY = monitor.rcWork.bottom - g_viewSize.cy - 16;

    if (!Park(g_target)) {
        return 1;
    }

    g_view = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"ScaleSpikeView", L"scalespike",
                             WS_POPUP, viewX, viewY, g_viewSize.cx, g_viewSize.cy, nullptr, nullptr, instance, nullptr);

    if (g_strategy == Strategy::PrintWindow) {
        HDC screen = GetDC(nullptr);
        g_snapDc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        SetTimer(g_view, kSnapTimer, 1000, nullptr);
        TakeSnapshot();
    } else {
        if (FAILED(DwmRegisterThumbnail(g_view, g_target, &g_thumb))) {
            Log(L"DwmRegisterThumbnail FAILED");
            RestoreTarget();
            return 1;
        }
        DWM_THUMBNAIL_PROPERTIES props{};
        props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
        props.rcDestination = {0, 0, g_viewSize.cx, g_viewSize.cy};
        props.rcSource = g_sourceRect;
        props.fVisible = TRUE;
        props.opacity = 255;
        DwmUpdateThumbnailProperties(g_thumb, &props);
    }
    ShowWindow(g_view, SW_SHOWNOACTIVATE);

    if (g_cover) {
        g_coverWindow = CreateWindowExW(0, L"ScaleSpikeCover", L"scalespike cover (Esc to stop)", WS_POPUP,
                                        monitor.rcMonitor.left, monitor.rcMonitor.top,
                                        monitor.rcMonitor.right - monitor.rcMonitor.left,
                                        monitor.rcMonitor.bottom - monitor.rcMonitor.top, nullptr, nullptr, instance,
                                        nullptr);
        ShowWindow(g_coverWindow, SW_SHOW);
        SetWindowPos(g_coverWindow, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }

    g_startTick = GetTickCount64();
    SetTimer(g_view, kSampleTimer, 500, nullptr);
    SetTimer(g_view, kEndTimer, static_cast<UINT>(seconds) * 1000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_coverWindow != nullptr) {
        DestroyWindow(g_coverWindow);
    }
    if (g_thumb != nullptr) {
        DwmUnregisterThumbnail(g_thumb);
    }
    DestroyWindow(g_view);
    RestoreTarget();

    const double avgCoverage = g_samples > 0 ? g_coverageSum / g_samples : 0;
    const double changeRate = g_samples > 1 ? 100.0 * g_changed / (g_samples - 1) : 0;
    Log(L"RESULT \"%s\" strategy=%s cover=%d: samples=%d changed=%d (%.0f%% of intervals) coverage avg=%.0f%% "
        L"min=%.0f%%",
        targetTitle, strategyName.c_str(), g_cover ? 1 : 0, g_samples, g_changed, changeRate, avgCoverage,
        g_coverageMin);
    if (g_logFile != nullptr) {
        fclose(g_logFile);
    }
    return 0;
}
