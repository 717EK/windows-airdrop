param(
    [string]$Deps = (Join-Path $PSScriptRoot '..\..\deps'),
    [string]$MingwRoot = (Join-Path $env:USERPROFILE 'scoop\apps\mingw\current')
)
$ErrorActionPreference = 'Stop'
$Deps = (Resolve-Path $Deps).Path
$MingwRoot = (Resolve-Path -LiteralPath $MingwRoot).Path
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$upstream = Join-Path $Deps 'libwdi-1.5.1'
if (-not (Test-Path $upstream)) { Expand-Archive (Join-Path $Deps 'setup-downloads\libwdi-source.zip') -DestinationPath $Deps }
$redist = Join-Path $Deps 'wdk-redist'
& 7z.exe x (Join-Path $Deps 'setup-downloads\wdk-redist') "-o$redist" -y | Out-Null
if ($LASTEXITCODE) { throw 'Cannot extract Microsoft WDK redistributables.' }
$wdk = Join-Path $Deps 'wdk-embed'
$x64 = Join-Path $wdk 'redist\wdf\x64'
New-Item -ItemType Directory $x64 -Force | Out-Null
foreach ($name in @('WdfCoInstaller01011.dll','winusbcoinstaller2.dll')) {
    $input = Join-Path $redist ("_" + $name + '_00001')
    $signature = Get-AuthenticodeSignature $input
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') { throw "Microsoft signature failed: $name" }
    Copy-Item -LiteralPath $input -Destination (Join-Path $x64 $name) -Force
}
$wdkMacro = $wdk.Replace('\','/')
@"
#define WDK_DIR "$wdkMacro"
#define WDF_VER 1011
#define COINSTALLER_DIR "wdf"
#define X64_DIR "x64"
#define OPT_M64 1
#define ENABLE_LOGGING 1
#define INCLUDE_DEBUG_LOGGING 1
#define DEFAULT_VISIBILITY __attribute__((visibility("default")))
"@ | Set-Content (Join-Path $upstream 'config.h') -Encoding ascii
$env:PATH = (Join-Path $MingwRoot 'bin') + ';' + $env:PATH
Push-Location (Join-Path $upstream 'libwdi')
try {
    & gcc -O2 -static -I.. installer.c -o installer_x64.exe -lsetupapi -lnewdev -lole32
    if ($LASTEXITCODE) { throw 'libwdi installer build failed.' }
    & gcc -O2 -I.. embedder.c -o embedder.exe
    if ($LASTEXITCODE) { throw 'libwdi resource builder failed.' }
    & .\embedder.exe embedded.h
    if ($LASTEXITCODE) { throw 'libwdi resource embedding failed.' }
    & windres -I. libwdi.rc -o libwdi-resource.o
    if ($LASTEXITCODE) { throw 'libwdi version resource failed.' }
    & gcc -O2 -static -I.. -I. (Join-Path $repo 'tools\usb-driver.c') libwdi.c libwdi_dlg.c logging.c tokenizer.c vid_data.c pki.c libwdi-resource.o -o (Join-Path $Deps 'pulsedrop-usb.exe') -lsetupapi -lole32 -lntdll -lcomctl32 -luuid
    if ($LASTEXITCODE) { throw 'Target-specific USB installer build failed.' }
} finally { Pop-Location }
