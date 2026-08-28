<#
.SYNOPSIS
    Bootstraps prerequisites for building Polish: Visual Studio 2022
    (17.8+) with the C++ desktop workload, and CMake (3.28+).

.DESCRIPTION
    Safe to re-run at any time — it only installs or upgrades whatever is
    missing or out of date, via winget.
#>

[CmdletBinding()]
param(
    [string]$MinVisualStudioVersion = '17.8',
    [string]$MinCMakeVersion = '3.28.0'
)

$ErrorActionPreference = 'Stop'

function Write-Step {
    param([string]$Message)
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Write-Ok {
    param([string]$Message)
    Write-Host "    OK: $Message" -ForegroundColor Green
}

function Write-Warn2 {
    param([string]$Message)
    Write-Host "    $Message" -ForegroundColor Yellow
}

function Assert-Winget {
    Write-Step 'Checking for winget'
    $winget = Get-Command winget -ErrorAction SilentlyContinue
    if (-not $winget) {
        Write-Error (
            "winget was not found. Install/update `"App Installer`" from the " +
            "Microsoft Store, then re-run this script: " +
            "https://apps.microsoft.com/detail/9nblggh4nns1"
        )
        exit 1
    }
    Write-Ok "winget found: $($winget.Source)"
}

function Find-VsWhere {
    $candidate = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $candidate) {
        return $candidate
    }
    $onPath = Get-Command vswhere.exe -ErrorAction SilentlyContinue
    if ($onPath) {
        return $onPath.Source
    }
    return $null
}

function Get-QualifyingVisualStudioPath {
    $vswhere = Find-VsWhere
    if (-not $vswhere) {
        return $null
    }

    $minVersion = [version]$MinVisualStudioVersion
    $upperBound = "$($minVersion.Major + 1).0"

    $installPath = & $vswhere -latest `
        -version "[$MinVisualStudioVersion,$upperBound)" `
        -products '*' `
        -requires Microsoft.VisualStudio.Workload.NativeDesktop `
        -property installationPath 2>$null

    if ([string]::IsNullOrWhiteSpace($installPath)) {
        return $null
    }
    return $installPath.Trim()
}

function Get-AnyVisualStudioPath {
    $vswhere = Find-VsWhere
    if (-not $vswhere) {
        return $null
    }
    $installPath = & $vswhere -latest -products '*' -property installationPath 2>$null
    if ([string]::IsNullOrWhiteSpace($installPath)) {
        return $null
    }
    return $installPath.Trim()
}

function Install-VisualStudio {
    Write-Step 'Installing Visual Studio 2022 Community with the C++ desktop workload (this can take a while)'
    winget install --id Microsoft.VisualStudio.2022.Community --exact `
        --silent --accept-package-agreements --accept-source-agreements `
        --override '--wait --quiet --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended'
    if ($LASTEXITCODE -ne 0) {
        Write-Warn2 "winget reported exit code $LASTEXITCODE installing Visual Studio; verifying anyway."
    }
}

function Add-CppWorkloadToExistingVisualStudio {
    param([string]$InstallPath)

    Write-Step "Adding the C++ desktop workload to the Visual Studio install at $InstallPath"

    $installerExe = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vs_installer.exe'
    if (-not (Test-Path $installerExe)) {
        Write-Warn2 'vs_installer.exe not found; falling back to a fresh Visual Studio Community install.'
        Install-VisualStudio
        return
    }

    & $installerExe modify --installPath $InstallPath `
        --add Microsoft.VisualStudio.Workload.NativeDesktop `
        --includeRecommended --quiet --norestart --wait
    if ($LASTEXITCODE -ne 0) {
        Write-Warn2 "vs_installer.exe reported exit code $LASTEXITCODE; verifying anyway."
    }
}

function Assert-VisualStudio {
    Write-Step "Checking for Visual Studio $MinVisualStudioVersion+ with the C++ desktop workload"

    if (Get-QualifyingVisualStudioPath) {
        Write-Ok "Found a qualifying Visual Studio install at $(Get-QualifyingVisualStudioPath)"
        return
    }

    $existing = Get-AnyVisualStudioPath
    if ($existing) {
        Write-Warn2 "Visual Studio is installed at $existing but is missing the C++ workload or is older than $MinVisualStudioVersion."
        Add-CppWorkloadToExistingVisualStudio -InstallPath $existing
    } else {
        Install-VisualStudio
    }

    if (-not (Get-QualifyingVisualStudioPath)) {
        Write-Error "Visual Studio still does not meet requirements (>= $MinVisualStudioVersion with the C++ desktop workload). Please install/update it manually."
        exit 1
    }
    Write-Ok "Visual Studio now satisfies the requirement."
}

function Get-InstalledCMakeVersion {
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $cmake) {
        return $null
    }
    $versionOutput = & cmake --version | Select-Object -First 1
    if ($versionOutput -notmatch '(\d+\.\d+\.\d+)') {
        return $null
    }
    return [version]$Matches[1]
}

function Assert-CMake {
    Write-Step "Checking for CMake $MinCMakeVersion+"

    $minVersion = [version]$MinCMakeVersion
    $installed = Get-InstalledCMakeVersion

    if ($installed -and $installed -ge $minVersion) {
        Write-Ok "CMake $installed found"
        return
    }

    if ($installed) {
        Write-Warn2 "CMake $installed found, but $MinCMakeVersion+ is required. Upgrading via winget."
        winget upgrade --id Kitware.CMake --exact `
            --silent --accept-package-agreements --accept-source-agreements
    } else {
        Write-Step 'Installing CMake via winget'
        winget install --id Kitware.CMake --exact `
            --silent --accept-package-agreements --accept-source-agreements
    }

    # winget-installed PATH changes may not be visible in this process yet.
    $env:Path = [System.Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
                [System.Environment]::GetEnvironmentVariable('Path', 'User')

    $installed = Get-InstalledCMakeVersion
    if (-not $installed -or $installed -lt $minVersion) {
        Write-Error "CMake still doesn't meet the $MinCMakeVersion+ requirement. Open a new terminal (so PATH refreshes) and re-run this script."
        exit 1
    }
    Write-Ok "CMake $installed found"
}

Write-Host 'Polish - prerequisite setup' -ForegroundColor Magenta
Assert-Winget
Assert-VisualStudio
Assert-CMake

Write-Host ''
Write-Host 'All prerequisites satisfied. You can now build:' -ForegroundColor Green
Write-Host '  cmake -B build -S .'
Write-Host '  cmake --build build --config Release'
