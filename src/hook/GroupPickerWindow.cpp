#include "hook/GroupPickerWindow.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <format>

#include "util/Logging.h"
#include "windowtracking/WindowFilters.h"

namespace polish {

namespace {

constexpr wchar_t kWindowClassName[] = L"PolishGroupPickerWindow";
constexpr int kAddButtonId = 1001;
constexpr int kRemoveButtonId = 1002;
constexpr int kCreateButtonId = 1003;
constexpr int kCancelButtonId = 1004;
constexpr int kMoveUpButtonId = 1005;
constexpr int kMoveDownButtonId = 1006;

// Logical (96 DPI) layout constants -- scaled by the window's actual DPI
// in LayoutControls/before CreateWindowExW.
constexpr int kWindowWidth = 640;
constexpr int kWindowHeight = 320;
constexpr int kMargin = 12;
constexpr int kButtonHeight = 28;
constexpr int kButtonWidth = 110;
constexpr int kNameRowHeight = 24;
constexpr int kNameLabelWidth = 50;
constexpr int kListLabelHeight = 18;
constexpr int kListLabelGap = 4;
constexpr int kMidColumnWidth = 100;
constexpr int kMidButtonHeight = 26;
constexpr int kMidButtonGap = 4;
constexpr int kMoveColumnWidth = 90;

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

// A single-line EDIT control does not reliably auto-center its text
// vertically when given a client rect taller than one line -- text just
// sits at the top (confirmed live: reordering WM_SETFONT vs. layout
// didn't change it). Rather than subclass the control to pad its
// non-client area (WM_NCCALCSIZE), the simpler fix used here is to
// give the control only the height text actually needs and center
// *that* short control within its layout row -- no subclassing, no
// WM_NCPAINT interaction with the WS_EX_CLIENTEDGE border to worry
// about.
int MeasureLineHeight(HWND hwnd, HFONT font) {
    HDC hdc = GetDC(hwnd);
    HGDIOBJ oldFont = SelectObject(hdc, font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(hdc, &metrics);
    SelectObject(hdc, oldFont);
    ReleaseDC(hwnd, hdc);
    return metrics.tmHeight + metrics.tmExternalLeading;
}

BOOL CALLBACK EnumPickerCandidatesProc(HWND hwnd, LPARAM lParam) {
    if (IsCandidateWindow(hwnd) && !IsElevatedWindow(hwnd)) {
        reinterpret_cast<std::vector<HWND>*>(lParam)->push_back(hwnd);
    }
    return TRUE;
}

}  // namespace

GroupPickerWindow::GroupPickerWindow(HINSTANCE instance) : instance_(instance) {
    static bool commonControlsInitialized = false;
    if (!commonControlsInitialized) {
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_LISTVIEW_CLASSES;
        InitCommonControlsEx(&icc);
        commonControlsInitialized = true;
    }

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProcThunk;
        windowClass.hInstance = instance_;
        windowClass.lpszClassName = kWindowClassName;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
        RegisterClassExW(&windowClass);
        classRegistered = true;
    }
}

GroupPickerWindow::~GroupPickerWindow() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
}

LRESULT CALLBACK GroupPickerWindow::WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    GroupPickerWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<GroupPickerWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<GroupPickerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->HandleMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT GroupPickerWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            CreateControls(hwnd);
            PopulateLists();
            return 0;

        case WM_SIZE:
            LayoutControls();
            return 0;

        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED) {
                switch (LOWORD(wParam)) {
                    case kAddButtonId:
                        MoveSelection(activeListView_, activeWindows_, groupWindows_);
                        break;
                    case kRemoveButtonId:
                        MoveSelection(groupListView_, groupWindows_, activeWindows_);
                        break;
                    case kMoveUpButtonId:
                        MoveSelectedInGroupList(-1);
                        break;
                    case kMoveDownButtonId:
                        MoveSelectedInGroupList(1);
                        break;
                    case kCreateButtonId:
                        Commit();
                        break;
                    case kCancelButtonId:
                        result_.reset();
                        done_ = true;
                        break;
                    default:
                        break;
                }
            }
            return 0;

        case WM_NOTIFY:
            return HandleNotify(reinterpret_cast<NMHDR*>(lParam));

        case WM_MOUSEMOVE:
            if (dragging_) {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ClientToScreen(hwnd, &pt);
                UpdateDrag(pt);
            }
            return 0;

        case WM_LBUTTONUP:
            if (dragging_) {
                EndDrag();
            }
            return 0;

        case WM_CAPTURECHANGED:
            dragging_ = false;
            dragItemIndex_ = -1;
            return 0;

        case WM_CLOSE:
            result_.reset();
            done_ = true;
            return 0;

        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

