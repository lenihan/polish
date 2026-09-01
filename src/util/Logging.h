#pragma once

#include <windows.h>

#include <format>
#include <fstream>
#include <mutex>
#include <string>

namespace polish {

// Minimal diagnostic logger: writes to both the debugger (OutputDebugStringW,
// visible via DebugView/an attached debugger) and a plain-text file, since
// this app has no console and debugging a tray-resident background process
// otherwise requires attaching a debugger before the interesting event
// happens. Not performance-sensitive -- only called on state transitions
// (window tracked/untracked, toggle requested), never per-frame.
//
// Every line is timestamped and every process start writes a banner, so
// log lines from a previous run (e.g. before a rebuild/restart) are easy
// to tell apart from the current one when reading %TEMP%\polish.log.
inline void LogDebug(const std::wstring& message) {
    SYSTEMTIME time;
    GetLocalTime(&time);
    const std::wstring line = std::format(L"[{:02}:{:02}:{:02}.{:03}] {}", time.wHour, time.wMinute,
                                           time.wSecond, time.wMilliseconds, message);

    OutputDebugStringW((line + L"\n").c_str());

    // Converted to UTF-8 and written via a narrow ofstream, rather than
    // just wofstream'ing the wide string directly -- a plain wofstream
    // with no explicit codecvt facet uses the "C" locale's narrow
    // conversion, which can't represent non-ASCII characters (an em
    // dash, or any real-world window title with accented/non-Latin
    // text) and silently truncates the rest of that line -- including
    // its trailing newline -- the moment it hits one. Confirmed the hard
    // way via a garbled, run-together log line.
    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8Line;
    if (utf8Length > 0) {
        utf8Line.resize(static_cast<size_t>(utf8Length) - 1);  // exclude the null terminator
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, utf8Line.data(), utf8Length, nullptr, nullptr);
    }

    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);
    wchar_t tempPath[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tempPath) == 0) {
        return;
    }
    std::ofstream file(std::wstring(tempPath) + L"polish.log", std::ios::app | std::ios::binary);
    if (file) {
        file << utf8Line << "\n";
    }
}

// Call once at process startup so a fresh run is visually obvious when
// scanning the (append-only) log file.
inline void LogStartupBanner() {
    LogDebug(L"======== Polish starting ========");
}

}  // namespace polish
