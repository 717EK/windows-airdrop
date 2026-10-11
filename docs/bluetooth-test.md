# Bluetooth test: can Windows do AirDrop's Bluetooth half?

**Date:** 2026-10-11, on this PC (Windows 11 Home 26200, built-in Bluetooth adapter)
**Tool:** `tools/bleprobe`, a C# program using only built-in WinRT APIs (`BluetoothLEAdvertisementPublisher` and `BluetoothLEAdvertisementWatcher`)

AirDrop uses Bluetooth LE in both directions. A sender broadcasts an Apple Continuity "AirDrop" advertisement (company ID `0x004C`, TLV type `0x05`). That wakes nearby receivers, which then bring up AWDL and announce themselves. To send to an iPhone, Windows has to broadcast that advertisement. To receive from one, Windows has to hear the iPhone's.

## Results

| Question | Result | Evidence |
|---|---|---|
| Does the adapter support LE advertising? | ✅ Yes | `LE=True peripheral=True central=True offload=True extendedAdv=False maxAdvLen=31` |
| Will Windows publish Apple (`0x004C`) manufacturer data? | ✅ **Yes, accepted and started** | `airdrop company=0x004c data=0512400000000000000003000000000000000000 status: Waiting -> Started` |
| Control (`0xFFFF` test company) | ✅ Started | `control company=0xffff data=010203 status: Waiting -> Started` |
| Can Windows hear Apple Continuity advertisements? | ✅ Yes | In 20 s it heard types `0x07`, `0x09`, `0x10` (Nearby info, from 2 devices), `0x12` (Find My) and `0x16` from nearby Apple devices |
| **Does our advertisement actually go out over the air, unchanged?** | ⏳ **Not yet confirmed** | Windows can't hear its own broadcasts. The Mac scanner (`tools/blescan.swift`) is blocked: macOS denies Bluetooth to processes started over SSH (`STATE 3 = unauthorized`). Run from Terminal, it waits on a permission prompt behind the locked screen. |
| **Can Windows see an iPhone's AirDrop advertisement (`0x05`)?** | ⏳ **Not yet tested** | It only appears while someone has the Share → AirDrop sheet open, so it needs a person with an iPhone |

The payload is omdrop-awdl's "arm A" shape (the OS 26 format), with a zeroed tag and zero contact hashes. Zero hashes mean "no contact match", which is what a receiver in Everyone mode accepts. `0512` is the Continuity type and length. Windows takes the company ID separately and adds it on air.

## What this changes

The roadmap listed this as the biggest unknown after the radio: would Windows let an app broadcast Apple's company ID? **It does, at least at the API level.** Windows accepts it and reports the publisher as started. So the Bluetooth half of AirDrop doesn't need custom drivers, BlueZ, or a Linux machine.

## Still open, and how to close it

1. **On-air check (about 2 minutes, with a phone).** Install nRF Connect on any phone and start scanning, filtered to company `0x004C`. On the PC, run:
   ```bash
   dotnet run --project tools/bleprobe -- publish 30
   ```
   You should see manufacturer data `4c00 0512 4000…` from the PC. Alternatively, unlock the Mac, run `swiftc tools/blescan.swift -o blescan && ./blescan 40` in Terminal, and allow Bluetooth when asked.
2. **iPhone to Windows.** Run `dotnet run --project tools/bleprobe -- scan 30`, then on an iPhone nearby open Share → AirDrop. A line for `type 0x05 AirDrop` means Windows can see AirDrop senders.
3. **Does the iPhone react to us?** This can only be tested once there's an AWDL radio (Gate B). The Bluetooth advertisement only wakes the iPhone; nothing visible happens until it finds us over AWDL.

## Caveats

- `maxAdvLen=31` and no extended advertising: the AirDrop TLV (22 bytes with the company ID) fits in a legacy advertisement, so this doesn't matter.
- Windows picks the advertising address and rotates it. That probably matters less than it would for a pairing flow, but it hasn't been checked against an iPhone.
- The `0x16` type heard in the scan isn't one I can name with confidence, so it's left unlabelled.
