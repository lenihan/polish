#include "tabs/UiaWorker.h"

// objbase.h first, and deliberately: UIAutomationCore.h declares its
// interfaces with the `interface` keyword, which is a macro that only
// exists once a COM header has defined it. The project builds with
// WIN32_LEAN_AND_MEAN, so windows.h alone does not, and every one of
// those declarations fails to parse without this.
#include <objbase.h>
#include <oleacc.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <format>

#include "util/Logging.h"

using Microsoft::WRL::ComPtr;

namespace polish {
namespace {

// A BSTR returned by a UIA getter, freed on scope exit. UIA hands back
// raw BSTRs by out-param and several of the getters below would otherwise
// each need their own paired SysFreeString on every early-return path.
class ScopedBstr {
public:
    ScopedBstr() = default;
    ~ScopedBstr() { SysFreeString(value_); }
    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;

    BSTR* Receive() { return &value_; }
    std::wstring ToString() const { return value_ ? std::wstring(value_, SysStringLen(value_)) : std::wstring(); }

private:
    BSTR value_ = nullptr;
};

// A UIA element's runtime id as plain ints. Empty on failure, which
// callers treat as "cannot be identified across enumerations" rather than
// as an error -- it only costs that tab its place in the MRU order.
std::vector<int> RuntimeIdOf(IUIAutomationElement* element) {
    SAFEARRAY* array = nullptr;
    if (FAILED(element->GetRuntimeId(&array)) || array == nullptr) {
        return {};
    }
    std::vector<int> id;
    LONG lower = 0;
    LONG upper = -1;
    if (SUCCEEDED(SafeArrayGetLBound(array, 1, &lower)) && SUCCEEDED(SafeArrayGetUBound(array, 1, &upper))) {
        for (LONG i = lower; i <= upper; ++i) {
            int value = 0;
            if (SUCCEEDED(SafeArrayGetElement(array, &i, &value))) {
                id.push_back(value);
            }
        }
    }
    SafeArrayDestroy(array);
    return id;
}

// The text caret's rectangle via MSAA, in screen coordinates.
//
// Deliberately not UI Automation: its caret is a collapsed text range,
// which Chromium fills in with a fixed wrong value (see the header).
// MSAA models the caret as an object of its own, and Chromium implements
// that one -- it is what screen magnifiers follow.
//
// The caret belongs to the focused *child* window, not the top-level one,
// so GetGUIThreadInfo is used to find it. rcCaret from that same call is
// not used: apps that draw their own caret (anything Chromium or Electron)
// leave it empty, which is the case this exists to cover.
bool CaretRectViaMsaa(RECT& out) {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    HWND target = foreground;
    const DWORD thread = GetWindowThreadProcessId(foreground, nullptr);
    if (GetGUIThreadInfo(thread, &info)) {
        if (info.hwndCaret != nullptr) {
            target = info.hwndCaret;
        } else if (info.hwndFocus != nullptr) {
            target = info.hwndFocus;
        }
    }

    ComPtr<IAccessible> caret;
    if (FAILED(AccessibleObjectFromWindow(target, static_cast<DWORD>(OBJID_CARET), IID_PPV_ARGS(&caret))) ||
        !caret) {
        return false;
    }
    VARIANT self;
    VariantInit(&self);
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    LONG left = 0;
    LONG top = 0;
    LONG width = 0;
    LONG height = 0;
    const HRESULT hr = caret->accLocation(&left, &top, &width, &height, self);
    VariantClear(&self);
    if (FAILED(hr)) {
        return false;
    }
    // A zero-area caret is still a position, but one at the origin is the
    // "no caret here" answer some implementations give.
    // A zero-width caret is still a position; one at the origin is the
    // "no caret here" answer some implementations give. Explorer's address
    // bar returns S_FALSE and all zeroes -- a XAML control behind an
    // InputSiteWindowClass host has no MSAA caret to report.
    if (width < 0 || height <= 0 || (left == 0 && top == 0)) {
        return false;
    }
    out = RECT{left, top, left + width, top + height};
    return true;
}

}  // namespace

struct UiaWorker::WorkerState {
    ComPtr<IUIAutomation> uia;
    // The UIA elements behind the last enumeration's tabs, in the same
    // order as Snapshot::tabs -- this is what makes "activate tab 3" a
    // cheap, already-resolved call rather than a second tree search.
    // Worker thread only; see the class comment.
    std::vector<ComPtr<IUIAutomationElement>> elements;
    uint64_t generation = 0;
};

UiaWorker::UiaWorker(HWND notifyWindow) : notifyWindow_(notifyWindow), state_(new WorkerState()) {
    thread_ = std::thread([this] { ThreadMain(); });
}

UiaWorker::~UiaWorker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    // Safe to delete on this thread only because ThreadMain clears every
    // ComPtr it holds before returning -- so nothing here releases a COM
    // interface from a thread other than the one that created it.
    delete state_;
}

