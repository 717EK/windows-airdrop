param(
    [Parameter(Mandatory=$true)][string]$BleExe,
    [string]$LogDirectory,
    [switch]$Inspect
)
$ErrorActionPreference = 'Stop'
trap { if ($awdlPublisher -and -not $awdlPublisher.HasExited) { $awdlPublisher.Kill() }; Write-Output ($_ | Out-String); Write-Output $_.ScriptStackTrace; try { Stop-Transcript | Out-Null } catch { }; exit 1 }
$awdlStopped = @()
$awdlStartedBluetooth = $false
$awdlFailures = @()
$awdlPublisher = $null
$awdlReady = $false
$awdlRecoveryClock = [Diagnostics.Stopwatch]::StartNew()
$awdlSoftMs = 0
if (-not $Inspect) {
    if (-not $LogDirectory) { throw 'A session log directory is required.' }
    Start-Transcript -LiteralPath (Join-Path $LogDirectory 'recovery.log') -Force | Out-Null
}
# Identify this user's per-user service instead of pausing another logged-in user's session.
$awdlSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$awdlCdp = Get-CimInstance Win32_Service -Filter "Name LIKE 'CDPUserSvc_%'" | Where-Object {
    if ($_.ProcessId) {
        $awdlProcess = Get-CimInstance Win32_Process -Filter "ProcessId=$($_.ProcessId)"
        $awdlOwner = Invoke-CimMethod -InputObject $awdlProcess -MethodName GetOwnerSid
        $awdlOwner.Sid -eq $awdlSid
    }
} | Select-Object -First 1
if (-not $awdlCdp) { throw 'Cannot safely identify the current user connected-device service. No services were changed.' }
$awdlSuffix = $awdlCdp.Name.Substring('CDPUserSvc_'.Length)
$awdlBluetoothName = 'BluetoothUserService_' + $awdlSuffix
if ($Inspect) { Write-Output "Current-user service: $($awdlCdp.Name); Bluetooth service: $awdlBluetoothName"; return }
try {
    foreach ($awdlName in @($awdlCdp.Name, 'CDPSvc')) {
        $awdlService = Get-Service -Name $awdlName
        if ($awdlService.Status -eq 'Running') {
            $awdlService.Stop(); $awdlStopped += $awdlName
            $awdlService.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(10))
            Write-Output "Paused $awdlName"
        }
    }
    $awdlBluetooth = Get-Service -Name $awdlBluetoothName
    if ($awdlBluetooth.Status -eq 'Stopped') {
        Start-Service -Name $awdlBluetoothName
        $awdlStartedBluetooth = $true
    }
    # Start the AirDrop advertisement and wait for Windows to grant it.
    function Start-Publisher([string]$Tag, [int]$Tenths) {
        $out = Join-Path $LogDirectory "recovery-bluetooth$Tag.stdout.log"
        $err = Join-Path $LogDirectory "recovery-bluetooth$Tag.stderr.log"
        $script:awdlPublisherLog = $out
        $script:awdlPublisher = Start-Process -FilePath $BleExe -ArgumentList 'publish-airdrop 3600' -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
        for ($i = 0; $i -lt $Tenths; $i++) {
            if ($script:awdlPublisher.HasExited) { return $false }
            if ((Test-Path -LiteralPath $out) -and ((Get-Content -LiteralPath $out -Raw) -match 'airdrop state=Started error=Success')) { return $true }
            Start-Sleep -Milliseconds 100
        }
        if (-not $script:awdlPublisher.HasExited) { $script:awdlPublisher.Kill(); $script:awdlPublisher.WaitForExit() }
        return $false
    }
    # Soft step: with the connected-device services paused, the advertising slot is often
    # free already, and Bluetooth accessories stay connected.
    $awdlCycled = $false
    $awdlReady = Start-Publisher '-soft' 50
    $awdlSoftMs = $awdlRecoveryClock.ElapsedMilliseconds
    if ($awdlReady) { Write-Output 'Bluetooth AirDrop advertising started without switching Bluetooth off.' }
    else {
        Write-Output 'Advertising still blocked with connected-device services paused; switching Bluetooth off and on.'
        & $BleExe radio-cycle
        if ($LASTEXITCODE) { throw 'Bluetooth radio cycle was refused.' }
        $awdlCycled = $true
        $awdlReady = Start-Publisher '' 150
        if ($awdlReady) { Write-Output 'Bluetooth AirDrop advertising started.' }
    }
    if (-not $awdlReady) { throw 'Bluetooth advertising did not start. Inspect recovery-bluetooth logs.' }
} finally {
    if (-not $awdlReady -and $awdlPublisher -and -not $awdlPublisher.HasExited) { $awdlPublisher.Kill() }
    if ($awdlStartedBluetooth) {
        try { Stop-Service -Name $awdlBluetoothName; (Get-Service -Name $awdlBluetoothName).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(10)); Write-Output "Restored $awdlBluetoothName to stopped" }
        catch { $awdlFailures += $_.Exception.Message }
    }
    [array]::Reverse($awdlStopped)
    foreach ($awdlName in $awdlStopped) {
        try { Start-Service -Name $awdlName; (Get-Service -Name $awdlName).WaitForStatus('Running', [TimeSpan]::FromSeconds(10)); Write-Output "Restored $awdlName" }
        catch { $awdlFailures += $_.Exception.Message }
    }
    if ($awdlFailures.Count) {
        if ($awdlPublisher -and -not $awdlPublisher.HasExited) { $awdlPublisher.Kill() }
        throw ('Service restoration failed: ' + ($awdlFailures -join '; '))
    }
}
# Restoring services can reclaim the shared advertising resource. Check the latest state,
# rather than handing the app an old Started line from before restoration.
Start-Sleep -Milliseconds 1000
$awdlLatest = Get-Content -LiteralPath $awdlPublisherLog | Select-String 'airdrop state=' | Select-Object -Last 1
if ($awdlPublisher.HasExited -or -not $awdlLatest -or $awdlLatest.Line -notmatch 'airdrop state=Started error=Success') {
    throw 'Bluetooth advertising was lost after service restoration. No publisher was handed to the app.'
}
# The app adopts this exact child and releases it on Stop. Services are already restored.
Write-Output "advertiser_pid=$($awdlPublisher.Id)"

@{ publisher_pid = $awdlPublisher.Id; services_restored = $true; radio_cycled = $awdlCycled; soft_ms = $awdlSoftMs; total_ms = $awdlRecoveryClock.ElapsedMilliseconds; publisher_ready_after_restore = $true; publisher_log = $awdlPublisherLog } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $LogDirectory 'recovery-result.json') -Encoding UTF8
Stop-Transcript | Out-Null
