param([string]$Scenario, [string]$RecoveryScript, [string]$Root)
# Isolated command mocks: this harness never invokes a radio executable or Windows service mutation.
$ErrorActionPreference = 'Stop'
$global:pulseHarnessRoot = $Root
$global:pulseScenario = $Scenario
$global:pulseStates = @{ CDPUserSvc_unit='Running'; CDPSvc='Running'; BluetoothUserService_unit='Stopped' }
$global:pulseCycles = 0; $global:pulseStarts = 0; $global:pulseKilled = 0; $global:pulseLastLog = ''
function Save-HarnessState {
    @{ states=$global:pulseStates; cycles=$global:pulseCycles; publishers=$global:pulseStarts; killed=$global:pulseKilled } | ConvertTo-Json | Set-Content (Join-Path $global:pulseHarnessRoot 'harness-state.json')
}
function Start-Transcript { param($LiteralPath, [switch]$Force) }
function Stop-Transcript { }
function Start-Sleep { param($Milliseconds) }
function Get-CimInstance { param($ClassName,$Filter) [pscustomobject]@{Name='CDPUserSvc_unit';ProcessId=123} }
function Invoke-CimMethod { param($InputObject,$MethodName) [pscustomobject]@{Sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value} }
function Get-Service {
    param($Name)
    if (-not $global:pulseStates.ContainsKey($Name)) { throw 'Unexpected service access' }
    $service = [pscustomobject]@{Name=$Name;Status=$global:pulseStates[$Name]}
    $service | Add-Member ScriptMethod Stop { $global:pulseStates[$this.Name]='Stopped'; $this.Status='Stopped'; Save-HarnessState }
    $service | Add-Member ScriptMethod WaitForStatus { param($Desired,$Timeout) if($global:pulseStates[$this.Name] -ne [string]$Desired){throw 'Service did not reach expected state'} }
    return $service
}
function Start-Service {
    param($Name)
    if (-not $global:pulseStates.ContainsKey($Name)) { throw 'Unexpected service mutation' }
    if ($global:pulseScenario -eq 'restore-fails' -and $Name -eq 'CDPSvc') { throw 'Injected restoration failure' }
    $global:pulseStates[$Name]='Running'
    if ($global:pulseScenario -eq 'lost-after-restore' -and $Name -eq 'CDPSvc') { Add-Content $global:pulseLastLog 'airdrop state=Aborted error=ResourceInUse' }
    Save-HarnessState
}
function Stop-Service { param($Name) if(-not $global:pulseStates.ContainsKey($Name)){throw 'Unexpected service mutation'}; $global:pulseStates[$Name]='Stopped'; Save-HarnessState }
function PulseItFakeRadio { param($Action) if($Action -ne 'radio-cycle'){throw 'Unexpected radio operation'}; $global:pulseCycles++; $global:LASTEXITCODE=0; Save-HarnessState }
function Start-Process {
    param($FilePath,$ArgumentList,$WindowStyle,[switch]$PassThru,$RedirectStandardOutput,$RedirectStandardError)
    if ($FilePath -ne 'PulseItFakeRadio') { throw 'Refused real process launch in recovery test' }
    $global:pulseStarts++; $global:pulseLastLog=$RedirectStandardOutput
    $ready = $global:pulseScenario -ne 'never-starts' -and ($global:pulseScenario -ne 'fallback' -or $global:pulseStarts -gt 1)
    Set-Content $RedirectStandardOutput $(if($ready){'airdrop state=Started error=Success'}else{'airdrop state=Waiting error=ResourceInUse'})
    Set-Content $RedirectStandardError ''
    $process=[pscustomobject]@{Id=(1000+$global:pulseStarts);HasExited=$false}
    $process | Add-Member ScriptMethod Kill { $this.HasExited=$true; $global:pulseKilled++; Save-HarnessState }
    $process | Add-Member ScriptMethod WaitForExit { }
    Save-HarnessState
    return $process
}
Save-HarnessState
& $RecoveryScript -BleExe PulseItFakeRadio -LogDirectory $Root
exit $LASTEXITCODE
