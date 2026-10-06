#include "windowtracking/StackLayout.h"

#include <doctest/doctest.h>

using namespace polish;

namespace {

bool Same(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

constexpr RECT kStack = {100, 200, 1300, 900};

}  // namespace

TEST_CASE("ComputeStackFrame: horizontal puts the strip along the top, full width") {
    const StackFrame f = ComputeStackFrame(kStack, StackAlignment::Horizontal, 40);
    CHECK(Same(f.strip, RECT{100, 200, 1300, 240}));
    CHECK(Same(f.content, RECT{100, 240, 1300, 900}));
}

TEST_CASE("ComputeStackFrame: vertical puts the strip down the left, full height") {
    const StackFrame f = ComputeStackFrame(kStack, StackAlignment::Vertical, 200);
    CHECK(Same(f.strip, RECT{100, 200, 300, 900}));
    CHECK(Same(f.content, RECT{300, 200, 1300, 900}));
}

TEST_CASE("ComputeStackFrame: the strip and content tile the stack rect exactly") {
    for (StackAlignment a : {StackAlignment::Horizontal, StackAlignment::Vertical}) {
        const StackFrame f = ComputeStackFrame(kStack, a, 37);
        const long stripArea = static_cast<long>(f.strip.right - f.strip.left) * (f.strip.bottom - f.strip.top);
        const long contentArea =
            static_cast<long>(f.content.right - f.content.left) * (f.content.bottom - f.content.top);
        CHECK(stripArea + contentArea == static_cast<long>(1200) * 700);
    }
}

TEST_CASE("StackRectFromContent: the exact inverse of ComputeStackFrame's content") {
    // The invariant everything leans on: adopting a member's rect goes
    // content -> stack rect -> content, and must land where it started.
    const RECT contents[] = {{0, 0, 1440, 1824}, {7, 13, 1201, 907}, {-1920, -200, 0, 880}, {500, 400, 1300, 1000}};
    for (const RECT& content : contents) {
        for (StackAlignment a : {StackAlignment::Horizontal, StackAlignment::Vertical}) {
            for (int thickness : {0, 1, 36, 72, 200}) {
                const RECT stack = StackRectFromContent(content, a, thickness);
                CHECK(Same(ComputeStackFrame(stack, a, thickness).content, content));
            }
        }
    }
}

TEST_CASE("StackRectFromContent: grows the stack rect on the strip's side only") {
    const RECT content = {300, 400, 1300, 1000};
    CHECK(Same(StackRectFromContent(content, StackAlignment::Horizontal, 40), RECT{300, 360, 1300, 1000}));
    CHECK(Same(StackRectFromContent(content, StackAlignment::Vertical, 200), RECT{100, 400, 1300, 1000}));
}

TEST_CASE("ComputeStackFrame: a stack rect shorter than the strip is clamped, never inverted") {
    constexpr RECT tiny = {0, 0, 500, 20};
    const StackFrame f = ComputeStackFrame(tiny, StackAlignment::Horizontal, 40);
    CHECK(f.strip.bottom <= tiny.bottom);
    CHECK(f.strip.bottom > f.strip.top);
    CHECK(f.content.bottom > f.content.top);  // keeps at least a pixel
    CHECK(f.content.top == f.strip.bottom);
}

TEST_CASE("ComputeStackFrame: zero and negative thickness gives an empty strip") {
    CHECK(Same(ComputeStackFrame(kStack, StackAlignment::Horizontal, 0).content, kStack));
    CHECK(Same(ComputeStackFrame(kStack, StackAlignment::Horizontal, -5).content, kStack));
}

TEST_CASE("ComputeStackFrame: an empty stack rect gives empty parts") {
    const StackFrame f = ComputeStackFrame(RECT{10, 10, 10, 10}, StackAlignment::Horizontal, 40);
    CHECK(f.strip.right - f.strip.left == 0);
    CHECK(f.content.bottom - f.content.top == 0);
}
