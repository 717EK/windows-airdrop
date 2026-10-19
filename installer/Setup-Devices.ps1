param([ValidateSet('Inspect','Configure','Restore','Cleanup')][string]$Action = 'Inspect', [string]$Root = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path -LiteralPath $Root).Path
$stateRoot = Join-Path $env:ProgramData 'PulseDrop'
$devicePattern = '^USB\\VID_2357&PID_012D\\'
function Device {
    $found = @(Get-PnpDevice -PresentOnly | Where-Object InstanceId -Match $devicePattern)
    if ($found.Count -ne 1) { throw "Connect exactly one Archer T3U (2357:012d). Found $($found.Count)." }
    return $found[0]
}
function Service($id) { (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_Service).Data }
function RestoreInf {
    $backup = Join-Path $stateRoot 'original-driver'
    $saved = @(Get-ChildItem $backup -Filter '*.inf' -Recurse -ErrorAction SilentlyContinue | Where-Object { (Get-Content $_.FullName -Raw) -match '(?im)^Class\s*=\s*Net\s*$' })
    if ($saved.Count -eq 1) { return $saved[0].FullName }
    $inbox = Join-Path $env:WINDIR 'INF\netrtwlanu.inf'
    if ((Test-Path $inbox) -and (Get-Content $inbox -Raw) -match '(?i)USB\\VID_2357&PID_012D') { return $inbox }
    throw 'No saved or compatible inbox Wi-Fi driver is available. Keep WinUSB and install the original TP-Link Wi-Fi driver before restoring.'
}
function ForceDriver($inf) {
    Add-Type @'
using System; using System.Runtime.InteropServices;
public static class PulseUsbRestore {
 [DllImport("newdev.dll", CharSet=CharSet.Unicode, SetLastError=true)]
 public static extern bool UpdateDriverForPlugAndPlayDevices(IntPtr hwnd, string hardwareId, string inf, uint flags, out bool reboot);
}
'@
    $reboot = $false
    if (-not [PulseUsbRestore]::UpdateDriverForPlugAndPlayDevices([IntPtr]::Zero, 'USB\VID_2357&PID_012D', $inf, 1, [ref]$reboot)) { throw (New-Object ComponentModel.Win32Exception([Runtime.InteropServices.Marshal]::GetLastWin32Error())) }
    if ($reboot) { Write-Output 'Windows requested a reboot to complete the driver change.' }
}
if ($Action -eq 'Cleanup') {
    foreach ($name in @('PulseDrop TCP on awdl0','PulseDrop mDNS on awdl0')) {
        Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue | Where-Object {
            ($_ | Get-NetFirewallApplicationFilter).Program -eq (Join-Path $Root 'runtime\python.exe')
        } | Remove-NetFirewallRule
    }
    return
}
$device = Device
if ($Action -eq 'Inspect') {
    $restore = $null
    try { $restore = RestoreInf } catch { }
    @{device=$device.InstanceId; service=(Service $device.InstanceId); restore_driver=$restore; tap_present=[bool](Get-NetAdapter -Name awdl0 -ErrorAction SilentlyContinue)} | ConvertTo-Json
    return
}
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Administrator approval is required.' }
if (Get-Process PulseDrop,awdl_link,devourer-bridge -ErrorAction SilentlyContinue) { throw 'Close PulseDrop before changing its setup or USB driver.' }
New-Item -ItemType Directory $stateRoot -Force | Out-Null
& icacls $stateRoot /inheritance:r /grant:r '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-545:(OI)(CI)RX' | Out-Null
if ($LASTEXITCODE) { throw 'Cannot protect setup state.' }
Start-Transcript -Path (Join-Path $stateRoot 'setup.log') -Append | Out-Null
try {
    if ($Action -eq 'Restore') {
        if ((Service $device.InstanceId) -ne 'WinUSB') { Write-Output 'The adapter is already using a Wi-Fi driver.'; return }
        $inf = RestoreInf
        ForceDriver $inf
        if ((Service $device.InstanceId) -eq 'WinUSB') { throw 'Windows still reports WinUSB after restoration.' }
        Write-Output 'The TP-Link Wi-Fi driver is restored. PulseDrop needs WinUSB again before it can use this adapter.'
        return
    }
    $manifest = Get-Content (Join-Path $Root 'payload-sha256.json') -Raw | ConvertFrom-Json
    foreach ($item in $manifest.PSObject.Properties) {
        $path = [IO.Path]::GetFullPath((Join-Path $Root $item.Name))
        if (-not $path.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid payload path.' }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $item.Value) { throw "Payload changed: $($item.Name)" }
    }
    if (-not (Get-NetAdapter -Name awdl0 -ErrorAction SilentlyContinue)) {
        & (Join-Path $Root 'install-awdl-tap.ps1') -Install -Package (Join-Path $Root 'drivers\tap')
    }
    $tap = Get-NetAdapter -Name awdl0
    if ($tap.InterfaceDescription -notlike 'TAP-Windows Adapter V9*') { throw 'awdl0 is not the supported TAP adapter; no USB change made.' }
    & (Join-Path $Root 'configure-awdl-firewall.ps1') -PythonExe (Join-Path $Root 'runtime\python.exe') -RulePrefix PulseDrop
    if ((Service $device.InstanceId) -ne 'WinUSB') {
        $driver = Get-CimInstance Win32_PnPSignedDriver | Where-Object DeviceID -EQ $device.InstanceId
        $backup = Join-Path $stateRoot 'original-driver'
        if (Test-Path $backup) {
            $restorable = RestoreInf
            if ($driver.InfName -match '^oem\d+\.inf$' -and $restorable -notlike "$backup\*") { throw 'Previous vendor backup is incomplete; USB was not changed.' }
        } else {
            New-Item -ItemType Directory $backup | Out-Null
        if ($driver.InfName -match '^oem\d+\.inf$') {
            & pnputil.exe /export-driver $driver.InfName $backup
            if ($LASTEXITCODE -notin @(0,3010)) { throw 'The original Wi-Fi driver could not be backed up. USB was not changed.' }
        } elseif ($driver.InfName -eq 'netrtwlanu.inf') {
            [void](RestoreInf)  # Windows retains this compatible inbox driver; it cannot be exported by pnputil.
        } else { throw 'Cannot preserve this original driver; USB was not changed.' }
        }
        try {
            & (Join-Path $Root 'drivers\pulsedrop-usb.exe') --install (Join-Path $stateRoot 'winusb')
            if ($LASTEXITCODE) { throw 'WinUSB installation failed.' }
            if ((Service $device.InstanceId) -ne 'WinUSB') { throw 'Windows did not bind WinUSB.' }
        } catch {
            $failure = $_
            try { ForceDriver (RestoreInf) } catch { Write-Output "Original driver restoration also failed: $_" }
            throw $failure
        }
    }
    @{configured=$true;device=$device.InstanceId;time=(Get-Date).ToString('o')} | ConvertTo-Json | Set-Content (Join-Path $stateRoot 'setup-result.json') -Encoding UTF8
    Write-Output 'PulseDrop setup is ready.'
} finally { Stop-Transcript | Out-Null }
