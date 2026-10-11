# Section 16 audit: omdrop-owl, OWL and omdrop on Windows

**Date:** 2026-10-11
**Scope:** the five deliverables from section 16 of the development plan.
**Hardware touched:** none. Nothing here shows that any Windows Wi-Fi adapter can send raw 802.11 frames.

## Summary

- OWL's protocol core (`src/`, about 4,600 lines of C) compiles on Windows with MinGW-w64 GCC 16.2. It needs five small stand-in headers and one function, and **no changes to OWL's code**. All 65 of OWL's own unit tests pass on Windows, and so do 5 new tests that run frames through a mock radio.
- Everything that touches the radio or the operating system (`daemon/`, about 2,100 lines) depends on Linux. It uses nl80211 through libnl, a TAP device, libpcap monitor mode with injection, and libev. None of it can be ported as-is. It has to be rewritten against whatever radio path survives Gate A.
- **Wintun is the wrong virtual adapter for OWL.** OWL creates `awdl0` as a Layer 2 TAP device (`IFF_TAP | IFF_NO_PI`) and passes whole Ethernet frames through it. Wintun only carries Layer 3 IP packets. The options are TAP-Windows6, or teaching OWL to handle IPv6 neighbour discovery itself.
- **Licensing is not clean.** omdrop-owl has no LICENSE file. omdrop-awdl's LICENSE only covers its kernel patches, and its userspace tools have no license headers. Ask both authors before anything is redistributed.
- The AirDrop receiver is not in any of the repos the plan names. It lives in **omdrop-plugin** (`airdrop-serve.py`). The radio-backend support it needs is in an unmerged pull request.

## 1. Upstream dependency, commit and license map

| Component | Repository | Pinned commit | Date | License | Role |
|---|---|---|---|---|---|
| omdrop-owl | t4t5/omdrop-owl | `ca3de11584370325647b63f8d2ecf571bb10e9d2` (HEAD audited) | 2026-10-11 | **No LICENSE file.** PKGBUILD lists GPL-3.0-or-later, GPL-2.0-only and ISC for the parts it bundles, but its own scripts, patches and profiles carry no license | Radio backend for omdrop: `omdrop-discoverable`, `owl-profiles`, 6 OWL patches, build and test scripts |
| OWL (fork) | jedbillyb/owl | `832d70f815c3d4a06a02117bf0fc5e868daa1ff0` (pinned by omdrop-owl, and current HEAD) | 2026-09-26 | GPL-3.0-or-later | AWDL in userspace. The fork adds the `-S` channel-sequence strategies and iOS 26 fixes |
| OWL (upstream) | seemoo-lab/owl | `da255a70f221784c836d943dd3f243bc798f223b` | 2026-08-17 | GPL-3.0-or-later | Original OWL. Reference only |
| radiotap parser | radiotap/radiotap-library (OWL submodule) | `94984dd829a605a2d7af55241145c159607f3b30` | — | ISC | Radiotap header parsing, compiled into the core |
| googletest (OWL submodule) | google/googletest | `703bd9caab50b139428cea1aaff9974ebee5742e` | 2018 | BSD-3-Clause | Not used. It doesn't build with current compilers, and omdrop-owl skips it too. This harness uses v1.15.2 |
| omdrop-awdl | brentkearney/omdrop-awdl | `534f91a525337951ca19c311e53d333bd6d55100` (pinned). HEAD `2bd88a08` is 14 commits ahead, with changes to `omdrop-discoverable` and `send-to-peer` | — | LICENSE says GPL-2.0-only for the **kernel patches**. The userspace tools have no license headers | Broadcom backend. omdrop-owl reuses 8 of its userspace tools unmodified (see section 2) |
| omdrop-plugin | brentkearney/omdrop-plugin | `cf93519c255a77b27919835ec0f2bff88ae0bb1d` (main). Radio-backend support is in PR #25, head `24f00386` | 2026-10-10 | MIT | The `omdrop` CLI, the panel, and the **receiver** `airdrop-serve.py` (TCP 8771 on `awdl0`). Defines the radio-backend contract in `docs/radio-backend.md`, version 0.7.0 |
| OpenDrop | seemoo-lab/opendrop | `11fe7ba7861093b302bc0637e8cb10adf2d29337` | 2026-08-17 | GPL-3.0 | Older AirDrop protocol reference. **Not in omdrop-owl's dependency chain** |

**The 6 omdrop-owl patches applied to OWL** (they apply cleanly at the pinned commit, and the harness applies them):

