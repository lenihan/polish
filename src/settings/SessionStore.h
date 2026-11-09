#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace polish {

// One window that was open, as much as it takes to open it again: the program
// and the command line it was started with (which is what makes a document
// reopen), or for a Store app its app ID.
struct SavedApp {
    std::wstring exe;
    std::wstring commandLine;
    std::wstring aumid;  // set for packaged apps, which are launched by it
    std::wstring title;
    std::wstring windowClass;

    bool operator==(const SavedApp&) const = default;
};

// What `hwnd` is, or nullopt when it cannot be reopened: its process cannot be
// opened (an elevated app) or it is Explorer's own, whose folder windows have
// no command line that names the folder.
std::optional<SavedApp> DescribeApp(HWND hwnd);

// Whether two descriptions are the same app: the app ID for packaged apps,
// otherwise the program and the window class.
bool SameApp(const SavedApp& a, const SavedApp& b);

// Opens the app again. False if it could not be started.
bool LaunchApp(const SavedApp& app);

// The whole list in one registry value, HKCU\Software\Polish\Session.
void SaveSession(const std::vector<SavedApp>& apps);
std::vector<SavedApp> LoadSession();

}  // namespace polish