LRESULT GroupPickerWindow::HandleNotify(NMHDR* header) {
    if (header == nullptr) {
        return 0;
    }
    if (header->code == LVN_BEGINDRAG && header->hwndFrom == groupListView_) {
        const auto* nmlv = reinterpret_cast<const NMLISTVIEW*>(header);
        BeginDrag(nmlv->iItem);
    } else if (header->code == LVN_ITEMACTIVATE) {
        const auto* activate = reinterpret_cast<const NMITEMACTIVATE*>(header);
        if (activate->iItem < 0) {
            return 0;
        }
        const size_t index = static_cast<size_t>(activate->iItem);
        if (header->hwndFrom == activeListView_ && index < activeWindows_.size()) {
            MoveSingle(activeWindows_, groupWindows_, index);
        } else if (header->hwndFrom == groupListView_ && index < groupWindows_.size()) {
            MoveSingle(groupWindows_, activeWindows_, index);
        }
    } else if (header->code == LVN_ITEMCHANGED &&
               (header->hwndFrom == groupListView_ || header->hwndFrom == activeListView_)) {
        // Covers selection changes RefreshListView's own call to
        // UpdateButtonStates doesn't -- the user clicking a different
        // row directly, not via Add/Remove/Move.
        const auto* changed = reinterpret_cast<const NMLISTVIEW*>(header);
        if ((changed->uChanged & LVIF_STATE) != 0) {
            UpdateButtonStates();
        }
    }
    return 0;
}

