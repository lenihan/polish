#include "settings/SessionStore.h"

#include <shellapi.h>
#include <winternl.h>

#include <filesystem>

#include "settings/TextCodec.h"
#include "util/AppIdentity.h"

namespace polish {

namespace {

using namespace polish::text;

constexpr wchar_t kKeyPath[] = L"Software\\Polish";
constexpr wchar_t kValueName[] = L"Session";
// ProcessCommandLineInformation: not in the public headers' enum, but stable
// since Windows 8.1 and readable with just PROCESS_QUERY_LIMITED_INFORMATION.
constexpr int kProcessCommandLineInformation = 60;

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

std::wstring CommandLineOf(HANDLE process) {
    using QueryFn = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static const auto query =
        reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (query == nullptr) {
        return L"";
    }
    for (ULONG size = 2048; size <= 65536; size *= 4) {
        std::vector<BYTE> buffer(size);
        ULONG needed = 0;
        const LONG status = query(process, kProcessCommandLineInformation, buffer.data(), size, &needed);
        if (status == 0) {
            const auto* text = reinterpret_cast<const UNICODE_STRING*>(buffer.data());
            return std::wstring(text->Buffer, text->Length / sizeof(wchar_t));
        }
        // STATUS_INFO_LENGTH_MISMATCH / BUFFER_OVERFLOW / BUFFER_TOO_SMALL:
        // try a bigger buffer; anything else is a real failure.
        if (status != static_cast<LONG>(0xC0000004) && status != static_cast<LONG>(0x80000005) &&
            status != static_cast<LONG>(0xC0000023)) {
            break;
        }
        if (needed > size) {
            size = needed / 4 + 1;  // loop multiplies by four
        }
    }
    return L"";
}

}  // namespace

std::optional<SavedApp> DescribeApp(HWND hwnd) {
    SavedApp app;
    if (const auto aumid = GetPackagedAppAumid(hwnd); aumid.has_value()) {
        app.aumid = *aumid;
    }
    const auto image = GetWindowProcessImagePath(hwnd);
    if (app.aumid.empty()) {
        if (!image.has_value()) {
            return std::nullopt;
        }
        if (EqualsNoCase(std::filesystem::path(*image).filename().wstring(), L"explorer.exe")) {
            return std::nullopt;
        }
        app.exe = *image;
        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId)) {
            app.commandLine = CommandLineOf(process);
            CloseHandle(process);
        }
    } else if (image.has_value()) {
        app.exe = *image;  // only for its icon in the list
    }
    wchar_t title[256] = L"";
    GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    app.title = title;
    wchar_t windowClass[256] = L"";
    GetClassNameW(hwnd, windowClass, ARRAYSIZE(windowClass));
    app.windowClass = windowClass;
    return app;
}

bool SameApp(const SavedApp& a, const SavedApp& b) {
    if (!a.aumid.empty() || !b.aumid.empty()) {
        return EqualsNoCase(a.aumid, b.aumid);
    }
    return EqualsNoCase(a.exe, b.exe) && EqualsNoCase(a.windowClass, b.windowClass);
}

bool LaunchApp(const SavedApp& app) {
    if (!app.aumid.empty()) {
        const std::wstring target = L"shell:AppsFolder\\" + app.aumid;
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr,
                                                       SW_SHOWNORMAL)) > 32;
    }
    if (app.exe.empty()) {
        return false;
    }
    std::wstring commandLine = app.commandLine.empty() ? L"\"" + app.exe + L"\"" : app.commandLine;
    const std::wstring directory = std::filesystem::path(app.exe).parent_path().wstring();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        directory.empty() ? nullptr : directory.c_str(), &startup, &info)) {
        return false;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}

void SaveSession(const std::vector<SavedApp>& apps) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKeyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return;
    }
    if (apps.empty()) {
        RegDeleteValueW(key, kValueName);
        RegCloseKey(key);
        return;
    }
    std::wstring text;
    for (const SavedApp& app : apps) {
        text += Escape(app.exe) + L"\t" + Escape(app.commandLine) + L"\t" + Escape(app.aumid) + L"\t" +
                Escape(app.title) + L"\t" + Escape(app.windowClass) + L"\n";
    }
    RegSetValueExW(key, kValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()),
                   static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

std::vector<SavedApp> LoadSession() {
    std::vector<SavedApp> apps;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return apps;
    }
    DWORD type = 0;
    DWORD size = 0;
    std::wstring text;
    if (RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_SZ &&
        size >= sizeof(wchar_t)) {
        text.resize(size / sizeof(wchar_t));
        if (RegQueryValueExW(key, kValueName, nullptr, &type, reinterpret_cast<BYTE*>(text.data()), &size) ==
            ERROR_SUCCESS) {
            text.resize(wcsnlen(text.c_str(), text.size()));
        } else {
            text.clear();
        }
    }
    RegCloseKey(key);
    for (const std::wstring& line : Split(text, L'\n')) {
        const std::vector<std::wstring> f = Split(line, L'\t');
        if (f.size() < 5) {
            continue;
        }
        SavedApp app;
        app.exe = Unescape(f[0]);
        app.commandLine = Unescape(f[1]);
        app.aumid = Unescape(f[2]);
        app.title = Unescape(f[3]);
        app.windowClass = Unescape(f[4]);
        apps.push_back(std::move(app));
    }
    return apps;
}

}  // namespace polish
