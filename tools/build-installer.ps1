param(
    [string]$Deps = (Join-Path $PSScriptRoot '..\..\deps'),
    [string]$MingwRoot = (Join-Path $env:USERPROFILE 'scoop\apps\mingw\current')
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Deps = (Resolve-Path $Deps).Path
$downloads = Join-Path $Deps 'setup-downloads'
$stage = Join-Path $repo 'build\installer-stage'
$python = Join-Path $Deps 'airdrop-venv\Scripts\python.exe'
$manifest = Get-Content (Join-Path $repo 'installer\dependencies.json') -Raw | ConvertFrom-Json
New-Item -ItemType Directory $downloads,$stage -Force | Out-Null
foreach ($file in $manifest.files) {
    $target = Join-Path $downloads $file.name
    if (-not (Test-Path $target)) { Invoke-WebRequest -Uri $file.url -OutFile $target }
    if ((Get-FileHash $target -Algorithm SHA256).Hash -ne $file.sha256) { throw "Dependency hash mismatch: $($file.name)" }
}
& (Join-Path $PSScriptRoot 'build-usb-helper.ps1') -Deps $Deps -MingwRoot $MingwRoot
dotnet publish (Join-Path $repo 'desktop') -c Release -r win-x64 --self-contained true -o $stage --verbosity quiet
if ($LASTEXITCODE) { throw 'Desktop publish failed.' }
dotnet publish (Join-Path $repo 'tools\bleprobe') -c Release -r win-x64 --self-contained true -o (Join-Path $stage 'engine\bleprobe') --verbosity quiet
if ($LASTEXITCODE) { throw 'Bluetooth publish failed.' }
$runtime = Join-Path $stage 'runtime'
Expand-Archive (Join-Path $downloads 'python-embed.zip') -DestinationPath $runtime -Force
$signature = Get-AuthenticodeSignature (Join-Path $runtime 'python.exe')
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Python Software Foundation') { throw 'Python runtime signature failed.' }
& $python -m pip install --upgrade --target (Join-Path $runtime 'Lib\site-packages') --only-binary=:all: --platform win_amd64 --python-version 3.13 --implementation cp --abi cp313 -r (Join-Path $repo 'installer\requirements.txt')
if ($LASTEXITCODE) { throw 'Vendored Python packages failed.' }
@('python313.zip','.','Lib\site-packages','..\airdrop','import site') | Set-Content (Join-Path $runtime 'python313._pth') -Encoding ascii
@{PythonExe='runtime\python.exe'} | ConvertTo-Json | Set-Content (Join-Path $stage 'settings.json') -Encoding UTF8
foreach ($folder in @('airdrop','drivers\tap','licenses','source')) { New-Item -ItemType Directory (Join-Path $stage $folder) -Force | Out-Null }
Get-ChildItem (Join-Path $repo 'airdrop') -File | Copy-Item -Destination (Join-Path $stage 'airdrop') -Force
Copy-Item (Join-Path $repo 'build\awdl_link.exe'),(Join-Path $Deps 'devourer-bridge.exe') -Destination (Join-Path $stage 'engine') -Force
$mingw = (Resolve-Path -LiteralPath $MingwRoot).Path
foreach ($dll in @('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) { Copy-Item (Join-Path $mingw "bin\$dll") -Destination (Join-Path $stage 'engine') -Force }
Copy-Item (Join-Path $Deps 'pulseit-usb.exe') -Destination (Join-Path $stage 'drivers') -Force
Copy-Item (Join-Path $Deps 'tap-9.27.0\dist.win10\amd64\*') -Destination (Join-Path $stage 'drivers\tap') -Recurse -Force
Copy-Item (Join-Path $repo 'desktop\RecoverBluetooth.ps1'),(Join-Path $repo 'installer\Setup-Devices.ps1'),(Join-Path $repo 'tools\install-awdl-tap.ps1'),(Join-Path $repo 'tools\configure-awdl-firewall.ps1') -Destination $stage -Force
Copy-Item (Join-Path $repo 'assets') -Destination $stage -Recurse -Force
foreach ($doc in @('README.md','QUICK-START.html','CONTRIBUTING.md','SECURITY.md','LICENSE','COPYING','THIRD_PARTY_NOTICES.md','PRIVACY.md','CODE-SIGNING.md')) { Copy-Item (Join-Path $repo $doc) -Destination $stage -Force }
New-Item -ItemType Directory (Join-Path $stage 'docs') -Force | Out-Null
foreach ($doc in @('build.md','architecture.md','limitations.md')) { Copy-Item (Join-Path $repo "docs\$doc") -Destination (Join-Path $stage 'docs') -Force }
Copy-Item (Join-Path $Deps 'devourer\LICENSE') (Join-Path $stage 'licenses\devourer-GPL-2.txt') -Force
Copy-Item (Join-Path $repo 'build\_deps\owl-src\COPYING') (Join-Path $stage 'licenses\OWL-GPL-3.txt') -Force
Copy-Item (Join-Path $Deps 'libwdi-1.5.1\COPYING-LGPL') (Join-Path $stage 'licenses\libwdi-LGPL-3.txt') -Force
Copy-Item (Join-Path $Deps 'wdk-redist\redist.txt.x64') (Join-Path $stage 'licenses\Microsoft-WDK-redist.txt') -Force
Copy-Item (Join-Path $mingw 'licenses\gcc\COPYING.RUNTIME') (Join-Path $stage 'licenses\GCC-runtime-exception.txt') -Force
Copy-Item (Join-Path $mingw 'licenses\winpthreads\COPYING') (Join-Path $stage 'licenses\winpthreads.txt') -Force
Copy-Item (Join-Path $downloads 'libwdi-source.zip') (Join-Path $stage 'source\libwdi-1.5.1.zip') -Force
# Bundle corresponding local/upstream source without device captures, identities or credentials.
& $python (Join-Path $repo 'tools\installer-source.py') --stage $stage --deps $Deps
if ($LASTEXITCODE) { throw 'Source and payload manifests failed.' }
& (Join-Path $runtime 'python.exe') -E -s -X utf8 -c 'import cryptography, zeroconf, streaming, service'
if ($LASTEXITCODE) { throw 'Bundled runtime import check failed.' }
Write-Output 'Bundled runtime imports: PASS'
$compiler = Join-Path $Deps 'inno-6.7.3\ISCC.exe'
if (-not (Test-Path $compiler)) {
    $setup = Join-Path $downloads 'innosetup.exe'
    $signature = Get-AuthenticodeSignature $setup
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Pyrsys') { throw 'Compiler signature failed.' }
    Start-Process $setup -WindowStyle Hidden -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/CURRENTUSER',("/DIR=" + (Join-Path $Deps 'inno-6.7.3'))) -Wait
}
& $compiler ("/DStage=$stage") (Join-Path $repo 'installer\PulseIt.iss')
if ($LASTEXITCODE) { throw 'Setup compiler failed.' }