void GroupPickerWindow::CreateControls(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);

    // SS_CENTERIMAGE vertically centers a STATIC control's own text
    // within whatever rect it's given -- needed since the label spans
    // the full (taller-than-one-line) name row, to stay visually
    // aligned with the shrunk-and-centered edit box next to it (see
    // MeasureLineHeight's comment).
    nameLabel_ = CreateWindowExW(0, L"STATIC", L"Name:", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 0, 0, 0,
                                  0, hwnd, nullptr, instance_, nullptr);
    nameEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initialName_.c_str(),
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, nullptr, instance_,
                                 nullptr);

    // The list names are separate STATIC labels sitting above each list
    // (outside its border), not the list view's own built-in column
    // header -- LVS_NOCOLUMNHEADER hides that entirely, so there's no
    // redundant/awkward header row baked into the control itself.
    // "Open windows," not "Active windows" -- "active" reads as "the
    // window with focus" to a user, not "currently open," which is
    // what this list actually means.
    activeListLabel_ = CreateWindowExW(0, L"STATIC", L"Open windows", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                                        nullptr, instance_, nullptr);
    groupListLabel_ = CreateWindowExW(0, L"STATIC", L"Group", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr,
                                       instance_, nullptr);

    // LVS_SHOWSELALWAYS -- without it, a list view hides its selection
    // highlight entirely as soon as it loses keyboard focus (standard
    // Win32 default), which happens the instant Move Up/Down/Add/
    // Remove is clicked -- looked exactly like the selection was lost,
    // even though it wasn't.
    activeListView_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS, 0, 0, 0, 0, hwnd, nullptr,
        instance_, nullptr);
    ListView_SetExtendedListViewStyle(activeListView_, LVS_EX_FULLROWSELECT);

    groupListView_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS, 0, 0, 0, 0, hwnd, nullptr,
        instance_, nullptr);
    ListView_SetExtendedListViewStyle(groupListView_, LVS_EX_FULLROWSELECT);

    LVCOLUMNW activeColumn{};
    activeColumn.mask = LVCF_WIDTH;
    activeColumn.cx = Scale(200, dpi);
    ListView_InsertColumn(activeListView_, 0, &activeColumn);

    LVCOLUMNW groupColumn{};
    groupColumn.mask = LVCF_WIDTH;
    groupColumn.cx = Scale(200, dpi);
    ListView_InsertColumn(groupListView_, 0, &groupColumn);

    addButton_ = CreateWindowExW(0, L"BUTTON", L"Add >", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddButtonId)), instance_, nullptr);
    removeButton_ = CreateWindowExW(0, L"BUTTON", L"< Remove", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
                                     hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRemoveButtonId)),
                                     instance_, nullptr);

    // Drag-to-reorder within the Group list works but isn't
    // discoverable on its own -- these are the primary, always-visible
    // way to reorder (act on whatever's currently selected in the
    // Group list).
    moveUpButton_ = CreateWindowExW(0, L"BUTTON", L"Move Up", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
                                     hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMoveUpButtonId)),
                                     instance_, nullptr);
    moveDownButton_ = CreateWindowExW(0, L"BUTTON", L"Move Down", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0,
                                       0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMoveDownButtonId)),
                                       instance_, nullptr);

    createButton_ = CreateWindowExW(0, L"BUTTON", editing_ ? L"Update Group" : L"Create Group",
                                     WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCreateButtonId)), instance_,
                                     nullptr);
    cancelButton_ = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
                                     hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelButtonId)),
                                     instance_, nullptr);

    // Real sizing (LayoutControls) runs before WM_SETFONT, not after --
    // a single-line EDIT control's internal vertical-text-centering
    // offset is computed relative to its size at the time it receives
    // WM_SETFONT, not recomputed on a later resize. Every control here
    // is created at a placeholder 0x0 size, so sending WM_SETFONT before
    // the real MoveWindow left the name field's text pinned to the top
    // instead of centered.
    LayoutControls();

    HFONT dialogFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    for (HWND control : {nameLabel_, nameEdit_, activeListLabel_, groupListLabel_, activeListView_, groupListView_,
                          addButton_, removeButton_, moveUpButton_, moveDownButton_, createButton_,
                          cancelButton_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(dialogFont), TRUE);
    }
}