uint64_t UiaWorker::RequestTabs(HWND window, const TabRule& rule) {
    const uint64_t generation = nextGeneration_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Supersede any enumeration still waiting: only the newest one
        // can matter, and a burst of foreground changes should cost one
        // tree walk, not one per change.
        std::erase_if(queue_, [](const Request& r) { return r.kind == Request::Kind::Enumerate; });
        Request request;
        request.kind = Request::Kind::Enumerate;
        request.window = window;
        request.rule = rule;
        request.generation = generation;
        queue_.push_back(std::move(request));
    }
    wake_.notify_one();
    return generation;
}

uint64_t UiaWorker::RequestSelectionRect() {
    const uint64_t generation = nextGeneration_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Only the newest matters: an older one would answer about a
        // selection the user has already moved on from.
        std::erase_if(queue_, [](const Request& r) { return r.kind == Request::Kind::SelectionRect; });
        Request request;
        request.kind = Request::Kind::SelectionRect;
        request.generation = generation;
        queue_.push_back(std::move(request));
    }
    wake_.notify_one();
    return generation;
}

void UiaWorker::RequestActivate(uint64_t generation, size_t index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Request request;
        request.kind = Request::Kind::Activate;
        request.generation = generation;
        request.index = index;
        queue_.push_back(std::move(request));
    }
    wake_.notify_one();
}

UiaWorker::Snapshot UiaWorker::LatestSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

UiaWorker::SelectionSnapshot UiaWorker::LatestSelection() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return selectionSnapshot_;
}

void UiaWorker::ThreadMain() {
    // MTA: this thread makes only cross-process UIA calls and pumps no
    // message loop of its own, which is exactly what MTA is for. An STA
    // here would require a message pump to marshal calls through and
    // would gain nothing.
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comInit)) {
        LogDebug(std::format(L"[Polish] Tabs: CoInitializeEx failed (hr=0x{:08x}), tab switching disabled",
                             static_cast<uint32_t>(comInit)));
        return;
    }
    const HRESULT created =
        CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&state_->uia));
    if (FAILED(created) || !state_->uia) {
        LogDebug(std::format(L"[Polish] Tabs: could not create the UI Automation client (hr=0x{:08x}), "
                             L"tab switching disabled",
                             static_cast<uint32_t>(created)));
    }

    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                break;
            }
            request = std::move(queue_.front());
            queue_.erase(queue_.begin());
        }
        switch (request.kind) {
            case Request::Kind::Enumerate:
                Enumerate(request);
                break;
            case Request::Kind::Activate:
                Activate(request);
                break;
            case Request::Kind::SelectionRect:
                ResolveSelection(request);
                break;
        }
    }

    // Release every COM interface on the thread that created it, before
    // CoUninitialize -- see the destructor's comment.
    state_->elements.clear();
    state_->uia.Reset();
    CoUninitialize();
}

