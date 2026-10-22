# Development installer

Version 0.2.7 is a diagnostic developer build, not a qualified public binary release. Build prerequisites and commands are in [docs/build.md](../docs/build.md).

`tools/build-installer.ps1` packages a self-contained desktop UI, BLE helper, isolated Python runtime, separate OWL/devourer engines, TAP-Windows6 and a target-restricted libwdi frontend. Downloads and hashes are pinned in `dependencies.json`; Python packages are pinned in `requirements.txt`. Build tools verify expected signatures where implemented. The installer itself is currently unsigned.

The setup offers driver configuration explicitly. WinUSB binding is restricted to one connected, non-composite `2357:012d` adapter, which loses ordinary Wi-Fi use. Inspect is read-only. Configure exports the prior vendor package where applicable, creates interface/link-local scoped firewall rules and binds supported devices. Restore attempts to reinstate a compatible saved/inbox Wi-Fi driver. It does not alter Intel Bluetooth. Close PulseIt before configuration/restoration. Setup state and backups under `%PROGRAMDATA%\PulseIt` are protected against ordinary-user writes.

OWL (GPL-3.0-or-later) and devourer (GPL-2.0-only) remain separate executables. libwdi (LGPL-3.0-or-later) is statically linked into the USB helper; preserve upstream source and the build/configuration/relink inputs when distributing it. Third-party runtime/driver redistribution terms need separate review.

Source snapshots exclude local handovers and session records. Existing previously built installers are not retroactively sanitized: rebuild from the reviewed public tree and inspect embedded source before upload. No existing local installer should accompany this source preparation.

Release gates: signing, exact bundled notices/source/relink compliance, clean-PC install/uninstall and driver restoration, repeated unplug/replug, live Bluetooth coexistence and current-build large transfers both directions. Automated fixtures and adapter startup checks do not satisfy those gates. Diagnostic exports can contain personal metadata and must be reviewed before sharing.
