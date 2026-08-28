#include <windows.h>

#include <format>
#include <string>

namespace {

constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\Polish_SingleInstance_Mutex";
constexpr wchar_t kMessageWindowClassName[] = L"PolishMessageWindow";

// Confirms the manifest's Per-Monitor-V2 declaration actually took effect.
// If it didn't, every screen-coordinate calculation involving other
// processes' windows will be silently wrong on non-96-DPI monitors.
void LogDpiAwareness() {
    const DPI_AWARENESS_CONTEXT context = GetThreadDpiAwarenessContext();
    const DPI_AWARENESS awareness = GetAwarenessFromDpiAwarenessContext(context);

    const wchar_t* description = L"unknown";
    switch (awareness) {
        case DPI_AWARENESS_UNAWARE:
            description = L"unaware";
            break;
        case DPI_AWARENESS_SYSTEM_AWARE:
            description = L"system-aware";
            break;
        case DPI_AWARENESS_PER_MONITOR_AWARE:
            description = L"per-monitor-aware";
            break;
        default:
            break;
    }

    OutputDebugStringW(std::format(L"[Polish] DPI awareness: {}\n", description).c_str());

    if (awareness != DPI_AWARENESS_PER_MONITOR_AWARE) {
        OutputDebugStringW(
            L"[Polish] WARNING: expected Per-Monitor-V2 DPI awareness from app.manifest; "
            L"window positioning math will be wrong on non-96-DPI monitors.\n");
    }
}

LRESULT CALLBACK MessageWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// A hidden, message-only window that will host the app's event handling
// (WinEvent hook dispatch, tray icon callback, hotkey messages) as those
// pieces come online in later phases.
HWND CreateMessageWindow(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = MessageWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kMessageWindowClassName;

    if (RegisterClassExW(&windowClass) == 0) {
        return nullptr;
    }

    return CreateWindowExW(0, kMessageWindowClassName, L"Polish", 0, 0, 0, 0, 0, HWND_MESSAGE,
                            nullptr, instance, nullptr);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE singleInstanceMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
    if (singleInstanceMutex == nullptr) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Another instance is already tray-resident; nothing more to do.
        CloseHandle(singleInstanceMutex);
        return 0;
    }

    LogDpiAwareness();

    const HWND messageWindow = CreateMessageWindow(instance);
    if (messageWindow == nullptr) {
        CloseHandle(singleInstanceMutex);
        return 1;
    }

    MSG msg;
    BOOL getMessageResult;
    while ((getMessageResult = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (getMessageResult == -1) {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CloseHandle(singleInstanceMutex);
    return static_cast<int>(msg.wParam);
}