void UiaWorker::Enumerate(const Request& request) {
    const ULONGLONG startTick = GetTickCount64();
    std::vector<TabTarget> tabs;
    std::vector<ComPtr<IUIAutomationElement>> elements;

    IUIAutomation* uia = state_->uia.Get();
    // A condition matching one UIA control type. The VARIANT is cleared
    // by CreatePropertyCondition taking its own copy.
    const auto controlTypeCondition = [uia](CONTROLTYPEID type) {
        ComPtr<IUIAutomationCondition> condition;
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_I4;
        value.lVal = type;
        uia->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &condition);
        VariantClear(&value);
        return condition;
    };

    ComPtr<IUIAutomationElement> root;
    if (uia != nullptr && SUCCEEDED(uia->ElementFromHandle(request.window, &root)) && root) {
        const ComPtr<IUIAutomationCondition> isTabContainer = controlTypeCondition(UIA_TabControlTypeId);
        const ComPtr<IUIAutomationCondition> isTabItem = controlTypeCondition(UIA_TabItemControlTypeId);
        ComPtr<IUIAutomationElementArray> containers;
        if (isTabContainer && isTabItem &&
            SUCCEEDED(root->FindAll(TreeScope_Descendants, isTabContainer.Get(), &containers)) && containers) {
            int containerCount = 0;
            containers->get_Length(&containerCount);
            for (int i = 0; i < containerCount; ++i) {
                ComPtr<IUIAutomationElement> container;
                if (FAILED(containers->GetElement(i, &container)) || !container) {
                    continue;
                }
                ScopedBstr className;
                if (FAILED(container->get_CurrentClassName(className.Receive()))) {
                    continue;
                }
                // The allowlist's whole job -- see TabRule::containerClassNames.
                const std::wstring containerClass = className.ToString();
                if (std::find(request.rule.containerClassNames.begin(), request.rule.containerClassNames.end(),
                              containerClass) == request.rule.containerClassNames.end()) {
                    continue;
                }
                // The whole subtree, not just direct children: every XAML
                // app probed (Explorer, Terminal, Notepad) nests its tabs
                // under an intermediate ListView, and Edge deeper still.
                ComPtr<IUIAutomationElementArray> items;
                if (FAILED(container->FindAll(TreeScope_Descendants, isTabItem.Get(), &items)) || !items) {
                    continue;
                }
                int itemCount = 0;
                items->get_Length(&itemCount);
                for (int j = 0; j < itemCount; ++j) {
                    ComPtr<IUIAutomationElement> item;
                    if (FAILED(items->GetElement(j, &item)) || !item) {
                        continue;
                    }
                    TabTarget target;
                    target.runtimeId = RuntimeIdOf(item.Get());
                    // Edge reports the same tabs through two nested
                    // containers, so the same element can arrive twice.
                    // A tab with no runtime id cannot be compared, and is
                    // let through rather than dropped.
                    if (!target.runtimeId.empty() &&
                        std::any_of(tabs.begin(), tabs.end(), [&target](const TabTarget& seen) {
                            return seen.runtimeId == target.runtimeId;
                        })) {
                        continue;
                    }
                    ScopedBstr name;
                    item->get_CurrentName(name.Receive());
                    target.title = name.ToString();
                    ComPtr<IUIAutomationSelectionItemPattern> selection;
                    if (SUCCEEDED(item->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&selection))) &&
                        selection) {
                        BOOL isSelected = FALSE;
                        if (SUCCEEDED(selection->get_CurrentIsSelected(&isSelected))) {
                            target.selected = isSelected != FALSE;
                        }
                    }
                    tabs.push_back(std::move(target));
                    elements.push_back(std::move(item));
                }
            }
        }
    }

    state_->elements = std::move(elements);
    state_->generation = request.generation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.window = request.window;
        snapshot_.tabs = tabs;
        snapshot_.generation = request.generation;
        snapshot_.resolved = true;
    }
    LogDebug(std::format(L"[Polish] Tabs: enumerated {} tab(s) for hwnd={} in {}ms (generation {})", tabs.size(),
                         reinterpret_cast<void*>(request.window), GetTickCount64() - startTick, request.generation));
    PostMessageW(notifyWindow_, kTabsReadyMessage, static_cast<WPARAM>(request.generation), 0);
}

