param([string]$Destination = (Join-Path $PSScriptRoot '..\..\PulseIt'), [switch]$NoShortcuts)
$ErrorActionPreference = 'Stop'
# PulseIt runs elevated, so the bundle and its Python runtime are locked to administrators (see the end).
$awdlAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $awdlAdmin) { throw 'Run packaging as administrator: the bundle is locked against changes by non-admin processes.' }
$awdlRepo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$awdlDeps = (Resolve-Path (Join-Path $awdlRepo '..\deps')).Path
$awdlPython = Join-Path $awdlDeps 'airdrop-venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $awdlPython)) { throw 'Create the tested deps/airdrop-venv first.' }
foreach ($awdlName in @('PulseIt','awdl_link','devourer-bridge')) {
    if (Get-Process -Name $awdlName -ErrorAction SilentlyContinue) { throw "Close $awdlName before packaging; no running app will be overwritten." }
}
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
dotnet publish (Join-Path $awdlRepo 'desktop\PulseIt.csproj') -c Release --self-contained false -o $Destination --verbosity quiet
if ($LASTEXITCODE) { throw 'Desktop build failed.' }
$awdlEngine = Join-Path $Destination 'engine'
New-Item -ItemType Directory -Path $awdlEngine -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $awdlRepo 'build\awdl_link.exe') -Destination $awdlEngine
Copy-Item -LiteralPath (Join-Path $awdlDeps 'devourer-bridge.exe') -Destination $awdlEngine
$awdlMingw = Join-Path $env:USERPROFILE 'scoop\apps\mingw\current\bin'
foreach ($awdlDll in @('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $awdlMingw $awdlDll) -Destination $awdlEngine
}
dotnet publish (Join-Path $awdlRepo 'tools\bleprobe\bleprobe.csproj') -c Release --self-contained false -o (Join-Path $awdlEngine 'bleprobe') --verbosity quiet
if ($LASTEXITCODE) { throw 'Bluetooth helper build failed.' }
$awdlScripts = Join-Path $Destination 'airdrop'
New-Item -ItemType Directory -Path $awdlScripts -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $awdlRepo 'airdrop') -File | Copy-Item -Destination $awdlScripts
Copy-Item -LiteralPath (Join-Path $awdlRepo 'desktop\RecoverBluetooth.ps1') -Destination $Destination
Copy-Item -LiteralPath (Join-Path $awdlRepo 'assets') -Destination $Destination -Recurse -Force
Copy-Item -LiteralPath (Join-Path $awdlRepo 'QUICK-START.html') -Destination $Destination -Force
$awdlLicenses = Join-Path $Destination 'licenses'
New-Item -ItemType Directory -Path $awdlLicenses -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $awdlRepo 'build\_deps\owl-src\COPYING') -Destination (Join-Path $awdlLicenses 'OWL-GPL-3.txt')
Copy-Item -LiteralPath (Join-Path $awdlDeps 'devourer\LICENSE') -Destination (Join-Path $awdlLicenses 'devourer-GPL-2.txt')
foreach ($awdlLicenseName in @('COPYING3','COPYING3.LIB','COPYING.RUNTIME')) {
    Copy-Item -LiteralPath (Join-Path $awdlMingw "..\licenses\gcc\$awdlLicenseName") -Destination (Join-Path $awdlLicenses "GCC-$awdlLicenseName.txt")
}
Copy-Item -LiteralPath (Join-Path $awdlMingw '..\licenses\winpthreads\COPYING') -Destination (Join-Path $awdlLicenses 'winpthreads.txt')
@{ PythonExe = $awdlPython } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Destination 'settings.json') -Encoding UTF8
@"
PulseIt - quick local setup

1. Plug in the tested TP-Link Archer T3U (2357:012D, already using WinUSB).
2. Open PulseIt.exe and accept the Windows administrator prompt.
3. Click Start receiving. On iPhone choose Share > AirDrop > Windows PC.
4. Approve incoming files on this PC. Received files opens your Downloads folder.
5. To send: unlock iPhone, refresh Everyone for 10 Minutes, Find devices,
   Choose file, Send to device. Accept on the phone.
6. If discovery cannot wake the phone, Bluetooth recovery is an explicit,
   temporary operation that disconnects Bluetooth accessories and restores services.

This local bundle uses the existing TAP driver, scoped firewall rules,
Python runtime at $awdlPython, and .NET Desktop Runtime 8 already installed
on this PC. It is not a portable installer for another PC. Keep the Python
runtime and this folder in place. Sessions last up to one hour; Stop or close
the window to release the adapter. Maximum one file of 1 GiB per send.

No driver rebinding, driver installation, or firewall changes are performed.
The native link and devourer bridge stay separate executables. Dependencies
and source workspace: $awdlRepo
This is a local development bundle; public distribution still needs the
corresponding source/license packaging and signing review.

Remove the desktop/Start menu shortcuts and this folder to remove the UI.
That leaves existing drivers, received files and logs untouched.
"@ | Set-Content -LiteralPath (Join-Path $Destination 'READ-ME.txt') -Encoding UTF8
if (-not $NoShortcuts) {
    $awdlShell = New-Object -ComObject WScript.Shell
    foreach ($awdlShortcutPath in @(
        (Join-Path ([Environment]::GetFolderPath('Desktop')) 'PulseIt.lnk'),
        (Join-Path ([Environment]::GetFolderPath('Programs')) 'PulseIt.lnk')
    )) {
        $awdlShortcut = $awdlShell.CreateShortcut($awdlShortcutPath)
        $awdlShortcut.TargetPath = Join-Path $Destination 'PulseIt.exe'
        $awdlShortcut.WorkingDirectory = $Destination
        $awdlShortcut.Description = 'Native iPhone AirDrop with the TP-Link adapter'
        $awdlShortcut.Save()
    }
}
# Anything the elevated app executes must not be writable by ordinary user processes,
# or they could change it and gain administrator rights on the next Start.
foreach ($awdlLocked in @($Destination, (Split-Path (Split-Path $awdlPython)))) {
    icacls $awdlLocked /setowner '*S-1-5-32-544' /T /C /Q | Out-Null
    icacls $awdlLocked /inheritance:r /grant:r '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-545:(OI)(CI)RX' /C /Q | Out-Null
    if ($LASTEXITCODE) { throw "Could not lock $awdlLocked" }
    icacls (Join-Path $awdlLocked '*') /reset /T /C /Q | Out-Null
}
Write-Output "Ready: $Destination\PulseIt.exe"
