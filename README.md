<p align="center">
  <img src="assets/readme/hero.svg" alt="PulseIt — send files between your iPhone and Windows PC. Developer preview; supported USB adapter required." width="100%">
</p>

<p align="center">
  <a href="#what-you-need">What you need</a> ·
  <a href="#get-started">Get started</a> ·
  <a href="#send-a-file">Send a file</a> ·
  <a href="#need-help">Need help?</a> ·
  <a href="docs/build.md">Build from source</a>
</p>

**PulseIt lets a Windows PC exchange files with a nearby iPhone using AirDrop. No iPhone app is needed.**

> **Developer preview:** this needs a specific USB Wi-Fi adapter and administrator setup. It does not work with your PC's built-in Wi-Fi alone. iPhone-to-PC transfers were measured repeatedly on the current build (an 80 MB file in about 14–20 seconds); PC-to-iPhone was last verified on an earlier build. [Current limits →](docs/limitations.md)

## What you need

| On your Windows PC | On your iPhone |
| --- | --- |
| Windows x64; tested on Windows 11 | A nearby, unlocked iPhone |
| **TP-Link Archer T3U — USB ID `2357:012d`** | AirDrop: **Everyone for 10 Minutes** |
| Working Bluetooth | No extra app |
| Administrator access for driver setup | Accept incoming files when asked |

**Check the USB ID, not just the adapter name.** Other hardware revisions are not qualified. Setup also installs/configures a virtual network adapter. The dedicated TP-Link adapter cannot provide normal Wi-Fi while configured for PulseIt, so keep another internet connection available.

## Get started

1. **Build the development setup.** Follow the [Windows build guide](docs/build.md). There is no qualified public installer attached to this source snapshot. A local installer is a developer test build, not a general release.
2. **Configure the supported adapter.** Plug it in, run setup and approve the administrator prompt. Choose adapter configuration only after checking the USB ID. Close PulseIt before changing drivers.
3. **Open PulseIt.** Turn receiving on, then follow the three steps below.

<img src="assets/readme/quick-start.svg" alt="Receive in three steps: 1. Plug in the supported TP-Link adapter. 2. Open the configured PulseIt app and start receiving. 3. On iPhone choose Share, AirDrop, Windows PC; approve on Windows." width="100%">

### iPhone → PC

1. On the iPhone, open a photo, video or file.
2. Tap **Share → AirDrop → Windows PC** (or the name you set in PulseIt).
3. **Accept** the request on Windows. Wait for completion, then use **Received files** to open the saved folder.

### PC → iPhone

1. Unlock the iPhone and refresh **AirDrop → Everyone for 10 Minutes**.
2. In PulseIt, open **Send** and refresh the device list.
3. Choose a file, select the iPhone and press **Send**.
4. **Accept** on the iPhone and wait for completion.

Keep the phone nearby and unlocked. The app limit is **1 GiB** per transfer; large videos may take several minutes. Keep channel matching off for the baseline setup. Turn on **Compatible sending (12 Mbit/s)** in the gear menu only if transfers become unreliable.

## Inside the app

These are real UI renders with **synthetic demo data**, not proof of a live connection or transfer.

<table>
  <tr>
    <td align="center"><strong>Find a device and send</strong><br><img src="assets/readme/send-demo.png" alt="Demo of the PulseIt Send screen with a sample iPhone device" width="360"></td>
    <td align="center"><strong>Turn receiving on</strong><br><img src="assets/readme/receive-demo.png" alt="Demo of the PulseIt Receive screen" width="360"></td>
  </tr>
</table>

## Need help?