void UiaWorker::ResolveSelection(const Request& request) {
    RECT bounds{};
    bool found = false;
    RECT caret{};
    const bool caretFound = CaretRectViaMsaa(caret);
    RECT uiaCaret{};
    bool uiaCaretFound = false;

    IUIAutomation* uia = state_->uia.Get();
    ComPtr<IUIAutomationElement> focused;
    if (uia != nullptr && SUCCEEDED(uia->GetFocusedElement(&focused)) && focused) {
        // The focused element itself often is not the text. Selecting
        // inside a web page leaves focus on a link or a pane, with the
        // TextPattern living on the document above it -- confirmed live in
        // Edge. So walk up a few ancestors before giving up. Plenty of
        // focused controls are genuinely not text (a button, a list), and
        // finding nothing is an ordinary miss, not an error.
        ComPtr<IUIAutomationTextPattern> text;
        ComPtr<IUIAutomationTreeWalker> walker;
        uia->get_ControlViewWalker(&walker);
        ComPtr<IUIAutomationElement> candidate = focused;
        for (int depth = 0; depth < 5 && candidate; ++depth) {
            if (SUCCEEDED(candidate->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text))) && text) {
                break;
            }
            text.Reset();
            if (!walker) {
                break;
            }
            ComPtr<IUIAutomationElement> parent;
            if (FAILED(walker->GetParentElement(candidate.Get(), &parent)) || !parent) {
                break;
            }
            candidate = std::move(parent);
        }
        if (text) {
            ComPtr<IUIAutomationTextRangeArray> ranges;
            if (SUCCEEDED(text->GetSelection(&ranges)) && ranges) {
                int rangeCount = 0;
                ranges->get_Length(&rangeCount);
                for (int i = 0; i < rangeCount; ++i) {
                    ComPtr<IUIAutomationTextRange> range;
                    if (FAILED(ranges->GetElement(i, &range)) || !range) {
                        continue;
                    }
                    // A caret is not a selection, and its reported rect
                    // cannot be trusted. Chromium's omnibox hands back a
                    // plausible-looking 2px caret rect pinned to the
                    // control's left edge no matter where the caret really
                    // is -- confirmed live in Edge, identical coordinates
                    // across every paste, while RichEdit reported the true
                    // position and tracked it. There is no way to tell a
                    // truthful "caret at position 0" from that lie, so a
                    // collapsed range is not used at all: this feature is
                    // about aiming at what is selected, and with nothing
                    // selected the caret/cursor/window chain in
                    // AnchorPoint is both older and more reliable.
                    //
                    // Emptiness, not geometry, decides: GetText on a
                    // collapsed range returns nothing, which is exact,
                    // where "narrower than some pixel threshold" would be
                    // a guess that a one-character selection could fail.
                    ScopedBstr rangeText;
                    range->GetText(1, rangeText.Receive());
                    const bool collapsed = rangeText.ToString().empty();

                    // A collapsed range has no rectangles -- UI Automation
                    // documents GetBoundingRectangles as returning an empty
                    // array for one, and Explorer's address bar does
                    // exactly that (Edge's omnibox instead returns a rect,
                    // and a wrong one). So to find out where the caret is,
                    // give the range a character to measure: clone it and
                    // stretch it by one.
                    //
                    // Which way it stretches matters. Forward normally, and
                    // the caret is then the left edge of the character
                    // ahead of it. But with the caret at the very end of
                    // the text -- pasting at the end of a path or URL,
                    // which is the ordinary case -- there is no character
                    // ahead and the move reports nothing moved; stretching
                    // back over the preceding character instead puts the
                    // caret at that character's right edge.
                    ComPtr<IUIAutomationTextRange> measurable;
                    bool caretIsRightEdge = false;
                    if (collapsed && SUCCEEDED(range->Clone(&measurable)) && measurable) {
                        int moved = 0;
                        measurable->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Character, 1, &moved);
                        if (moved == 0) {
                            measurable->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character, -1,
                                                           &moved);
                            caretIsRightEdge = true;
                        }
                        if (moved == 0) {
                            measurable.Reset();  // empty field: nothing to measure either way
                        }
                    }
                    IUIAutomationTextRange* rectSource = measurable ? measurable.Get() : range.Get();
                    SAFEARRAY* rects = nullptr;
                    if (FAILED(rectSource->GetBoundingRectangles(&rects)) || rects == nullptr) {
                        continue;
                    }
                    // Flat array of doubles, four per rectangle: left, top,
                    // width, height. One rectangle per line of a selection
                    // that wraps, so they are unioned rather than taken
                    // individually.
                    double* values = nullptr;
                    LONG lower = 0;
                    LONG upper = -1;
                    if (SUCCEEDED(SafeArrayAccessData(rects, reinterpret_cast<void**>(&values))) &&
                        SUCCEEDED(SafeArrayGetLBound(rects, 1, &lower)) &&
                        SUCCEEDED(SafeArrayGetUBound(rects, 1, &upper))) {
                        const LONG count = upper - lower + 1;
                        for (LONG r = 0; r + 3 < count; r += 4) {
                            const LONG left = static_cast<LONG>(values[r]);
                            const LONG top = static_cast<LONG>(values[r + 1]);
                            const LONG right = left + static_cast<LONG>(values[r + 2]);
                            const LONG bottom = top + static_cast<LONG>(values[r + 3]);
                            // A caret legitimately has no width, so only
                            // a rect with no area at all is useless.
                            if (right < left || bottom <= top) {
                                continue;
                            }
                            RECT& into = collapsed ? uiaCaret : bounds;
                            bool& intoFound = collapsed ? uiaCaretFound : found;
                            if (!intoFound) {
                                into = RECT{left, top, right, bottom};
                                intoFound = true;
                            } else {
                                into.left = std::min(into.left, left);
                                into.top = std::min(into.top, top);
                                into.right = std::max(into.right, right);
                                into.bottom = std::max(into.bottom, bottom);
                            }
                        }
                        SafeArrayUnaccessData(rects);
                    }
                    SafeArrayDestroy(rects);

                    // The measured character is one wide; the caret sits on
                    // whichever of its edges the stretch came from, not in
                    // its middle.
                    if (collapsed && uiaCaretFound) {
                        if (caretIsRightEdge) {
                            uiaCaret.left = uiaCaret.right;
                        } else {
                            uiaCaret.right = uiaCaret.left;
                        }
                    }
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        selectionSnapshot_.bounds = bounds;
        selectionSnapshot_.found = found;
        selectionSnapshot_.caret = caret;
        selectionSnapshot_.caretFound = caretFound;
        selectionSnapshot_.uiaCaret = uiaCaret;
        selectionSnapshot_.uiaCaretFound = uiaCaretFound;
        selectionSnapshot_.generation = request.generation;
    }
    PostMessageW(notifyWindow_, kSelectionReadyMessage, static_cast<WPARAM>(request.generation), 0);
}

void UiaWorker::Activate(const Request& request) {
    if (request.generation != state_->generation) {
        // The cached elements have moved on since the list the user was
        // looking at was built; activating index N against a different
        // list would switch to an arbitrary tab.
        LogDebug(std::format(L"[Polish] Tabs: ignoring activate for stale generation {} (worker is at {})",
                             request.generation, state_->generation));
        return;
    }
    if (request.index >= state_->elements.size()) {
        return;
    }
    ComPtr<IUIAutomationSelectionItemPattern> selection;
    const ComPtr<IUIAutomationElement>& element = state_->elements[request.index];
    if (FAILED(element->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&selection))) || !selection) {
        LogDebug(L"[Polish] Tabs: activate failed -- tab exposes no SelectionItem pattern");
        return;
    }
    const HRESULT hr = selection->Select();
    LogDebug(std::format(L"[Polish] Tabs: activate index {} -> hr=0x{:08x}", request.index,
                         static_cast<uint32_t>(hr)));
}

}  // namespace polish
