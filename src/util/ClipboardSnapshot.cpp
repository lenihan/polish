#include "util/ClipboardSnapshot.h"

#include <cctype>
#include <cstring>
#include <cwchar>
#include <format>
#include <string_view>

namespace polish {

namespace {

// Largest clipboard worth keeping a copy of. A snapshot is taken on every
// clipboard change, so this is also what bounds the cost of a big image.
constexpr size_t kMaxSnapshotBytes = 48u * 1024u * 1024u;

// Opening the clipboard fails while another process holds it, and right
// after a change that is often the process that just wrote it. A few short
// retries are the documented way through; this is on the UI thread, so the
// total is kept to about 40 ms.
bool OpenClipboardWithRetry(HWND owner) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (OpenClipboard(owner)) {
            return true;
        }
        Sleep(5);
    }
    return false;
}

// Formats whose handle is not global memory, which a byte copy cannot
// hold. The private and GDI-object ranges are app-defined handles too.
bool IsHandleFormat(UINT format) {
    switch (format) {
        case CF_BITMAP:
        case CF_METAFILEPICT:
        case CF_PALETTE:
        case CF_ENHMETAFILE:
        case CF_OWNERDISPLAY:
        case CF_DSPBITMAP:
        case CF_DSPMETAFILEPICT:
        case CF_DSPENHMETAFILE:
            return true;
        default:
            break;
    }
    return format >= CF_PRIVATEFIRST && format <= CF_GDIOBJLAST;
}

bool NameIs(UINT format, const wchar_t* expected);

// Formats that are expensive to produce or hold: images, metafiles and the
// spreadsheet/OLE set. A clipboard with any of them is not snapshotted.
bool IsHeavyFormat(UINT format) {
    switch (format) {
        case CF_DIB:
        case CF_DIBV5:
            return true;
        default:
            break;
    }
    static const wchar_t* const kHeavyNames[] = {
        L"PNG",         L"GIF",          L"JFIF",         L"Biff12",        L"Biff8",
        L"Biff5",       L"Csv",          L"XML Spreadsheet", L"Link Source", L"Embed Source",
        L"Object Descriptor", L"Link Source Descriptor", L"Art::GVML ClipFormat", L"Drawing Format",
    };
    for (const wchar_t* name : kHeavyNames) {
        if (NameIs(format, name)) {
            return true;
        }
    }
    return false;
}

std::wstring FormatName(UINT format) {
    wchar_t name[64] = L"";
    if (format >= 0xC000 && GetClipboardFormatNameW(format, name, 64) > 0) {
        return name;
    }
    return std::format(L"{}", format);
}

bool NameIs(UINT format, const wchar_t* expected) {
    wchar_t name[64] = L"";
    return format >= 0xC000 && GetClipboardFormatNameW(format, name, 64) > 0 && _wcsicmp(name, expected) == 0;
}

// Content a user would call "something was copied" even with no text.
bool IsSubstantiveNonText(UINT format) {
    switch (format) {
        case CF_BITMAP:
        case CF_DIB:
        case CF_DIBV5:
        case CF_HDROP:
        case CF_ENHMETAFILE:
        case CF_METAFILEPICT:
            return true;
        default:
            break;
    }
    // "HTML Format" is deliberately not here: whether it counts depends on
    // what is in it (see IsBlankHtml), so DescribeClipboard reads it.
    return NameIs(format, L"Rich Text Format") || NameIs(format, L"PNG") ||
           NameIs(format, L"FileGroupDescriptorW") || NameIs(format, L"Shell IDList Array");
}

bool IsHtmlWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

