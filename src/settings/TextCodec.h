#pragma once

#include <cstdlib>
#include <string>
#include <vector>

namespace polish::text {

// Fields are tab-separated and records newline-separated, so those two (and
// the escape character) are escaped inside a field.
inline std::wstring Escape(const std::wstring& text) {
    std::wstring out;
    for (const wchar_t c : text) {
        if (c == L'\\') out += L"\\\\";
        else if (c == L'\t') out += L"\\t";
        else if (c == L'\n') out += L"\\n";
        else if (c == L'\r') out += L"\\r";
        else out += c;
    }
    return out;
}

inline std::wstring Unescape(const std::wstring& text) {
    std::wstring out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\\' && i + 1 < text.size()) {
            const wchar_t n = text[++i];
            out += n == L't' ? L'\t' : n == L'n' ? L'\n' : n == L'r' ? L'\r' : n;
        } else {
            out += text[i];
        }
    }
    return out;
}

inline std::vector<std::wstring> Split(const std::wstring& text, wchar_t separator) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (true) {
        const size_t at = text.find(separator, start);
        if (at == std::wstring::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, at - start));
        start = at + 1;
    }
}

inline unsigned long long ParseHex(const std::wstring& text) {
    return text.empty() ? 0 : wcstoull(text.c_str(), nullptr, 16);
}

inline long ParseInt(const std::wstring& text) { return text.empty() ? 0 : wcstol(text.c_str(), nullptr, 10); }

}  // namespace polish::text