void GroupPickerWindow::LayoutControls() {
    if (activeListView_ == nullptr) {
        return;
    }
    // window_ isn't assigned yet the first time this runs (it's called
    // from WM_CREATE, which fires before CreateWindowExW returns) --
    // activeListView_ is already a real child window by that point, so
    // it's a safe DPI-query fallback.
    const UINT dpi = GetDpiForWindow(window_ != nullptr ? window_ : activeListView_);
    RECT client{};
    GetClientRect(GetParent(activeListView_), &client);

    const int margin = Scale(kMargin, dpi);
    const int nameRowHeight = Scale(kNameRowHeight, dpi);
    const int nameLabelWidth = Scale(kNameLabelWidth, dpi);
    const int listLabelHeight = Scale(kListLabelHeight, dpi);
    const int listLabelGap = Scale(kListLabelGap, dpi);
    const int buttonHeight = Scale(kButtonHeight, dpi);
    const int buttonWidth = Scale(kButtonWidth, dpi);
    const int midColumnWidth = Scale(kMidColumnWidth, dpi);
    const int midButtonHeight = Scale(kMidButtonHeight, dpi);
    const int midButtonGap = Scale(kMidButtonGap, dpi);
    const int moveColumnWidth = Scale(kMoveColumnWidth, dpi);

    MoveWindow(nameLabel_, margin, margin, nameLabelWidth, nameRowHeight, TRUE);

    // Shrink the edit box to one line's actual height and center that
    // within the row -- see MeasureLineHeight's comment for why (a
    // single-line EDIT given a taller rect doesn't reliably center its
    // own text).
    const int editHeight =
        std::min(nameRowHeight, MeasureLineHeight(nameEdit_, reinterpret_cast<HFONT>(GetStockObject(
                                                                  DEFAULT_GUI_FONT))) +
                                     Scale(6, dpi));
    const int editY = margin + (nameRowHeight - editHeight) / 2;
    MoveWindow(nameEdit_, margin + nameLabelWidth, editY, client.right - margin - (margin + nameLabelWidth),
               editHeight, TRUE);

    const int buttonsTop = client.bottom - margin - buttonHeight;
    const int listsBottom = buttonsTop - margin;
    const int listLabelsTop = margin + nameRowHeight + margin;
    const int listsTop = listLabelsTop + listLabelHeight + listLabelGap;

    const int listWidth =
        (client.right - 3 * margin - midColumnWidth - moveColumnWidth - 2 * margin) / 2;
    const int leftListLeft = margin;
    const int leftListRight = leftListLeft + listWidth;
    const int midLeft = leftListRight + margin;
    const int midRight = midLeft + midColumnWidth;
    const int rightListLeft = midRight + margin;
    const int rightListRight = rightListLeft + listWidth;
    const int moveColumnLeft = rightListRight + margin;

    MoveWindow(activeListLabel_, leftListLeft, listLabelsTop, listWidth, listLabelHeight, TRUE);
    MoveWindow(groupListLabel_, rightListLeft, listLabelsTop, rightListRight - rightListLeft, listLabelHeight,
               TRUE);

    MoveWindow(activeListView_, leftListLeft, listsTop, listWidth, listsBottom - listsTop, TRUE);
    MoveWindow(groupListView_, rightListLeft, listsTop, rightListRight - rightListLeft, listsBottom - listsTop,
               TRUE);

    const int midCenterY = listsTop + (listsBottom - listsTop) / 2;
    MoveWindow(addButton_, midLeft, midCenterY - midButtonHeight - midButtonGap, midColumnWidth, midButtonHeight,
               TRUE);
    MoveWindow(removeButton_, midLeft, midCenterY + midButtonGap, midColumnWidth, midButtonHeight, TRUE);

    MoveWindow(moveUpButton_, moveColumnLeft, midCenterY - midButtonHeight - midButtonGap, moveColumnWidth,
               midButtonHeight, TRUE);
    MoveWindow(moveDownButton_, moveColumnLeft, midCenterY + midButtonGap, moveColumnWidth, midButtonHeight, TRUE);

    // Standard dialog pairing: Cancel at the far right, the primary
    // action immediately to its left -- aligning Create Group to the
    // Group list instead (tried previously) stopped making sense once
    // the Move Up/Down column shifted where that list actually sits.
    MoveWindow(cancelButton_, client.right - margin - buttonWidth, buttonsTop, buttonWidth, buttonHeight, TRUE);
    MoveWindow(createButton_, client.right - 2 * margin - 2 * buttonWidth, buttonsTop, buttonWidth, buttonHeight,
               TRUE);
}

void GroupPickerWindow::PopulateLists() {
    std::vector<HWND> candidates;
    EnumWindows(EnumPickerCandidatesProc, reinterpret_cast<LPARAM>(&candidates));

    // Existing group members are always kept in the Group list even if
    // they'd normally be filtered out of "Open windows" (e.g.
    // currently minimized) -- editing membership should never silently
    // drop a member just because of a transient state at edit time.
    groupWindows_.clear();
    for (HWND hwnd : initialSelection_) {
        if (IsWindow(hwnd)) {
            groupWindows_.push_back(hwnd);
        }
    }

    activeWindows_.clear();
    for (HWND hwnd : candidates) {
        if (std::find(groupWindows_.begin(), groupWindows_.end(), hwnd) == groupWindows_.end()) {
            activeWindows_.push_back(hwnd);
        }
    }

    RefreshListView(activeListView_, activeWindows_);
    RefreshListView(groupListView_, groupWindows_);
}

void GroupPickerWindow::RefreshListView(HWND listView, const std::vector<HWND>& windows) {
    ListView_DeleteAllItems(listView);
    for (size_t i = 0; i < windows.size(); ++i) {
        wchar_t title[256] = L"";
        GetWindowTextW(windows[i], title, static_cast<int>(sizeof(title) / sizeof(title[0])));
        if (title[0] == L'\0') {
            continue;
        }
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = title;
        ListView_InsertItem(listView, &item);
    }
    ListView_SetColumnWidth(listView, 0, LVSCW_AUTOSIZE_USEHEADER);

    // A refresh (Add/Remove/Move/double-click) always clears selection
    // in whichever list it touched, unless something explicitly
    // reselects afterward -- recompute every button's state rather than
    // trusting whatever it was before.
    UpdateButtonStates();
}