| Patch | Touches | What it does |
|---|---|---|
| owl-01-tx-rate | `daemon/owl.c`, `src/tx.c`, `src/tx.h` | `-R`: PHY rate for data frames (legacy or MCS) |
| owl-02-awdl-addr | `daemon/io.c`, `io.h`, `owl.c` | `-m`: override the AWDL MAC address |
| owl-03-fixed-radio | `daemon/core.c`, `owl.c` | `-F`: the radio stays on one channel, with a narrower multicast guard |
| owl-04-election-metric | `daemon/owl.c`, `src/election.c` | `-E`: elect the sync master by metric, the way Apple devices do |
| owl-05-fixed-radio-noack | `src/tx.c` | Unicast is sent with radiotap `TX_NOACK`, once and with no retries |
| owl-06-fixed-radio-pin | `daemon/core.c` | `-S pin`: advertise our channel in all 16 slots |

**System dependencies on Linux** (from the PKGBUILD): libpcap, libev, libnl-3 (genl and route), iw, iproute2, NetworkManager, polkit, keyutils, BlueZ, python-dbus, python-gobject, python-zeroconf, python-ifaddr, python-libarchive-c, python-pillow, and the openssl CLI.

**License risk.** OWL is GPL-3.0-or-later. Anything marked GPL-2.0-only can't be combined with it into one program. Today they ship as separate programs, which is fine. A Windows port should keep the same process boundary. It also must not redistribute omdrop-owl's scripts or omdrop-awdl's userspace tools until their authors state a license.

## 2. Linux-specific API inventory

### OWL core (`src/`): portable

| Linux/POSIX dependency | Where | How the Windows harness handles it |
|---|---|---|
| `<endian.h>` (`htole16`, `be32toh` and similar, 90+ call sites) | `wire.h` | `compat/endian.h` with byte-swap builtins, assuming little-endian |
| `<net/ethernet.h>`, `<netinet/ether.h>`: `struct ether_addr` (79 uses), `ETHER_ADDR_LEN`, `ETHER_MAX_LEN` | `ieee80211.h`, `peers.h`, `wire.h`, `channel.h` | `compat/net/ethernet.h` |
| `ether_ntoa` (13 uses, logging only) | several | `compat/compat.c` |
| `<netinet/in.h>`, `<netinet/ip6.h>` | `rx.c` (included but unused) | Empty stand-in headers |
| `clock_gettime(CLOCK_MONOTONIC)` | `state.c` | Provided by MinGW's winpthreads |
| `localtime_r` | `log.c` | Provided by MinGW with `_POSIX_THREAD_SAFE_FUNCTIONS` |
| `__attribute__((packed))` (17 structs) | frame and wire headers | Needs GCC or Clang. **MSVC `cl` can't build it** without rewriting those structs with `#pragma pack` |
| `radiotap.h` assumes `<stdint.h>` was already included | radiotap | Forced include |

### OWL daemon (`daemon/`): Linux-only and must be rewritten

| Area | Linux API | Used for |
|---|---|---|
| Wi-Fi control | libnl, nl80211: `NL80211_CMD_SET_INTERFACE` with `IFTYPE_MONITOR` and `MNTR_FLAG_ACTIVE`, `CMD_SET_CHANNEL`, `CMD_GET_WIPHY` | Switching to monitor mode, setting channels, listing allowed frequencies |
| Raw radio input and output | libpcap: `pcap_set_rfmon`, `pcap_setdirection(PCAP_D_IN)`, `DLT_IEEE802_11_RADIO`, `pcap_inject`, `pcap_get_selectable_fd` | Capturing and **injecting** radiotap frames |
| Virtual NIC | `/dev/net/tun`, `TUNSETIFF` with `IFF_TAP \| IFF_NO_PI`, plus `SIOCSIFHWADDR`, `SIOCSIFMTU` and `SIOCSIFFLAGS` ioctls | Creating `awdl0` as a **Layer 2** Ethernet device |
| Neighbour table | `rtnl_neigh_add`/`delete` with `NUD_PERMANENT`, `rtnl_link_*`, `getifaddrs` | Static IPv6 neighbour entries for each peer, link state, and link-local address |
| Event loop | libev: timers, `ev_io` on pcap/tap/netlink file descriptors, signals | All slot and TX timing (AWDL time units, about 1 ms) |
| Process | `getopt`, `signal(SIGHUP, SIGCHLD, SIGUSR1)`, `fcntl(O_NONBLOCK)`, `syslog` | CLI and daemon plumbing |

### omdrop-owl helper and the omdrop-awdl tools (Python and bash)

