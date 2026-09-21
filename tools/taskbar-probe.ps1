<#
.SYNOPSIS
    Prints every taskbar app button and the windows each one stands for, so
    the button-to-windows mapping the taskbar features depend on can be
    checked by eye without running Polish at all.

.DESCRIPTION
    Mirrors, in PowerShell, exactly what src/windowtracking/TaskbarButtons.*
    and src/util/AppResolver.* do in C++:

      * taskbar buttons come from UI Automation, filtered on the class
        Taskbar.TaskListButtonAutomationPeer, with the AppUserModelID taken
        from AutomationId minus its literal "Appid: " prefix;
      * each visible window's AppUserModelID comes from
        IApplicationResolver::GetAppIDForWindow;
      * the two are joined on that id.

    Read-only: it hovers nothing, clicks nothing and changes nothing.

    A healthy run has every running-app button matched to at least one
    window, with the count agreeing with the count in the button's own name
    ("... - 2 running windows"). A running app whose button shows no windows
    is the failure this tool exists to catch.

.PARAMETER ShowUnmatched
    Also list windows that matched no taskbar button. Normal for tray-only
    and tool windows; interesting if a real app turns up there.

.EXAMPLE
    .\tools\taskbar-probe.ps1
    .\tools\taskbar-probe.ps1 -ShowUnmatched
#>
param(
    [switch]$ShowUnmatched
)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, WindowsBase