void GroupPickerWindow::MoveSelection(HWND fromListView, std::vector<HWND>& from, std::vector<HWND>& to) {
    std::vector<int> selectedIndices;
    int index = -1;
    while ((index = ListView_GetNextItem(fromListView, index, LVNI_SELECTED)) != -1) {
        selectedIndices.push_back(index);
    }
    if (selectedIndices.empty()) {
        return;
    }
    std::sort(selectedIndices.begin(), selectedIndices.end());

    // Erase in descending order so earlier removals don't shift indices
    // still to be processed; collect into `moved` in that same
    // descending order, then reverse once so the append preserves the
    // original relative (ascending) order.
    std::vector<HWND> moved;
    for (auto it = selectedIndices.rbegin(); it != selectedIndices.rend(); ++it) {
        const size_t idx = static_cast<size_t>(*it);
        if (idx < from.size()) {
            moved.push_back(from[idx]);
            from.erase(from.begin() + static_cast<std::ptrdiff_t>(idx));
        }
    }
    std::reverse(moved.begin(), moved.end());
    to.insert(to.end(), moved.begin(), moved.end());

    RefreshListView(activeListView_, activeWindows_);
    RefreshListView(groupListView_, groupWindows_);
}

void GroupPickerWindow::MoveSingle(std::vector<HWND>& from, std::vector<HWND>& to, size_t index) {
    to.push_back(from[index]);
    from.erase(from.begin() + static_cast<std::ptrdiff_t>(index));
    RefreshListView(activeListView_, activeWindows_);
    RefreshListView(groupListView_, groupWindows_);
}