| Tool | Linux dependencies |
|---|---|
| `omdrop-discoverable` (bash) | `iw`, `ip`, NetworkManager (`nmcli`), polkit/`pkexec`, `/sys/class/net/*/device/driver`, driver profiles |
| `send-to-peer` | `ip -6 neigh`, `pkexec`, `loginctl`, `AF_INET6` sockets scoped to `awdl0` |
| `airdrop-send.py` | Portable: `http.client`, `ssl`, `plistlib`. Its socket is bound to the AWDL interface |
| `awdl_identity.py` | `pwd`, `fcntl`, `resource`, `keyctl` (Linux kernel keyring), openssl CLI |
| `ble-airdrop-adv.py` | BlueZ over D-Bus: `org.bluez.LEAdvertisingManager1`, Apple manufacturer data with company ID `0x004C` |
| `awdl-airdrop-adv.py`, `awdl-mdns-respond.py` | `socket.AF_PACKET` raw Layer 2 sockets on `awdl0`. Windows has no equivalent |
| `awdl-af-parse.py`, `awdl_debug.py` | Portable |
| omdrop-plugin `airdrop-serve.py` and the rest | `pwd`, `gi` (GObject), libarchive, `pkexec`. Mostly portable Python plus HTTP/TLS |

## 3. Windows portability matrix

| Component | Lines | Portability | Windows path | Notes |
|---|---|---|---|---|
| OWL core (`src/`) | ~4,600 | ✅ **Shown to build and pass tests** | Build as-is with `compat/` (MinGW GCC or clang-cl) | 70 of 70 tests pass. MSVC `cl` is not supported |
| radiotap parser | ~600 | ✅ Shown to build | As-is | — |
| Radio capture and inject (`io.c`, the pcap parts) | ~200 | ❌ **Blocked by hardware** | Npcap capture might work on some adapters. **No Windows injection path has been shown** | Gate A. On this PC (Intel AC 7265, 2015 driver), Native Wi-Fi reports "Action Frame: Not Supported" |
| Channel control (`netutils.c`, nl80211 parts) | ~350 | ❌ No equivalent API | WLAN API has no per-slot channel control. It would need a vendor or custom driver | AWDL hops channels every 16 time units (about 16 ms) |
| `awdl0` TAP device (`io.c`) | ~100 | ⚠️ Possible with a different driver | **TAP-Windows6** (Layer 2, as OWL expects). Wintun would need OWL to handle neighbour discovery and Layer 2 itself | Wintun is the plan's candidate, but it doesn't match OWL's design |
| Neighbour table (`netutils.c`, rtnl parts) | ~150 | ✅ Has an equivalent | `CreateIpNetEntry2` / `DeleteIpNetEntry2` (iphlpapi) | Needs admin rights |
| Event loop (libev) | — | ⚠️ | libev's Windows support is weak. Use libuv, or a waitable timer plus `WaitForMultipleObjects` | Timer resolution matters: Windows defaults to about 15.6 ms, so raise it with `timeBeginPeriod(1)` or high-resolution waitable timers |
| Daemon CLI and signals | ~260 | ✅ Simple | Console control handler instead of signals, and a small getopt | — |
| BLE advertiser | ~275 | ⚠️ **Unproven** | `Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementPublisher` | Not verified: whether Windows lets an app publish Apple (`0x004C`) manufacturer data |
| mDNS advertiser and responder | ~340 | ⚠️ | Rewrite over the TAP adapter with a normal UDP multicast socket (or Npcap on the virtual adapter), instead of `AF_PACKET` | OWL needs `--plain` mode, as on Linux |
| Sender and receiver (HTTP/TLS, plist, cpio) | ~2,000+ | ✅ Mostly portable Python | Replace `pwd`, `keyctl` and `pkexec`. Bind to the TAP adapter's scope ID | The identity and certificate store needs a Windows home (DPAPI) |
| Wi-Fi state management (NetworkManager, `iw`) | ~600 bash | ❌ Rewrite | `netsh wlan` or the WLAN API to disconnect and restore Wi-Fi in exclusive mode | — |

## 4. Proposed portable AWDL core boundaries

```text
          portable, built and tested on Windows today     |   platform, rewritten per OS
                                                           |
  awdl (OWL src/ unmodified + omdrop-owl patches)          |   radio backend (radio/radio.h)
   frame build/parse, TLVs, sync, election, chanseq,       |    mock      done, in-memory medium
   peers, tx gating, data encapsulation                    |    npcap     blocked on Gate A
        ▲                        │                         |    bridge    Linux box over USB/Ethernet
        │ awdl_rx(frame)         │ awdl_init_*_frame()     |
        │                        ▼                         |   virtual NIC (TAP-Windows6)
  scheduler / event loop  ◄── NEW, replaces daemon/core.c  |   neighbour table (iphlpapi)
   slot timing, tx queues, peer timeouts                   |   timers (waitable timer, 1 ms)
```

