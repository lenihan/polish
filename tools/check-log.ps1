<#
.SYNOPSIS
    Scans polish.log for the failure/flood markers this project's group
    and UWP-attach work is known to produce, so a human doesn't have to
    grep the log by hand after a test pass.

.PARAMETER Path
    Path to the log file. Defaults to %TEMP%\polish.log.

.EXAMPLE
    .\tools\check-log.ps1
    .\tools\check-log.ps1 -Path C:\Users\david\AppData\Local\Temp\polish.log
#>
param(
    [string]$Path = (Join-Path $env:TEMP 'polish.log')
)

if (-not (Test-Path -LiteralPath $Path)) {
    Write-Error "Log file not found: $Path"
    exit 1
}

$lines = Get-Content -LiteralPath $Path

function Count-Matching([string]$Pattern) {
    ($lines | Select-String -SimpleMatch $Pattern).Count
}

$failureMarkers = @(
    'UNCOVERED resize band',
    'overlay creation FAILED',
    'keeps refusing its slot',
    'dropping unjoinable member',
    'failed to register the clipboard listener'
)

$anyFailure = $false

Write-Host "Log: $Path ($($lines.Count) lines)"
Write-Host ''
Write-Host '--- Failure markers ---'
foreach ($marker in $failureMarkers) {
    $count = Count-Matching $marker
    $status = if ($count -gt 0) { 'FAIL'; $anyFailure = $true } else { 'ok' }
    $line = "{0,-6} {1,4}  {2}" -f $status, $count, $marker
    if ($count -gt 0) { Write-Host $line -ForegroundColor Red } else { Write-Host $line -ForegroundColor Green }
}

Write-Host ''
Write-Host '--- Floods ---'
$pulledCount = Count-Matching 'pulled member'
$floodThreshold = 50
$floodStatus = if ($pulledCount -gt $floodThreshold) { 'FAIL'; $anyFailure = $true } else { 'ok' }
$floodLine = "{0,-6} {1,4}  pulled member (threshold {2})" -f $floodStatus, $pulledCount, $floodThreshold
if ($pulledCount -gt $floodThreshold) { Write-Host $floodLine -ForegroundColor Red } else { Write-Host $floodLine -ForegroundColor Green }

Write-Host ''
Write-Host '--- Informational (expected, not a failure) ---'
$attachDidNotStick = Count-Matching 'owner did NOT stick'
Write-Host ("{0,-6} {1,4}  Attach: owner did NOT stick" -f 'info', $attachDidNotStick) -ForegroundColor Yellow

Write-Host ''
if ($anyFailure) {
    Write-Host 'RESULT: at least one failure marker found -- see above.' -ForegroundColor Red
    exit 1
} else {
    Write-Host 'RESULT: clean.' -ForegroundColor Green
    exit 0
}
