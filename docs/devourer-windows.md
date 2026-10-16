# devourer on Windows

devourer is OpenIPC's user-space Realtek Wi-Fi driver, the route around the Windows driver blocker (see [native-radio-workaround.md](native-radio-workaround.md)). **It builds and passes its self-tests on this PC with MinGW-w64, and with an Archer T3U it hears iPhone AWDL traffic on Windows.**

## Status (updated 2026-10-15)

| Check | Result |
|---|---|
| Library plus all tools, MinGW-w64 GCC 16.2, Release | ✅ Builds |
| devourer's hardware-free self-tests (`ctest`) | ✅ **78 of 78 pass**. 3 skip because they need Python generators or the `reference/` submodules. |
| `rxdemo.exe` and `txdemo.exe` start, and libusb enumerates USB devices | ✅ They report "no supported device", as expected with no dongle |
| `tools/build-devourer.sh` from an empty folder | ✅ Works. The libusb signature check rejects a tampered archive. |
| Archer T3U (`2357:012d`) on WinUSB via Zadig, brought up by devourer | ✅ 2026-10-15 |
| Monitor receive on 2.4 GHz and 5 GHz | ✅ Channel 6: 2,930 frames in 15 s. Channel 44: 817 in 10 s. |
| **iPhone AWDL frames heard on Windows** (AirDrop sheet open) | ✅ **Channel 6: 271** AWDL frames (198 PSF, 73 MIF) from 2 addresses. **Channel 149: 309** (150 PSF, 159 MIF) from 3. Channel 44: none. |
| **OWL parses those real iOS frames** (`awdl_pcap`) | ✅ **580 of 580 accepted**. OWL tracked 3 Apple peers, decoded 2 AWDL names, and elected the iPhone cluster's master. Needed `patches/win-02`. |
| Transmit (injection) on air | ⏳ Not tried yet |

Pinned versions:
- devourer `74c6b37ce1c7ae198873d3a720466def691b86fb` (GPL-2.0)
- libusb v1.0.30 official Windows binaries, statically linked. Signature checked against Tormod Volden's key `C681 8737 9B23 DE9E FC46 651E 2C80 FF56 C683 0A0E`, as listed in libusb's `SIGNING_KEYS.txt`.

## Capture tool

`radio/devourer/awdlcap.cpp` (built by the script to `../deps/awdlcap.exe`) monitors one channel, writes a Wireshark-readable pcap (802.11, DLT 105), and counts frame types, picking out AWDL PSF and MIF frames and their senders:

```bash
../deps/awdlcap.exe 149 20 ch149.pcap
build/awdl_pcap.exe ch149.pcap          # replay through OWL's parser
```

Captures contain the addresses of nearby devices, so they're kept out of the repo.

## Build

```bash
scoop install cmake ninja mingw pkgconf 7zip
bash tools/build-devourer.sh              # into ../deps (D:\pulse_it\deps)
```

Two fixes were needed for MinGW. Both look worth sending upstream:

1. **`txdemo` calls `fork()`**, for a legacy Termux-only receive path. MinGW has no `fork()`. devourer stubs it out for MSVC but not for MinGW, so `patches/devourer-01-mingw-txdemo-fork.patch` extends the stub.
2. **Static link order.** CMake places `libusb-1.0.a` before `libdevourer.a`, and GNU ld resolves static archives in a single pass, which leaves `libusb_error_name` undefined. The script appends libusb to the end of every link line through `CMAKE_CXX_STANDARD_LIBRARIES`. No source change is needed.

## When the Archer T3U arrives

1. **Check it's the right chip:**
   ```bash
   Get-PnpDevice -PresentOnly | Where-Object InstanceId -match 'VID_2357&PID_012D'
   ```
2. **Bind it to WinUSB with Zadig** (https://zadig.akeo.ie): *Options → List All Devices*, pick the device with USB ID **2357 012D** and nothing else, choose *WinUSB*, then *Replace Driver*. Your Intel Wi-Fi and its Bluetooth must stay on their own drivers. To undo, uninstall the device in Device Manager with *delete the driver* ticked, then replug it.
3. **First light (receive)** on channel 6. You should see the beacons of nearby networks:
   ```bash
   DEVOURER_VID=0x2357 DEVOURER_PID=0x012d DEVOURER_CHANNEL=6 ../deps/devourer/build/rxdemo.exe
   ```
4. **AWDL sighting:** run it on channels 6, 44 and 149 with an iPhone nearby and its Share → AirDrop sheet open, and look for 802.11 action frames (category 127, Apple OUI `00:17:f2`). That shows the dongle hears AWDL on Windows.
5. **Transmit check:** run `txdemo.exe` with the same environment variables. It injects devourer's test beacon, and the same dongle (or a phone app like nRF Connect, or a Wi-Fi scanner) can confirm it's on air.

## Notes from devourer's own docs that matter here

- The T3U's internal antennas fall off at high data rates (devourer measured MCS7 delivery around 3% from one T3U). AWDL action frames and OWL's data frames use low to mid rates (OWL defaults to 12 Mbps legacy), so this is expected to be fine. Measure before assuming.
- The **library reads no environment variables**. The `DEVOURER_*` variables belong to the demos. Our radio bridge will set `DeviceConfig` directly.
- **The caller owns libusb, including teardown order:** destroy the radio first, then release the interface, close the handle, and call `libusb_exit` last. Doing it in the wrong order crashes under transmit load.
- `send_packet` buffers must start with a radiotap header. OWL already builds frames that way.
