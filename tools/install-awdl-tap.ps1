# Installs only the prepared, signature-checked TAP driver and one awdl0 adapter.
# Run without -Install to validate the package without changing Windows.
param([switch]$Install, [string]$Package)
$ErrorActionPreference = 'Stop'
$tapPackage = if ($Package) { $Package } else { Join-Path $PSScriptRoot '..\..\deps\tap-9.27.0\dist.win10\amd64' }
$tapPackage = (Resolve-Path -LiteralPath $tapPackage).Path
$expected = @{
    'devcon.exe' = 'BEE3A63DB18565AB77AD5714594B658C1D47C7E475009B25D430DF9ED634EA46'
    'OemVista.inf' = '1327AB3A8C50691F04BEA8E2CA356C5B604092A719E219464F8CC4B42E192DE9'
    'tap0901.cat' = 'EE062E5EF2743CEAB10C64830E4CEFE52E35CC1ECE85947AC4E61DDD1C0B05F7'
    'tap0901.sys' = '581DCAACE05D5C1AC9512457FF50565ACA5D904D2C209BD3FC369CA4D4A0D2B1'
}
foreach ($tapFile in $expected.Keys) {
    $tapPath = Join-Path $tapPackage $tapFile
    if ((Get-FileHash -LiteralPath $tapPath -Algorithm SHA256).Hash -ne $expected[$tapFile]) {
        throw "Package file changed: $tapFile"
    }
}
foreach ($tapFile in @('tap0901.cat', 'devcon.exe')) {
    $tapSignature = Get-AuthenticodeSignature -LiteralPath (Join-Path $tapPackage $tapFile)
    if ($tapSignature.Status -ne 'Valid' -or $tapSignature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
        throw "Microsoft signature verification failed: $tapFile"
    }
}
Write-Output 'Prepared TAP-Windows6 9.27.0 amd64 package verified.'
if (-not $Install) { Write-Output 'Validation only. Use -Install in an administrator PowerShell after approval.'; return }
$tapIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$tapIdentity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script in an administrator PowerShell.'
}
if (Get-NetAdapter -Name awdl0 -ErrorAction SilentlyContinue) { throw 'awdl0 already exists; no changes made.' }
$tapBefore = @(Get-NetAdapter -IncludeHidden | Select-Object -ExpandProperty InterfaceGuid)
Push-Location -LiteralPath $tapPackage
try {
    & .\devcon.exe install OemVista.inf tap0901
    $tapExit = $LASTEXITCODE
    if ($tapExit -notin @(0,1)) { throw "devcon failed: $tapExit" }
} finally { Pop-Location }
$tapNew = @()
for ($tapAttempt = 0; $tapAttempt -lt 20; $tapAttempt++) {
    $tapNew = @(Get-NetAdapter -IncludeHidden | Where-Object {
        $_.InterfaceDescription -like 'TAP-Windows Adapter V9*' -and $_.InterfaceGuid -notin $tapBefore
    })
    if ($tapNew.Count -eq 1) { break }
    Start-Sleep -Milliseconds 500
}
if ($tapNew.Count -ne 1) { throw 'Could not uniquely identify the new TAP adapter; inspect Device Manager.' }
$tapNew[0] | Rename-NetAdapter -NewName awdl0
Get-NetAdapter -Name awdl0 | Select-Object Name, InterfaceDescription, InterfaceGuid, ifIndex, Status
if ($tapExit -eq 1) { Write-Output 'Windows reported that a reboot may be required.' }
