# windows-airdrop

A research harness for the Windows AirDrop development plan. It builds OWL's AWDL core on Windows, unmodified and with omdrop-owl's patches applied, and runs it against a mock radio. No Wi-Fi hardware is involved.

- Audit and findings: [docs/section16-audit.md](docs/section16-audit.md)
- The scheduler that replaces OWL's Linux event loop: `sched/` ([docs/scheduler.md](docs/scheduler.md))
- Radio interface and mock: `radio/`
- POSIX stand-ins for OWL's core, plus a clock that tests can replace: `compat/`
- Our fixes to OWL: `patches/`
- Bluetooth test (can Windows broadcast and hear Apple's AirDrop signal?): `tools/bleprobe` ([docs/bluetooth-test.md](docs/bluetooth-test.md))

```bash
scoop install cmake ninja mingw
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build
ctest --test-dir build
```

License: OWL is GPL-3.0-or-later, so anything built here that links it is GPL-3.0-or-later too.
