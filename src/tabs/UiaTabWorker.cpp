#include "tabs/UiaTabWorker.h"

// objbase.h first, and deliberately: UIAutomationCore.h declares its
// interfaces with the `interface` keyword, which is a macro that only
// exists once a COM header has defined it. The project builds with
// WIN32_LEAN_AND_MEAN, so windows.h alone does not, and every one of
// those declarations fails to parse without this.
#include <objbase.h>
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

}  // namespace

struct UiaTabWorker::WorkerState {
    ComPtr<IUIAutomation> uia;
    // The UIA elements behind the last enumeration's tabs, in the same
    // order as Snapshot::tabs -- this is what makes "activate tab 3" a
    // cheap, already-resolved call rather than a second tree search.
    // Worker thread only; see the class comment.
    std::vector<ComPtr<IUIAutomationElement>> elements;
    uint64_t generation = 0;
};

UiaTabWorker::UiaTabWorker(HWND notifyWindow) : notifyWindow_(notifyWindow), state_(new WorkerState()) {
    thread_ = std::thread([this] { ThreadMain(); });
}

UiaTabWorker::~UiaTabWorker() {
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

uint64_t UiaTabWorker::RequestTabs(HWND window, const TabRule& rule) {
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

void UiaTabWorker::RequestActivate(uint64_t generation, size_t index) {
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

UiaTabWorker::Snapshot UiaTabWorker::LatestSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void UiaTabWorker::ThreadMain() {
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
        if (request.kind == Request::Kind::Enumerate) {
            Enumerate(request);
        } else {
            Activate(request);
        }
    }

    // Release every COM interface on the thread that created it, before
    // CoUninitialize -- see the destructor's comment.
    state_->elements.clear();
    state_->uia.Reset();
    CoUninitialize();
}

void UiaTabWorker::Enumerate(const Request& request) {
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

void UiaTabWorker::Activate(const Request& request) {
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
