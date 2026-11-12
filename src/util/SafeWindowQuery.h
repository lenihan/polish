#pragma once

#include <windows.h>

namespace polish {

// Ways to ask another program's window a question without being stuck behind
// it. GetWindowText, GetWindowTextLength and a plain SendMessage all wait for
// the window's own thread to answer, and a program that has stopped pumping
// messages (OneNote, say, while it waits on a slow network) never does. Polish
// asks from threads that also run its keyboard and mouse hooks, and a blocked
// hook thread freezes typing and clicking for the whole desktop.
//
// SMTO_ABORTIFHUNG returns at once for a window Windows already knows is hung;
// the short timeout covers one that is merely slow. Either way the answer is
// "nothing", which callers already treat as an ordinary missing title or icon.
inline constexpr UINT kSafeQueryTimeoutMs = 100;

inline bool SafeSend(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, DWORD_PTR* result) {
    return SendMessageTimeoutW(hwnd, message, wParam, lParam, SMTO_ABORTIFHUNG | SMTO_NORMAL, kSafeQueryTimeoutMs,
                               result) != 0;
}

// GetWindowTextW that gives up on a hung window: the text, or "" on failure.
inline int SafeGetWindowText(HWND hwnd, wchar_t* buffer, int count) {
    if (buffer == nullptr || count <= 0) {
        return 0;
    }
    buffer[0] = L'\0';
    DWORD_PTR length = 0;
    if (!SafeSend(hwnd, WM_GETTEXT, static_cast<WPARAM>(count), reinterpret_cast<LPARAM>(buffer), &length)) {
        buffer[0] = L'\0';
        return 0;
    }
    return static_cast<int>(length);
}

// GetWindowTextLengthW that gives up on a hung window: 0 on failure.
inline int SafeGetWindowTextLength(HWND hwnd) {
    DWORD_PTR length = 0;
    return SafeSend(hwnd, WM_GETTEXTLENGTH, 0, 0, &length) ? static_cast<int>(length) : 0;
}

// WM_GETICON that gives up on a hung window: null on failure.
inline HICON SafeGetIcon(HWND hwnd, WPARAM size) {
    DWORD_PTR icon = 0;
    return SafeSend(hwnd, WM_GETICON, size, 0, &icon) ? reinterpret_cast<HICON>(icon) : nullptr;
}

}  // namespace polish
