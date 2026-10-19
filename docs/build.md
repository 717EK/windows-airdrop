# Building PulseDrop

Use Windows x64, Git/Git Bash, CMake 3.20+, Ninja, MinGW-w64 GCC, .NET 8 SDK and Python 3.11+. MSVC `cl` is unsupported for the OWL core. Keep a sibling `deps/` directory beside this checkout for downloads. Ordinary builds and tests do not install drivers.

## Native protocol and tests

In Git Bash with GCC and Ninja on PATH:

```bash
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake fetches pinned OWL, omdrop-owl and GoogleTest revisions. Local `win-*.patch` files apply after the pinned omdrop-owl series. The TAP round-trip test skips without an installed TAP adapter and administrator access; the timing benchmark is opt-in. A skip is not hardware acceptance.

## Python and desktop

In PowerShell at the repository root:

```powershell
py -3 -m venv ..\deps\airdrop-venv
..\deps\airdrop-venv\Scripts\python.exe -m pip install -r airdrop\requirements.txt
..\deps\airdrop-venv\Scripts\python.exe -m unittest discover -s tests -p 'test_*.py'
dotnet build desktop\PulseDrop.csproj -c Release
dotnet build tools\bleprobe\bleprobe.csproj -c Release
```

The automated PowerShell recovery tests mock services and the Bluetooth radio; they do not modify the live OS. In contrast, desktop `--lifecycle-test`, `--lifecycle-soak-test` and `--recovery-test` access actual hardware and can disrupt adapters.

## Bridge and local bundle

Run `tools/build-devourer.sh` from Git Bash to build pinned OpenIPC devourer and the separate GPL-2.0-only bridge. It also requires `gh`, GnuPG, pkgconf and 7-Zip, and verifies the official libusb release signature. Do not combine devourer objects with an OWL-linked target.

After building and configuring devices, `tools/package-desktop.ps1 -Destination <absolute-output-folder> -NoShortcuts` creates an administrator-protected local bundle. It requires elevated PowerShell and locks the destination and its Python environment against non-administrator writes. Use a dedicated environment and destination. This bundle references its sibling Python environment and is not portable.

## Optional installer

The manual **Unsigned development installer** GitHub Actions workflow builds the native engines, USB helper, managed apps and complete installer on a GitHub-hosted Windows runner. It uploads an unsigned installer, corresponding source, payload manifest and build-origin record; it neither publishes a release nor requests signing. Run it from the Actions tab after publishing reviewed source. The workflow has been linted and its build steps tested locally; a successful hosted run is still required.

For the same dependency bootstrap locally, run `tools/bootstrap-ci.ps1`, build the native targets and radio bridge, then run `tools/build-installer.ps1 -MingwRoot ../deps/mingw64`. Bootstrap downloads and validates the TAP package without installing it. Compiler and redistributable archives have pinned hashes; hosted runner tools, MSYS2 helper packages and the .NET 8 servicing SDK can change, so this is not a promise of byte-identical builds.

`tools/build-installer.ps1` expects the native engines, sibling Python environment, signed TAP-Windows6 9.27.0 package under `../deps/tap-9.27.0/dist.win10/amd64`, MinGW and 7-Zip. Downloads are pinned in `installer/dependencies.json`. It is not a one-command bootstrap of a clean PC. See [installer notes](../installer/README.md).

Device configuration is separate: `Setup-Devices.ps1 -Action Inspect` is read-only; `Configure` changes supported drivers and scoped firewall rules; `Restore` attempts to restore the saved vendor Wi-Fi driver. Inspect USB IDs first and close PulseDrop before configuration.

Before distributing binaries, rebuild from reviewed public source, inspect embedded source/notices, sign the binaries/installer and validate clean-PC installation, transfers, cancellation, unplug/replug and restoration. Existing local installers may embed private historical documentation and must not be uploaded.
