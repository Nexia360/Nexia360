#requires -version 5.1

<#
    XNA Windows 11 Runtime Installer
    --------------------------------
    Installs the common prerequisites required to RUN XNA games.

    Installs:
      - .NET Framework 3.5 Windows feature
      - .NET 9 Runtime (Microsoft.NETCore.App, runs Nexia's XNA host)
      - Microsoft XNA Framework 3.1
      - Microsoft XNA Framework 4.0 Refresh
      - DirectX End-User Runtimes (June 2010)

    Also downloads:
      - XNA Game Studio 4.0 Refresh (XNAGS40_setup.exe)

    Does NOT install:
      - Visual Studio 2010
      - Games for Windows Live
      - XNA Game Studio development tools

    Must run elevated.
#>

$ErrorActionPreference = "Stop"

# ------------------------------------------------------------
# Administrator check
# ------------------------------------------------------------

function Test-Administrator
{
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)

    return $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator
    )
}

if (-not (Test-Administrator))
{
    Write-Host "Requesting Administrator privileges..." -ForegroundColor Yellow

    Start-Process powershell.exe `
        -Verb RunAs `
        -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""

    exit
}

# ------------------------------------------------------------
# Configuration
# ------------------------------------------------------------

$InstallRoot = Join-Path $env:TEMP "XNA-Win11-Installer"
$DownloadDir = Join-Path $InstallRoot "Downloads"
$DirectXDir  = Join-Path $InstallRoot "DirectX"

New-Item -ItemType Directory -Force -Path $InstallRoot | Out-Null
New-Item -ItemType Directory -Force -Path $DownloadDir | Out-Null

# Official Microsoft downloads

$XNAGS40Url =
    "https://download.microsoft.com/download/e/c/6/ec68782d-872a-4d58-a8d3-87881995cdd4/XNAGS40_setup.exe"

$XNA40Url =
    "https://download.microsoft.com/download/5/3/a/53a804c8-ec78-43cd-a0f0-2fb4d45603d3/xnafx40_redist.msi"

$XNA31Url =
    "https://download.microsoft.com/download/5/9/1/5912526c-b950-4662-99b6-119a83e60e5c/xnafx31_redist.msi"

$DirectXUrl =
    "https://download.microsoft.com/download/8/4/a/84a35bf1-dafe-4ae8-82af-ad2ae20b6b14/directx_Jun2010_redist.exe"

$OsArchitecture = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
$DotNetArch = switch ($OsArchitecture)
{
    "ARM64" { "arm64" }
    "x86"   { "x86" }
    default { "x64" }
}

$DotNet9Url =
    "https://aka.ms/dotnet/9.0/dotnet-runtime-win-$DotNetArch.exe"


$XNAGS40Path = Join-Path $DownloadDir "XNAGS40_setup.exe"
$XNA40Path   = Join-Path $DownloadDir "xnafx40_redist.msi"
$XNA31Path   = Join-Path $DownloadDir "xnafx31_redist.msi"
$DirectXPath = Join-Path $DownloadDir "directx_Jun2010_redist.exe"
$DotNet9Path = Join-Path $DownloadDir "dotnet-runtime-9-win-$DotNetArch.exe"

# ------------------------------------------------------------
# Helpers
# ------------------------------------------------------------

function Write-Section
{
    param([string]$Text)

    Write-Host ""
    Write-Host "============================================================" `
        -ForegroundColor DarkGray
    Write-Host " $Text" -ForegroundColor Cyan
    Write-Host "============================================================" `
        -ForegroundColor DarkGray
}


function Download-File
{
    param(
        [string]$Url,
        [string]$Destination,
        [string]$Description
    )

    if (Test-Path $Destination)
    {
        Write-Host "$Description already downloaded." -ForegroundColor DarkGray
        return
    }

    Write-Host "Downloading $Description..."
    Write-Host "  $Url" -ForegroundColor DarkGray

    Invoke-WebRequest `
        -Uri $Url `
        -OutFile $Destination `
        -UseBasicParsing

    if (-not (Test-Path $Destination))
    {
        throw "Download failed: $Description"
    }
}


function Test-MicrosoftSignature
{
    param([string]$Path)

    Write-Host "Checking Microsoft digital signature..."

    $Signature = Get-AuthenticodeSignature $Path

    if ($Signature.Status -ne "Valid")
    {
        throw "Invalid digital signature on:`n$Path`nStatus: $($Signature.Status)"
    }

    if ($Signature.SignerCertificate.Subject -notmatch "Microsoft")
    {
        throw "File is signed, but not by Microsoft:`n$Path"
    }

    Write-Host "  Valid Microsoft signature." -ForegroundColor Green
}