- **Keep `src/` unmodified** and build it with `compat/`. Upstream changes then merge straight in.
- **Write a new scheduler** in place of `daemon/core.c`. It should keep core.c's logic (when to send PSF and MIF frames, the multicast and unicast guards, and the omdrop-owl `-F`/`-S pin` behaviour) but run on an injected clock and the `RadioBackend` interface instead of libev and pcap. That makes it testable against the mock with a simulated clock. core.c is GPL-3.0, so the rewrite stays GPL-3.0.
- **`radio/radio.h`** is the only thing radio backends implement. A backend sets a capability only after a physical test shows it.
- **Network side**: a small interface (`create_l2_if`, `write_frame`, `read_frame`, `add_neighbour`, `del_neighbour`) with one Windows implementation over TAP-Windows6 and iphlpapi.
- **AirDrop side**: stays Python and a separate process, as in omdrop. It only needs a scoped IPv6 interface. That keeps the GPL-2.0-only and GPL-3.0 parts apart.

## 5. The Windows CMake project

The project is in `windows-airdrop/` and needs no Wi-Fi hardware.

- FetchContent clones the pinned OWL commit (with only the radiotap submodule) and the pinned omdrop-owl commit, applies the 6 patches, and fetches googletest v1.15.2 with its hash checked.
- Targets: `awdl` (OWL core and compat), `radiotap`, `radio_mock`, and `owl_tests`.
- Tests: OWL's 65 upstream tests (wire, sync, peers, election, channel, txgate) plus `tests/test_mock_radio.cpp`:
  - a MIF frame built by node A goes through the mock radio, OWL parses it, and node B learns A as a peer;
  - a sender doesn't hear itself, and different channels are isolated;
  - a node ignores its own frames (`RX_IGNORE_FROM_SELF`);
  - a frame cut anywhere except a TLV boundary is rejected;
  - corrupting any single byte to `0x00` or `0xff` does not crash the parser.

**Result on this PC** (Windows 11, MinGW-w64 GCC 16.2.0, CMake 4.4.4, Ninja): `100% tests passed out of 70`.

Finding from the truncation test: OWL accepts a frame that ends exactly at a TLV boundary. That's correct behaviour, because AWDL frames have no overall length field and OWL sends no FCS by default. It does mean truncation is only detectable inside a TLV.

## Unproven assumptions

None of these has been shown. Each one needs a physical or documented test before anyone relies on it.

1. **That any Windows driver can inject raw 802.11 frames.** Npcap's guide describes its raw 802.11 feature only as *capture*, through the Native WiFi API in monitor mode, and makes no claim about 802.11 injection (https://npcap.com/guide/npcap-users-guide.html). This PC's adapter reports no action-frame support.
2. That any Windows driver allows channel changes at AWDL's slot rate (about every 16 ms) while staying in monitor mode.
3. That firmware on any Windows adapter ACKs unicast to the AWDL address. On Linux this took an IBSS cell (Intel) or a station-MAC trick (MT7925).
4. That Windows timers can hold AWDL's slot timing with an acceptable error. OWL's tolerance has not been measured.
5. That TAP-Windows6 still installs and works on current Windows 11 with Secure Boot and HVCI/memory integrity on.
6. That `BluetoothLEAdvertisementPublisher` can send Apple `0x004C` manufacturer data with the AirDrop TLV.
7. That the omdrop-awdl userspace tools and omdrop-owl's scripts may be redistributed. Neither states a license for them.
8. That omdrop-plugin PR #25 (the radio-backend contract) gets merged as it stands. The contract version 0.7.0 could change.
9. That iOS keeps working with OWL. The fork's iOS 26 fixes show it can break between releases.
10. That the bit-flip test is a meaningful fuzz test. It isn't one. Run real fuzzing (libFuzzer or AFL++ with ASan) on Linux, where sanitizers work.
11. That a Linux radio bridge (WSL2 with `usbipd`, or a separate box) keeps AWDL's timing over USB/IP. This hasn't been tested.

## Reproduce

```bash
scoop install cmake ninja mingw
cmake -S windows-airdrop -B windows-airdrop/build -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build windows-airdrop/build
ctest --test-dir windows-airdrop/build
```
