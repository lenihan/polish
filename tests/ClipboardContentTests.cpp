#include "util/ClipboardSnapshot.h"

#include <doctest/doctest.h>

using namespace polish;

TEST_CASE("IsEmptyCopyText: nothing, or one bare line break") {
    CHECK(IsEmptyCopyText(L""));
    // What an editor writes for "copy the current line" on an empty line.
    CHECK(IsEmptyCopyText(L"\r\n"));
    CHECK(IsEmptyCopyText(L"\n"));
    CHECK(IsEmptyCopyText(L"\r"));
}

TEST_CASE("IsEmptyCopyText: anything a person might really have copied is not empty") {
    CHECK_FALSE(IsEmptyCopyText(L" "));
    CHECK_FALSE(IsEmptyCopyText(L"    "));  // an indent
    CHECK_FALSE(IsEmptyCopyText(L"\t"));
    CHECK_FALSE(IsEmptyCopyText(L"\r\n\r\n"));
    CHECK_FALSE(IsEmptyCopyText(L"a"));
    CHECK_FALSE(IsEmptyCopyText(L"0"));
}

TEST_CASE("IsBlankHtml: the empty-line fragment VS Code writes") {
    const char* html =
        "Version:0.9\r\nStartHTML:0000000105\r\nEndHTML:0000000398\r\nStartFragment:0000000141\r\n"
        "EndFragment:0000000362\r\n<html>\r\n<body>\r\n<!--StartFragment-->"
        "<div style=\"color: #bbbebf;background-color: #121314;font-family: 'Segoe WPC', sans-serif;"
        "white-space: pre;\"><br></div><!--EndFragment-->\r\n</body>\r\n</html>";
    CHECK(IsBlankHtml(html));
}

TEST_CASE("IsBlankHtml: text makes it content, markup alone does not") {
    CHECK(IsBlankHtml("<!--StartFragment--><div><br></div><!--EndFragment-->"));
    CHECK(IsBlankHtml("<!--StartFragment--><div> <span>\r\n</span>&nbsp;</div><!--EndFragment-->"));
    CHECK(IsBlankHtml(""));

    CHECK_FALSE(IsBlankHtml("<!--StartFragment--><div>hello</div><!--EndFragment-->"));
    CHECK_FALSE(IsBlankHtml("<!--StartFragment--><span>x</span><!--EndFragment-->"));
    // A character reference is a character.
    CHECK_FALSE(IsBlankHtml("<!--StartFragment--><p>&amp;</p><!--EndFragment-->"));
}

TEST_CASE("IsBlankHtml: embedded media is content even with no text") {
    CHECK_FALSE(IsBlankHtml("<!--StartFragment--><img src=\"a.png\"><!--EndFragment-->"));
    CHECK_FALSE(IsBlankHtml("<!--StartFragment--><IMG SRC=a.png><!--EndFragment-->"));
    CHECK_FALSE(IsBlankHtml("<!--StartFragment--><svg></svg><!--EndFragment-->"));
    CHECK_FALSE(IsBlankHtml("<!--StartFragment--><table><tr><td></td></tr></table><!--EndFragment-->"));
}

TEST_CASE("IsBlankHtml: text outside the fragment markers is not the user's content") {
    // The header's digits and the document shell are not what was copied.
    CHECK(IsBlankHtml("StartHTML:105\r\n<html><body>words<!--StartFragment--><br><!--EndFragment-->more"
                      "</body></html>"));
}

TEST_CASE("IsBlankHtml: comments are skipped") {
    CHECK(IsBlankHtml("<!--StartFragment--><!-- a comment with text --><br><!--EndFragment-->"));
}