function Install-MSI
{
    param(
        [string]$Path,
        [string]$Description
    )

    Write-Host "Installing $Description..."

    $Process = Start-Process `
        -FilePath "$env:SystemRoot\System32\msiexec.exe" `
        -ArgumentList "/i `"$Path`" /qn /norestart" `
        -Wait `
        -PassThru

    switch ($Process.ExitCode)
    {
        0
        {
            Write-Host "  Installed successfully." -ForegroundColor Green
        }

        1641
        {
            Write-Host "  Installed successfully. Reboot requested." `
                -ForegroundColor Yellow
            $script:RebootRequired = $true
        }

        3010
        {
            Write-Host "  Installed successfully. Reboot required." `
                -ForegroundColor Yellow
            $script:RebootRequired = $true
        }

        default
        {
            throw "$Description installer returned error $($Process.ExitCode)."
        }
    }
}


function Test-DotNet9
{
    $Roots = @($env:DOTNET_ROOT, (Join-Path $env:ProgramFiles "dotnet"))

    foreach ($Root in $Roots)
    {
        if ([string]::IsNullOrEmpty($Root))
        {
            continue
        }

        $Shared = Join-Path $Root "shared\Microsoft.NETCore.App"

        if ((Test-Path $Shared) -and
            (Get-ChildItem -Path $Shared -Directory -Filter "9.*" -ErrorAction SilentlyContinue))
        {
            return $true
        }
    }

    return $false
}


$RebootRequired = $false


# ------------------------------------------------------------
# 1. Download XNA Game Studio
# ------------------------------------------------------------

Write-Section "Downloading Microsoft XNA Game Studio 4.0 Refresh"

Download-File `
    $XNAGS40Url `
    $XNAGS40Path `
    "Microsoft XNA Game Studio 4.0 Refresh"

Test-MicrosoftSignature $XNAGS40Path

Write-Host ""
Write-Host "XNAGS downloaded to:" -ForegroundColor Green
Write-Host "  $XNAGS40Path"
Write-Host ""
Write-Host "The full XNAGS installer will NOT be run."
Write-Host "Windows 11 can choke on its obsolete Games for Windows Live component." `
    -ForegroundColor Yellow


# ------------------------------------------------------------
# 2. Enable .NET Framework 3.5
# ------------------------------------------------------------

Write-Section "Enabling .NET Framework 3.5"

$NetFx3 = Get-WindowsOptionalFeature `
    -Online `
    -FeatureName NetFx3

if ($NetFx3.State -eq "Enabled")
{
    Write-Host ".NET Framework 3.5 is already enabled." `
        -ForegroundColor Green
}
else
{
    Write-Host "Enabling Windows NetFx3 feature..."

    $DISM = Start-Process `
        -FilePath "$env:SystemRoot\System32\dism.exe" `
        -ArgumentList "/Online /Enable-Feature /FeatureName:NetFx3 /All /NoRestart" `
        -Wait `
        -PassThru

    if ($DISM.ExitCode -eq 3010)
    {
        $RebootRequired = $true
    }
    elseif ($DISM.ExitCode -ne 0)
    {
        throw "DISM failed enabling .NET Framework 3.5. Exit code: $($DISM.ExitCode)"
    }

    Write-Host ".NET Framework 3.5 enabled." -ForegroundColor Green
}


# ------------------------------------------------------------
# 3. .NET 9 Runtime
# ------------------------------------------------------------

Write-Section "Installing .NET 9 Runtime ($DotNetArch)"

