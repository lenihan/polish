#include "settings/StackStore.h"

#include <algorithm>
#include <cwctype>
#include <format>

#include "settings/TextCodec.h"

namespace polish {

namespace {

using namespace polish::text;

constexpr wchar_t kKeyPath[] = L"Software\\Polish";
constexpr wchar_t kValueName[] = L"Stacks";

std::wstring ExeOf(HWND hwnd) {
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return L"";
    }
    wchar_t path[MAX_PATH];
    DWORD length = ARRAYSIZE(path);
    std::wstring result;
    if (QueryFullProcessImageNameW(process, 0, path, &length)) {
        result.assign(path, length);
    }
    CloseHandle(process);
    return result;
}

std::wstring ClassOf(HWND hwnd) {
    wchar_t name[256] = L"";
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    return name;
}

std::wstring TitleOf(HWND hwnd) {
    wchar_t title[256] = L"";
    GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    return title;
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

struct FindContext {
    const SavedMember* member;
    const std::vector<HWND>* claimed;
    HWND bestTitleMatch = nullptr;
    HWND firstMatch = nullptr;
};

BOOL CALLBACK FindProc(HWND hwnd, LPARAM lParam) {
    auto* context = reinterpret_cast<FindContext*>(lParam);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }
    if (std::find(context->claimed->begin(), context->claimed->end(), hwnd) != context->claimed->end()) {
        return TRUE;
    }
    if (!EqualsNoCase(ClassOf(hwnd), context->member->windowClass) ||
        !EqualsNoCase(ExeOf(hwnd), context->member->exe)) {
        return TRUE;
    }
    if (context->firstMatch == nullptr) {
        context->firstMatch = hwnd;
    }
    if (context->bestTitleMatch == nullptr && TitleOf(hwnd) == context->member->title) {
        context->bestTitleMatch = hwnd;
    }
    return TRUE;
}

}  // namespace

SavedMember DescribeWindow(HWND hwnd) {
    SavedMember member;
    member.hwnd = static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(hwnd));
    member.exe = ExeOf(hwnd);
    member.windowClass = ClassOf(hwnd);
    member.title = TitleOf(hwnd);
    return member;
}

void SaveStacks(const std::vector<SavedStack>& stacks) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKeyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return;
    }
    if (stacks.empty()) {
        RegDeleteValueW(key, kValueName);
        RegCloseKey(key);
        return;
    }
    std::wstring text;
    for (const SavedStack& stack : stacks) {
        text += L"S\t" + Escape(stack.name) + L"\t" + std::to_wstring(stack.alignment) + L"\t" +
                std::to_wstring(stack.rect.left) + L"\t" + std::to_wstring(stack.rect.top) + L"\t" +
                std::to_wstring(stack.rect.right) + L"\t" + std::to_wstring(stack.rect.bottom) + L"\t" +
                std::format(L"{:X}", stack.activeHwnd) + L"\n";
        for (const SavedMember& member : stack.members) {
            text += L"M\t" + std::format(L"{:X}", member.hwnd) + L"\t" + Escape(member.exe) + L"\t" +
                    Escape(member.windowClass) + L"\t" + Escape(member.title) + L"\n";
        }
    }
    RegSetValueExW(key, kValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()),
                   static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

std::vector<SavedStack> LoadStacks() {
    std::vector<SavedStack> stacks;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return stacks;
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
        if (f.size() >= 8 && f[0] == L"S") {
            SavedStack stack;
            stack.name = Unescape(f[1]);
            stack.alignment = static_cast<int>(ParseInt(f[2]));
            stack.rect = RECT{ParseInt(f[3]), ParseInt(f[4]), ParseInt(f[5]), ParseInt(f[6])};
            stack.activeHwnd = ParseHex(f[7]);
            stacks.push_back(std::move(stack));
        } else if (f.size() >= 5 && f[0] == L"M" && !stacks.empty()) {
            SavedMember member;
            member.hwnd = ParseHex(f[1]);
            member.exe = Unescape(f[2]);
            member.windowClass = Unescape(f[3]);
            member.title = Unescape(f[4]);
            stacks.back().members.push_back(std::move(member));
        }
    }
    return stacks;
}

HWND ResolveSavedMember(const SavedMember& member, const std::vector<HWND>& claimed) {
    // The same window, if it is still there and still the same kind of window
    // (a handle can be reused by an unrelated one).
    const HWND original = reinterpret_cast<HWND>(static_cast<UINT_PTR>(member.hwnd));
    if (original != nullptr && IsWindow(original) &&
        std::find(claimed.begin(), claimed.end(), original) == claimed.end() &&
        EqualsNoCase(ClassOf(original), member.windowClass) && EqualsNoCase(ExeOf(original), member.exe)) {
        return original;
    }
    FindContext context{&member, &claimed};
    EnumWindows(FindProc, reinterpret_cast<LPARAM>(&context));
    return context.bestTitleMatch != nullptr ? context.bestTitleMatch : context.firstMatch;
}

}  // namespace polish
