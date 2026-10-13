#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace polish {

// Whether copied text amounts to nothing: the empty string, or a single line
// break. The second is what an editor writes when you copy with nothing
// selected -- VS Code copies "the current line", which in an empty message
// box is one empty line, `\r\n`. Indentation and other whitespace are not
// treated as empty: copying "    " to paste an indent is something a person
// might really do, whereas copying one bare line break essentially never is.
bool IsEmptyCopyText(std::wstring_view text);

// Whether an HTML clipboard fragment carries nothing a person would call
// content: no text once tags and whitespace are gone, and no image or other
// embedded media. `<div style=...><br></div>` -- what the same empty copy
// puts in "HTML Format" -- is blank. Takes the whole "HTML Format" payload
// (the CF_HTML header and `<!--StartFragment-->` markers are handled).
bool IsBlankHtml(std::string_view html);

// What is on the clipboard right now, as far as "was that copy empty?" is
// concerned. Read under OpenClipboard, so null if the clipboard could not be
// opened (another process held it for longer than the short retry).
struct ClipboardDescription {
    // A text format is present and its content is empty -- see
    // IsEmptyCopyText.
    bool textEmpty = false;
    // Something other than text is on it that a user would count as
    // content: an image, files, RTF, or HTML with real content (see
    // IsBlankHtml). Empty text next to *these* is not an empty copy.
    bool hasSubstantiveNonText = false;
    // The formats present, for the log: "13,1,16,vscode-editor-data".
    std::wstring formats;

    // The copy produced nothing: the clipboard was overwritten with an empty
    // string and no other content.
    bool IsEmptyCopy() const { return textEmpty && !hasSubstantiveNonText; }
};

std::optional<ClipboardDescription> DescribeClipboard(HWND owner);

// A copy of the clipboard's contents, so a copy that wrote nothing can be
// undone.
//
// Polish cannot stop an app from replacing the clipboard with an empty
// string -- by the time it hears about it, the old content is gone -- so it
// keeps the previous content and puts it back. Only formats that live in
// global memory are kept; GDI-handle formats (CF_BITMAP and friends) are
// either synthesized from a kept format (a bitmap from its DIB) or make the
// snapshot incomplete, in which case nothing is restored: putting back
// *part* of what was there is worse than leaving the empty string.
class ClipboardSnapshot {
public:
    // Reads the clipboard now. Returns true if the snapshot is complete and
    // restorable; false leaves it invalid.
    bool Capture(HWND owner);

    // Writes the snapshot back, replacing whatever is there. False if there
    // is nothing valid to write or the clipboard could not be opened.
    bool Restore(HWND owner) const;

    bool Valid() const { return valid_; }
    void Invalidate();

private:
    struct Item {
        UINT format = 0;
        std::vector<uint8_t> bytes;
    };
    std::vector<Item> items_;
    bool valid_ = false;
};

}  // namespace polish
