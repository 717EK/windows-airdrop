param(
    [ValidateSet(6,36,44,149)][int]$Channel = 6,
    [ValidateRange(1,3600)][int]$Seconds = 300,
    [ValidateRange(0,65535)][int]$ServicePort = 0,
    [switch]$Fixed,
    [switch]$Tap,
    [switch]$Probe
)
$ErrorActionPreference = 'Stop'
$awdlRepo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$awdlDeps = (Resolve-Path (Join-Path $awdlRepo '..\deps')).Path
$awdlToolchain = Join-Path $env:USERPROFILE 'scoop\apps\mingw\current\bin'
$env:PATH = "$awdlToolchain;$env:PATH"
$awdlLog = Join-Path $awdlDeps ('run-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $awdlLog | Out-Null
$awdlPipe = '\\.\pipe\awdl-' + [guid]::NewGuid().ToString('N')
$awdlBridge = $null
try {
    $awdlBridge = Start-Process (Join-Path $awdlDeps 'devourer-bridge.exe') -ArgumentList $awdlPipe -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $awdlLog 'radio.stdout.log') -RedirectStandardError (Join-Path $awdlLog 'radio.stderr.log')
    Start-Sleep -Milliseconds 400
    $awdlStopFile = Join-Path $awdlLog "stop.request"
    $awdlArgs = @($awdlPipe, "$Channel", "$Seconds", "--stop-file", $awdlStopFile)
    if ($ServicePort) { $awdlArgs += @('--service-port', "$ServicePort") }
    if ($Tap) { $awdlArgs += @('--tap', 'awdl0') }
    if ($Fixed) { $awdlArgs += '--fixed' }
    if ($Probe) { $awdlArgs += '--probe' }
    $awdlLink = Start-Process (Join-Path $awdlRepo 'build\awdl_link.exe') -ArgumentList $awdlArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $awdlLog 'link.stdout.log') -RedirectStandardError (Join-Path $awdlLog 'link.stderr.log')
    @{radioPid=$awdlBridge.Id; linkPid=$awdlLink.Id; log=$awdlLog; stopFile=$awdlStopFile} | ConvertTo-Json | Set-Content (Join-Path $awdlDeps 'active-awdl-run.json')
    Write-Output "AWDL logs: $awdlLog"
    $awdlLink.WaitForExit()
    $awdlBridge.WaitForExit(10000) | Out-Null
    Get-Content (Join-Path $awdlLog 'link.stdout.log')
    if ($awdlLink.ExitCode) { throw "AWDL link exited with $($awdlLink.ExitCode)" }
} finally {
    if ($awdlBridge -and -not $awdlBridge.HasExited) { $awdlBridge.Kill() }
}
