# Workaround for the Windows Wi-Fi driver blocker

**Date:** 2026-10-13
**Problem:** No Windows Wi-Fi driver lets an app send raw 802.11 frames, and AWDL depends on that. The blocker is the Windows driver, not the chip.
**Workaround:** for a **USB** adapter, skip the vendor driver entirely. Bind the dongle to Microsoft's generic WinUSB driver (one click in Zadig) and run the Wi-Fi driver *in our own process*, talking to the chip over libusb. No kernel driver, no driver signing, and no Windows Wi-Fi stack in the way.

## Proof it works on Windows today

- **devourer** (https://github.com/OpenIPC/devourer, active as of 2026-10-13) is a user-space driver for Realtek USB Wi-Fi chips. Its README says it works "on Linux, macOS, Windows and Android", using libusb.
- OpenIPC ships Windows apps built on it: **aviateur** and **fpv4win**. Their setup steps are to install the libusb driver with Zadig, then run the app. These are video links that use raw monitor mode and frame sending. So raw 802.11 on Windows with ordinary USB dongles is a shipping reality, not a theory.

## What AWDL needs from the radio, and what devourer has

| AWDL needs | devourer (from its README and `src/AdapterCaps.h`) |
|---|---|
| Monitor-mode receive with radiotap | ✅ "raw monitor-mode RX" on every supported chip |
| Raw frame injection with per-frame rate | ✅ Each injected frame carries its own radiotap rate, bandwidth and power |
| Channel switching every slot (AWDL slots are about 64 TU, roughly 65 ms) | ✅ "A channel hop costs ~0.5–2.5 ms depending on chip" |
| **Hardware ACKs** for unicast to our address (the reason OWL needs "active monitor" cards) | ✅ `SetAckResponder(mac)` arms the chip's SIFS-timed ACK engine while monitor and injection keep running. Measured working on 8822B, 8812C/8822C, 8812E/8822E, 8814A and 8733B (1725 of 1725 frames ACKed). 8812A works but is degraded. Not available on the Wi-Fi 6 (AX) parts. |
| Accurate timing for AWDL sync | ✅ Every received frame is stamped with the chip's microsecond clock (TSF) |

All five map onto `radio/radio.h`: `capture`, `transmit`, `set_channel`, and `capabilities` (`raw_80211_injection`, `channel_control`, `reliable_unicast_ack`).

## Which dongle

Pick a chip with a working ACK responder that devourer has benchmarked:

| Chip | Example devourer tested (USB ID) | ACK responder | Notes |
|---|---|---|---|
| **RTL8822BU** | **TP-Link Archer T3U** (`2357:012d`) | ✅ | A common retail dongle, 2.4 and 5 GHz. **The first choice.** |
| RTL8812CU | LB-LINK WDN1300H (`0bda:c812`) | ✅ | The fastest in devourer's table |
| RTL8733BU | LB-LINK BL-M8733BU2-L (`0bda:b733`) | ✅ Best measured | A bare module, not a retail dongle |
| RTL8812AU | Alfa AWUS036ACH and others | ⚠️ Intermittent ACKs | Avoid for AWDL |

Check the USB ID after buying: product names get reused across chips, the way TL-WN722N v2 is a Realtek while v1 is an AR9271.

This **replaces the earlier "skip Realtek" advice**. That was about Linux kernel drivers. With devourer, Realtek is the only family with a working route on Windows.

## Risks (none tested for AWDL yet)

1. **No one has run AWDL on devourer.** iPhones are picky about timing and rates. Apple's AWDL also needs ACKs at HT rates (omdrop-owl found iPhones ignore peers that can't ACK HT).
2. **The ACK responder is built for FPV links,** where both ends are devourer. Whether its ACK timing and rate satisfy an iPhone is unknown.
3. **Timing through USB in user space.** The radio is about 1 ms away over USB, and our scheduler runs on Windows timers. omdrop-owl's MT7925 fixed-pace sending shows radios can impose their own limits.
4. **License: devourer is GPL-2.0, and OWL is GPL-3.0-or-later.** The devourer source I checked has no "or later" clause, so treat it as GPL-2.0-only, which can't be combined with OWL into one program. Run devourer in a **separate radio process** that talks to the OWL process over a local pipe. That's the same separation omdrop uses, and our `RadioBackend` interface already fits it.
5. Adapter limits: devourer's own notes give the RTL8812AU degraded ACKs, the RTL8812EU dead 2.4 GHz TX, and USB 2.0 caps on some parts.

## Plan

1. Buy a TP-Link Archer T3U and check it shows as `2357:012d`.
2. Build devourer on Windows (libusb via vcpkg), bind the dongle with Zadig, and run its `rxdemo`. Seeing the iPhone's AWDL action frames (and our own AirDrop advertisements nearby) is the first win, and it takes days, not weeks.
3. Write `radio/devourer-bridge`: a small GPL-2.0 process wrapping devourer, speaking a frame protocol over a named pipe, plus a `RadioBackend` client on the OWL side.
4. Gate A on Windows: inject PSF/MIF frames and see whether the iPhone lists us as an AWDL peer and ACKs flow in both directions.
5. Then Gates B and C exactly as planned, with no Linux anywhere.

Keep the AR9271 plus Linux bridge as the fallback, in case iPhones reject devourer's timing.