if (Test-DotNet9)
{
    Write-Host ".NET 9 Runtime is already installed." -ForegroundColor Green
}
else
{
    Download-File `
        $DotNet9Url `
        $DotNet9Path `
        ".NET 9 Runtime ($DotNetArch)"

    Test-MicrosoftSignature $DotNet9Path

    Write-Host "Installing .NET 9 Runtime..."

    $DotNet = Start-Process `
        -FilePath $DotNet9Path `
        -ArgumentList "/install /quiet /norestart" `
        -Wait `
        -PassThru

    switch ($DotNet.ExitCode)
    {
        0
        {
            Write-Host "  Installed successfully." -ForegroundColor Green
        }

        1638
        {
            Write-Host "  A newer .NET 9 Runtime is already installed." `
                -ForegroundColor Green
        }

        { $_ -eq 1641 -or $_ -eq 3010 }
        {
            Write-Host "  Installed successfully. Reboot required." `
                -ForegroundColor Yellow
            $script:RebootRequired = $true
        }

        default
        {
            throw ".NET 9 Runtime installer returned error $($DotNet.ExitCode)."
        }
    }
}


# ------------------------------------------------------------
# 4. XNA Framework 3.1
# ------------------------------------------------------------

Write-Section "Installing XNA Framework 3.1"

Download-File `
    $XNA31Url `
    $XNA31Path `
    "Microsoft XNA Framework 3.1"

Test-MicrosoftSignature $XNA31Path

Install-MSI `
    $XNA31Path `
    "Microsoft XNA Framework 3.1"


# ------------------------------------------------------------
# 5. XNA Framework 4.0 Refresh
# ------------------------------------------------------------

Write-Section "Installing XNA Framework 4.0 Refresh"

Download-File `
    $XNA40Url `
    $XNA40Path `
    "Microsoft XNA Framework 4.0 Refresh"

Test-MicrosoftSignature $XNA40Path

Install-MSI `
    $XNA40Path `
    "Microsoft XNA Framework 4.0 Refresh"


# ------------------------------------------------------------
# 6. DirectX June 2010 legacy runtimes
# ------------------------------------------------------------

Write-Section "Installing DirectX June 2010 Legacy Runtime"

Download-File `
    $DirectXUrl `
    $DirectXPath `
    "DirectX End-User Runtimes (June 2010)"

Test-MicrosoftSignature $DirectXPath

if (Test-Path $DirectXDir)
{
    Remove-Item $DirectXDir -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $DirectXDir | Out-Null

Write-Host "Extracting legacy DirectX components..."

$Extract = Start-Process `
    -FilePath $DirectXPath `
    -ArgumentList "/Q /C /T:`"$DirectXDir`"" `
    -Wait `
    -PassThru

if ($Extract.ExitCode -ne 0)
{
    throw "DirectX extraction failed with exit code $($Extract.ExitCode)."
}

$DXSetup = Join-Path $DirectXDir "DXSETUP.exe"

if (-not (Test-Path $DXSetup))
{
    throw "DXSETUP.exe was not found after extraction."
}

Write-Host "Installing legacy DirectX components..."

$DXProcess = Start-Process `
    -FilePath $DXSetup `
    -ArgumentList "/silent" `
    -Wait `
    -PassThru

if ($DXProcess.ExitCode -ne 0)
{
    throw "DirectX setup returned error $($DXProcess.ExitCode)."
}

Write-Host "DirectX legacy runtime installed." -ForegroundColor Green


# ------------------------------------------------------------
# 7. Basic verification
# ------------------------------------------------------------

Write-Section "Verifying XNA Installation"

if (Test-DotNet9)
{
    Write-Host "[OK] .NET 9 Runtime" -ForegroundColor Green
}
else
{
    Write-Host "[--] .NET 9 Runtime" -ForegroundColor DarkYellow
}

$FrameworkLocations = @(
    "$env:ProgramFiles(x86)\Microsoft XNA\XNA Game Studio\v3.1\References\Windows\x86",
    "$env:ProgramFiles(x86)\Microsoft XNA\XNA Game Studio\v4.0\References\Windows\x86"
)

foreach ($Location in $FrameworkLocations)
{
    if (Test-Path $Location)
    {
        Write-Host "[OK] $Location" -ForegroundColor Green
    }
    else
    {
        Write-Host "[--] $Location" -ForegroundColor DarkYellow
    }
}


# Check the GAC for the primary XNA assemblies

$GAC = "$env:windir\Microsoft.NET\assembly\GAC_32"

$ExpectedAssemblies = @(
    "Microsoft.Xna.Framework",
    "Microsoft.Xna.Framework.Game"
)

foreach ($Assembly in $ExpectedAssemblies)
{
    $Path = Join-Path $GAC $Assembly

    if (Test-Path $Path)
    {
        Write-Host "[OK] GAC: $Assembly" -ForegroundColor Green
    }
    else
    {
        Write-Host "[??] GAC: $Assembly" -ForegroundColor Yellow
    }
}


# ------------------------------------------------------------
# Finish
# ------------------------------------------------------------

Write-Section "XNA Game Runtime Setup Complete"

Write-Host "Installed:"
Write-Host "  [X] .NET Framework 3.5"
Write-Host "  [X] .NET 9 Runtime"
Write-Host "  [X] XNA Framework 3.1"
Write-Host "  [X] XNA Framework 4.0 Refresh"
Write-Host "  [X] DirectX June 2010 legacy components"
Write-Host ""
Write-Host "Downloaded:"
Write-Host "  [X] XNA Game Studio 4.0 Refresh"
Write-Host ""
Write-Host "Skipped intentionally:"
Write-Host "  [ ] Games for Windows Live"
Write-Host "  [ ] Visual Studio 2010"
Write-Host "  [ ] XNA development tools"

if ($RebootRequired)
{
    Write-Host ""
    Write-Host "A reboot is recommended before running an XNA game." `
        -ForegroundColor Yellow
}
else
{
    Write-Host ""
    Write-Host "No reboot appears to be required." `
        -ForegroundColor Green
}

Write-Host ""
Write-Host "Temporary installer directory:"
Write-Host "  $InstallRoot"
Write-Host ""