// Elements that are content with no text in them.
bool IsMediaTag(std::string_view name) {
    static const std::string_view kMedia[] = {"img", "svg", "video", "audio", "canvas", "object", "embed", "iframe",
                                              "picture", "table", "hr"};
    for (std::string_view media : kMedia) {
        if (name == media) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool IsEmptyCopyText(std::wstring_view text) {
    return text.empty() || text == L"\r\n" || text == L"\n" || text == L"\r";
}

bool IsBlankHtml(std::string_view html) {
    // Only the fragment is the user's content; the CF_HTML header before it
    // is digits and offsets.
    constexpr std::string_view kStart = "<!--StartFragment-->";
    constexpr std::string_view kEnd = "<!--EndFragment-->";
    const size_t start = html.find(kStart);
    if (start != std::string_view::npos) {
        html.remove_prefix(start + kStart.size());
        const size_t end = html.find(kEnd);
        if (end != std::string_view::npos) {
            html = html.substr(0, end);
        }
    } else if (const size_t firstTag = html.find('<'); firstTag != std::string_view::npos) {
        html.remove_prefix(firstTag);
    }

    size_t i = 0;
    while (i < html.size()) {
        const char c = html[i];
        if (c == '<') {
            if (html.compare(i, 4, "<!--") == 0) {
                const size_t close = html.find("-->", i + 4);
                i = close == std::string_view::npos ? html.size() : close + 3;
                continue;
            }
            size_t j = i + 1;
            if (j < html.size() && html[j] == '/') {
                ++j;
            }
            std::string name;
            while (j < html.size() && std::isalnum(static_cast<unsigned char>(html[j]))) {
                name += static_cast<char>(std::tolower(static_cast<unsigned char>(html[j])));
                ++j;
            }
            if (IsMediaTag(name)) {
                return false;
            }
            const size_t close = html.find('>', j);
            i = close == std::string_view::npos ? html.size() : close + 1;
            continue;
        }
        if (c == '&') {
            // An entity: &nbsp; is a blank, anything else is a character.
            const size_t semi = html.find(';', i);
            if (semi != std::string_view::npos && semi - i <= 8 && html.substr(i, semi - i + 1) == "&nbsp;") {
                i = semi + 1;
                continue;
            }
            return false;
        }
        if (!IsHtmlWhitespace(c)) {
            return false;
        }
        ++i;
    }
    return true;
}

std::optional<ClipboardDescription> DescribeClipboard(HWND owner) {
    if (!OpenClipboardWithRetry(owner)) {
        return std::nullopt;
    }
    ClipboardDescription description;
    bool hasUnicode = false;
    bool hasAnsi = false;
    UINT htmlFormat = 0;
    for (UINT format = EnumClipboardFormats(0); format != 0; format = EnumClipboardFormats(format)) {
        if (!description.formats.empty()) {
            description.formats += L",";
        }
        description.formats += FormatName(format);
        if (format == CF_UNICODETEXT) {
            hasUnicode = true;
        } else if (format == CF_TEXT) {
            hasAnsi = true;
        } else if (NameIs(format, L"HTML Format")) {
            htmlFormat = format;
        }
        if (IsSubstantiveNonText(format)) {
            description.hasSubstantiveNonText = true;
        }
    }
    // Unicode is preferred: it is what a modern app writes, and the ANSI
    // one is then synthesized from it. The length is bounded by the block's
    // own size, not trusted to a terminator.
    if (hasUnicode) {
        if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
            const SIZE_T chars = GlobalSize(handle) / sizeof(wchar_t);
            if (const auto* text = static_cast<const wchar_t*>(GlobalLock(handle))) {
                description.textEmpty = IsEmptyCopyText(std::wstring_view(text, wcsnlen(text, chars)));
                GlobalUnlock(handle);
            }
        }
    } else if (hasAnsi) {
        if (HANDLE handle = GetClipboardData(CF_TEXT)) {
            const SIZE_T bytes = GlobalSize(handle);
            if (const auto* text = static_cast<const char*>(GlobalLock(handle))) {
                const std::string_view narrow(text, strnlen(text, bytes));
                description.textEmpty = narrow.empty() || narrow == "\r\n" || narrow == "\n" || narrow == "\r";
                GlobalUnlock(handle);
            }
        }
    }
    // HTML counts as content only if there is something in it. Chromium puts
    // one on every copy, so an empty line arrives as `<div><br></div>`.
    if (htmlFormat != 0 && !description.hasSubstantiveNonText) {
        bool blank = false;
        if (HANDLE handle = GetClipboardData(htmlFormat)) {
            const SIZE_T bytes = GlobalSize(handle);
            if (const auto* html = static_cast<const char*>(GlobalLock(handle))) {
                blank = IsBlankHtml(std::string_view(html, strnlen(html, bytes)));
                GlobalUnlock(handle);
            }
        }
        if (!blank) {
            description.hasSubstantiveNonText = true;
        }
    }
    CloseClipboard();
    return description;
}

void ClipboardSnapshot::Invalidate() {
    items_.clear();
    valid_ = false;
}

bool ClipboardSnapshot::Capture(HWND owner) {
    Invalidate();
    if (!OpenClipboardWithRetry(owner)) {
        return false;
    }
    // First, without reading anything: refuse a clipboard that is heavy or
    // not made of plain memory. Reading a format makes an app that renders
    // lazily (a spreadsheet, an image editor) produce it, and this runs on
    // every clipboard change -- so protection is deliberately limited to
    // what is cheap to hold, text and files, and a big copy is left alone
    // rather than slowed down.
    for (UINT format = EnumClipboardFormats(0); format != 0; format = EnumClipboardFormats(format)) {
        if (IsHandleFormat(format) || IsHeavyFormat(format)) {
            CloseClipboard();
            return false;
        }
    }

    bool complete = true;
    size_t total = 0;
    std::vector<Item> items;
    for (UINT format = EnumClipboardFormats(0); format != 0; format = EnumClipboardFormats(format)) {
        HANDLE handle = GetClipboardData(format);
        if (handle == nullptr) {
            complete = false;  // delayed rendering that did not come
            continue;
        }
        const SIZE_T size = GlobalSize(handle);
        if (size == 0) {
            continue;  // an empty format: nothing to keep
        }
        if (total + size > kMaxSnapshotBytes) {
            complete = false;  // too big to hold; restoring part would be worse than none
            break;
        }
        const void* data = GlobalLock(handle);
        if (data == nullptr) {
            complete = false;
            continue;
        }
        Item item;
        item.format = format;
        item.bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        GlobalUnlock(handle);
        total += size;
        items.push_back(std::move(item));
    }
    CloseClipboard();
    if (!complete || items.empty()) {
        return false;
    }
    items_ = std::move(items);
    valid_ = true;
    return true;
}

bool ClipboardSnapshot::Restore(HWND owner) const {
    if (!valid_ || items_.empty() || !OpenClipboardWithRetry(owner)) {
        return false;
    }
    EmptyClipboard();
    bool any = false;
    for (const Item& item : items_) {
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, item.bytes.size());
        if (memory == nullptr) {
            continue;
        }
        if (void* dest = GlobalLock(memory)) {
            std::memcpy(dest, item.bytes.data(), item.bytes.size());
            GlobalUnlock(memory);
            if (SetClipboardData(item.format, memory) != nullptr) {
                any = true;  // the clipboard owns the memory now
                continue;
            }
        }
        GlobalFree(memory);
    }
    CloseClipboard();
    return any;
}

}  // namespace polish