| What you see | What to try |
| --- | --- |
| iPhone missing | Unlock it, refresh Everyone for 10 Minutes, then refresh PulseIt. |
| Windows PC missing on iPhone | Check that receiving is on and the supported USB adapter is plugged in. Reopen the AirDrop sheet. |
| Bluetooth recovery offered | It may briefly disconnect Bluetooth accessories. Use it only when you can tolerate that interruption. |
| Transfer paused | Keep both devices nearby and unlocked. Large files can pause; don't stop receiving while a transfer is active. |
| Transfer failed | Cancel the attempt, refresh the phone's Everyone window and device list, then retry. Avoid experimental speed settings. |
| Need normal Wi-Fi on the TP-Link again | Close PulseIt, then use **Restore USB Wi-Fi driver** from the Start menu. Restoration still needs clean-PC qualification. |

For a bug report, include the revision, adapter USB ID, direction, file size and what appeared on both devices. **Review and redact diagnostic exports before sharing:** they can contain names, addresses, file names and paths. Never post keys, captures or personal files.

## How fast is it?

Receiving an **80 MB file from an iPhone takes about 14–20 seconds (roughly 4–6 MB/s)**, down from about two minutes in 0.2.7. A Mac receives the same file in under 5 seconds, so there is still a gap. What changed:

- **80 MHz receive.** On channels 149, 36 and 44 the radio now listens at 80 MHz and the link advertises VHT, so the phone sends at its fastest rates. Start the bridge with `--width20` to go back to 20 MHz.
- **Block Ack reordering.** Aggregated frames the phone retransmits are put back in order before TCP sees them, instead of triggering slow recovery. It is on by default; start the link with `--no-rx-aggregation` to turn it off.
- **Cheaper replies.** Our TCP acknowledgements go out at 54 Mbit/s instead of 12, and back-to-back acknowledgements are merged (at most one per queued frame), so they use less of the phone's airtime.
- **Faster finish.** The receiver recognises the end of the upload after a quarter-second pause instead of waiting for a final chunk the iPhone never sends.
- **Quiet radio logging** by default, so per-frame logging no longer stalls the bridge.

Speed depends heavily on the phone. When the iPhone is connected to a Wi-Fi network on another channel, it gives AirDrop fewer of its time slots and transfers slow down; the phone gives AirDrop the most time when it is not connected to another Wi-Fi network. Sending from the PC has not been re-measured since these changes.

## What works — and what still needs testing

iPhone-to-PC transfers work on the current build and were measured repeatedly with an 80 MB file. Earlier builds also completed PC-to-iPhone transfers, including a **135.6 MB outgoing video**. The 0.2.7 startup fix passed ten adapter start/stop cycles. Current-build PC-to-iPhone transfer, Bluetooth coexistence, clean-PC install/restoration and sustained reliability remain open. **Contacts Only is unsupported.** This is not Apple-account identity verification.

[Detailed limitations](docs/limitations.md) · [Architecture](docs/architecture.md) · [Contributing](CONTRIBUTING.md) · [Security reporting](SECURITY.md) · [Privacy](PRIVACY.md) · [Code signing policy](CODE-SIGNING.md)

Signing status: **unsigned**. The SignPath Foundation application was submitted on October 18, 2026; review is pending. No approval or signed release is claimed.

<details>
<summary><strong>For developers: source layout and licensing</strong></summary>

| Folder | Purpose |
| --- | --- |
| `desktop/` | Windows UI and helper lifecycle |
| `airdrop/` | HTTPS discovery, consent and file streaming |
| `sched/`, `compat/`, `netif/` | OWL scheduling and virtual network interface |
| `radio/` | Separate USB radio bridge and pipe transport |
| `tools/`, `installer/` | Builds and explicit device configuration |
| `assets/`, `tests/`, `patches/` | Branding, regression tests and upstream changes |

Source defaults to GPL-3.0-or-later with GPL-2.0-only and MIT exceptions. See [LICENSE](LICENSE) and [third-party notices](THIRD_PARTY_NOTICES.md). OWL and devourer remain separate executables. Preserve applicable source/notices when distributing binaries.

</details>

PulseIt is independent of Apple, TP-Link, OpenIPC and the OWL authors. Product names belong to their respective owners.