$native = @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class TaskbarProbe {
  [ComImport, Guid("de25675a-72de-44b4-9373-05170450c140"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  public interface IApplicationResolver {
    int GetAppIDForShortcut(IntPtr psi, out IntPtr appId);
    int GetAppIDForShortcutObject(IntPtr psl, IntPtr psi, out IntPtr appId);
    int GetAppIDForWindow(IntPtr hwnd, out IntPtr appId, out int pinningPrevented,
                          out int explicitAppId, out int embeddedShortcutValid);
    int GetAppIDForProcess(uint pid, out IntPtr appId, out int pinningPrevented,
                           out int explicitAppId, out int embeddedShortcutValid);
  }
  [DllImport("ole32.dll")] static extern int CoCreateInstance(ref Guid clsid, IntPtr outer, uint ctx, ref Guid iid, out IntPtr ppv);
  [DllImport("ole32.dll")] static extern void CoTaskMemFree(IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int m);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetWindowLongW(IntPtr h, int i);
  public delegate bool EnumProc(IntPtr h, IntPtr l);

  static IApplicationResolver _r;
  public static string Init() {
    Guid clsid = new Guid("660b90c8-73a9-4b58-8cae-355b7f55341b");
    Guid iid   = new Guid("de25675a-72de-44b4-9373-05170450c140");
    IntPtr pv;
    int hr = CoCreateInstance(ref clsid, IntPtr.Zero, 1|4, ref iid, out pv);
    if (hr != 0) return "CoCreateInstance failed 0x" + hr.ToString("X8");
    _r = (IApplicationResolver)Marshal.GetObjectForIUnknown(pv);
    return "OK";
  }
  public static string AppId(IntPtr h) {
    if (_r == null) return null;
    IntPtr s; int a, b, c;
    if (_r.GetAppIDForWindow(h, out s, out a, out b, out c) != 0 || s == IntPtr.Zero) return null;
    string v = Marshal.PtrToStringUni(s); CoTaskMemFree(s); return v;
  }
  public static string Title(IntPtr h) { var s = new StringBuilder(256); GetWindowTextW(h, s, 256); return s.ToString(); }
  public static bool IsToolWindow(IntPtr h) { return (GetWindowLongW(h, -20) & 0x80) != 0; }
}
'@
Add-Type -TypeDefinition $native -ErrorAction Stop

$init = [TaskbarProbe]::Init()
if ($init -ne 'OK') { Write-Error "App resolver unavailable: $init"; exit 1 }

# --- taskbar buttons, per taskbar (primary plus one per extra monitor) ---
$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$cond = New-Object System.Windows.Automation.PropertyCondition(
    $AE::ClassNameProperty, 'Taskbar.TaskListButtonAutomationPeer')

$taskbars = @()
$primary = [TaskbarProbe]::FindWindowExW([IntPtr]::Zero, [IntPtr]::Zero, 'Shell_TrayWnd', $null)
if ($primary -ne [IntPtr]::Zero) { $taskbars += ,@('Shell_TrayWnd', $primary) }
$secondary = [IntPtr]::Zero
while ($true) {
    $secondary = [TaskbarProbe]::FindWindowExW([IntPtr]::Zero, $secondary, 'Shell_SecondaryTrayWnd', $null)
    if ($secondary -eq [IntPtr]::Zero) { break }
    $taskbars += ,@('Shell_SecondaryTrayWnd', $secondary)
}
if ($taskbars.Count -eq 0) { Write-Error 'No taskbar found (Shell_TrayWnd missing).'; exit 1 }

$buttons = @()
foreach ($tb in $taskbars) {
    $root = $AE::FromHandle($tb[1])
    foreach ($b in $root.FindAll($TS::Descendants, $cond)) {
        $autoId = $b.Current.AutomationId
        $appId = if ($autoId.StartsWith('Appid: ')) { $autoId.Substring(7) } else { $autoId }
        $r = $b.Current.BoundingRectangle
        $buttons += [pscustomobject]@{
            Taskbar = $tb[0]
            AppId   = $appId
            Name    = $b.Current.Name
            Rect    = ('{0},{1} {2}x{3}' -f [int]$r.X, [int]$r.Y, [int]$r.Width, [int]$r.Height)
        }
    }
}

# --- visible windows, keyed by resolved AppUserModelID ---
$windows = New-Object System.Collections.ArrayList
$cb = [TaskbarProbe+EnumProc]{
    param($h, $l)
    if ([TaskbarProbe]::IsWindowVisible($h) -and -not [TaskbarProbe]::IsToolWindow($h)) {
        $t = [TaskbarProbe]::Title($h)
        if ($t.Length -gt 0) {
            [void]$windows.Add([pscustomobject]@{
                Hwnd      = ('0x{0:X}' -f [int64]$h)
                Title     = $t
                AppId     = [TaskbarProbe]::AppId($h)
                Minimized = [TaskbarProbe]::IsIconic($h)
            })
        }
    }
    return $true
}
[void][TaskbarProbe]::EnumWindows($cb, [IntPtr]::Zero)

"Taskbars found : {0}  ({1})" -f $taskbars.Count, (($taskbars | ForEach-Object { $_[0] }) -join ', ')
"Buttons        : $($buttons.Count)"
"Windows        : $($windows.Count)"
''

$problems = 0
foreach ($b in $buttons) {
    $mine = @($windows | Where-Object { $_.AppId -eq $b.AppId })

    # The button's own name says how many windows the shell thinks it has.
    # Locale-dependent, so this is a cross-check only, never the source of
    # truth -- a non-English Windows simply reports "pinned, not running".
    $claimed = $null
    if ($b.Name -match '(\d+)\s+running window') { $claimed = [int]$Matches[1] }
    elseif ($b.Name -match 'running window') { $claimed = 1 }

    if ($null -eq $claimed) {
        $status = 'pinned, not running'
    } elseif ($mine.Count -eq 0) {
        $status = 'MISMATCH - no windows matched'; $problems++
    } elseif ($claimed -ne $mine.Count) {
        $status = "MISMATCH - shell says $claimed"; $problems++
    } else {
        $status = 'ok'
    }

    "[{0}] {1}" -f $status, $b.AppId
    "    button : {0}  ({1}, {2})" -f $b.Name, $b.Taskbar, $b.Rect
    if ($mine.Count -eq 0) {
        '    windows: (none matched)'
    } else {
        foreach ($w in $mine) {
            "    window : {0} {1} {2}" -f $w.Hwnd, $(if ($w.Minimized) { '[min]' } else { '[   ]' }), $w.Title
        }
    }
    ''
}

if ($ShowUnmatched) {
    $ids = @($buttons | ForEach-Object { $_.AppId })
    $orphans = @($windows | Where-Object { $ids -notcontains $_.AppId })
    "--- windows matching no taskbar button ($($orphans.Count)) ---"
    foreach ($w in $orphans) {
        "    {0} {1}  appId={2}" -f $w.Hwnd, $w.Title, $(if ($w.AppId) { $w.AppId } else { '<none>' })
    }
    ''
}

if ($problems -gt 0) {
    "RESULT: $problems button(s) did not map cleanly."
} else {
    'RESULT: every running app button mapped to the expected windows.'
}