// Swaps the Group list's currently-selected row with its neighbor
// (`direction` -1 = up, +1 = down) -- the discoverable alternative to
// drag-to-reorder (which still works, see LVN_BEGINDRAG in
// HandleNotify). No-op if nothing is selected or the move would go out
// of range.
void GroupPickerWindow::MoveSelectedInGroupList(int direction) {
    const int index = ListView_GetNextItem(groupListView_, -1, LVNI_SELECTED);
    if (index < 0) {
        return;
    }
    const int newIndex = index + direction;
    if (newIndex < 0 || static_cast<size_t>(newIndex) >= groupWindows_.size()) {
        return;
    }
    std::swap(groupWindows_[static_cast<size_t>(index)], groupWindows_[static_cast<size_t>(newIndex)]);
    RefreshListView(groupListView_, groupWindows_);
    ListView_SetItemState(groupListView_, newIndex, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(groupListView_, newIndex, FALSE);
}

// Every button whose usefulness depends on current selection/list
// state, recomputed together: Add/Remove need something selected in
// the list they'd move *from*; Move Up/Down additionally need
// somewhere left to go; Create Group needs at least one member. All
// disabled otherwise so each button's own state communicates whether
// clicking it would do anything, rather than it being a silent no-op.
void GroupPickerWindow::UpdateButtonStates() {
    if (activeListView_ == nullptr || groupListView_ == nullptr) {
        return;
    }
    EnableWindow(addButton_, ListView_GetSelectedCount(activeListView_) > 0);
    EnableWindow(removeButton_, ListView_GetSelectedCount(groupListView_) > 0);

    const int index = ListView_GetNextItem(groupListView_, -1, LVNI_SELECTED);
    const bool hasSelection = index >= 0;
    EnableWindow(moveUpButton_, hasSelection && index > 0);
    EnableWindow(moveDownButton_,
                 hasSelection && static_cast<size_t>(index) + 1 < groupWindows_.size());

    EnableWindow(createButton_, !groupWindows_.empty());
}

void GroupPickerWindow::BeginDrag(int itemIndex) {
    if (itemIndex < 0 || static_cast<size_t>(itemIndex) >= groupWindows_.size()) {
        return;
    }
    dragging_ = true;
    dragItemIndex_ = itemIndex;
    SetCapture(window_);
}

void GroupPickerWindow::UpdateDrag(POINT screenPt) {
    POINT clientPt = screenPt;
    ScreenToClient(groupListView_, &clientPt);
    RECT listRect{};
    GetClientRect(groupListView_, &listRect);
    if (!PtInRect(&listRect, clientPt)) {
        return;  // outside the Group list -- leave the dragged row where it is
    }

    LVHITTESTINFO hitTest{};
    hitTest.pt = clientPt;
    const int hitIndex = ListView_HitTest(groupListView_, &hitTest);
    if (hitIndex < 0 || hitIndex == dragItemIndex_ || static_cast<size_t>(hitIndex) >= groupWindows_.size()) {
        return;
    }

    // Live reorder on crossing -- same pattern as GroupChromeWindow's
    // own tab drag-reorder, not a separate insert-mark line.
    const HWND moved = groupWindows_[static_cast<size_t>(dragItemIndex_)];
    groupWindows_.erase(groupWindows_.begin() + dragItemIndex_);
    groupWindows_.insert(groupWindows_.begin() + hitIndex, moved);
    dragItemIndex_ = hitIndex;
    RefreshListView(groupListView_, groupWindows_);
    ListView_SetItemState(groupListView_, hitIndex, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

void GroupPickerWindow::EndDrag() {
    ReleaseCapture();
    dragging_ = false;
    dragItemIndex_ = -1;
}

void GroupPickerWindow::Commit() {
    wchar_t nameBuffer[256] = L"";
    GetWindowTextW(nameEdit_, nameBuffer, static_cast<int>(sizeof(nameBuffer) / sizeof(nameBuffer[0])));
    std::wstring name = nameBuffer;
    if (name.empty()) {
        name = editing_ ? initialName_ : L"New Group";  // never confirm an empty name
    }
    LogDebug(std::format(L"[Polish] GroupPicker: confirmed with {} window(s), name=\"{}\"", groupWindows_.size(),
                          name));
    result_ = GroupPickerResult{groupWindows_, name};
    done_ = true;
}

std::optional<GroupPickerResult> GroupPickerWindow::ShowModal(HWND owner, const std::vector<HWND>& initialSelection,
                                                                const std::wstring& initialName, bool editing) {
    initialSelection_ = initialSelection;
    initialName_ = initialName;
    editing_ = editing;
    dragging_ = false;
    dragItemIndex_ = -1;

    const UINT dpi = GetDpiForSystem();
    const int width = Scale(kWindowWidth, dpi);
    const int height = Scale(kWindowHeight, dpi);

    RECT monitorRect{};
    HMONITOR monitor = (owner != nullptr && IsWindow(owner)) ? MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY)
                                                               : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(monitor, &monitorInfo)) {
        monitorRect = monitorInfo.rcWork;
    }
    const int x = monitorRect.left + ((monitorRect.right - monitorRect.left) - width) / 2;
    const int y = monitorRect.top + ((monitorRect.bottom - monitorRect.top) - height) / 2;

    // Same title whether creating or editing -- both are the same
    // underlying action (choose which windows belong to the group), so
    // one consistent name is used everywhere it's referenced (the
    // taskbar menu, this title, the chrome's own context menu item).
    window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClassName, L"Edit Group Windows",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner, nullptr, instance_,
                               this);
    if (window_ == nullptr) {
        return std::nullopt;
    }

    ShowWindow(window_, SW_SHOW);
    SetForegroundWindow(window_);

    done_ = false;
    result_.reset();
    MSG msg;
    while (!done_) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            break;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE &&
            (msg.hwnd == window_ || IsChild(window_, msg.hwnd))) {
            result_.reset();
            done_ = true;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyWindow(window_);
    window_ = nullptr;
    activeListLabel_ = nullptr;
    groupListLabel_ = nullptr;
    activeListView_ = nullptr;
    groupListView_ = nullptr;
    addButton_ = nullptr;
    removeButton_ = nullptr;
    moveUpButton_ = nullptr;
    moveDownButton_ = nullptr;
    nameLabel_ = nullptr;
    nameEdit_ = nullptr;
    createButton_ = nullptr;
    cancelButton_ = nullptr;
    return result_;
}

}  // namespace polish
