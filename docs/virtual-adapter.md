# Virtual adapter (`awdl0` on Windows)

`netif/` gives OWL's scheduler the network interface it had on Linux. It uses a **TAP-Windows6** adapter, which carries whole Ethernet frames the way OWL's Linux TAP device does. Wintun only carries IP packets, so it can't be used. The IP Helper API stands in for what OWL did with netlink.

| OWL on Linux | Windows (`netif/awdl_netif_win.c`) |
|---|---|
| `/dev/net/tun`, `IFF_TAP` | TAP-Windows6 device `\\.\Global\{GUID}.tap`, with overlapped `ReadFile`/`WriteFile` |
| `SIOCSIFFLAGS` up | `TAP_WIN_IOCTL_SET_MEDIA_STATUS` (connected) |
| Kernel's EUI-64 link-local | `CreateUnicastIpAddressEntry(fe80::<eui64(mac)>)`. This PC has `RandomizeIdentifiers: Enabled`, so Windows wouldn't derive it from the MAC on its own. |
| `rtnl_neigh_add(NUD_PERMANENT)` for each peer | `CreateIpNetEntry2(NlnsPermanent)` |
| `rfc4291_addr()` | `awdl_rfc4291_addr()`, the same bytes |
| Adapter MAC set to the AWDL address | Inverted: **the adapter's MAC is our AWDL address**, so the stack's frames already carry it. You can change it in the adapter's *MAC Address* property. |

The calls are non-blocking. `awdl_netif_read()` returns 0 when nothing is waiting, and `awdl_netif_event()` returns the handle to put in `WaitForMultipleObjects` for the real-time loop.

Constants checked against TAP-Windows6 9.27.0's own `tap-windows.h` and `OemVista.inf`: IOCTL codes, the device path, the description "TAP-Windows Adapter V9", and *Non-Admin Access* allowed by default. The 9.27.0 `dist.win10` driver catalog is signed by *Microsoft Windows Hardware Compatibility Publisher*.

## Tests

- `netif.rfc4291_link_local_matches_eui64` always runs. ✅ It passes.
- `netif.tap_round_trip_with_windows_stack` **skips unless a TAP adapter exists and the tests run as administrator.** It does what AWDL will do:
  1. brings the adapter up and adds our link-local address;
  2. adds a permanent neighbour entry for a fake peer;
  3. writes that peer's IPv6 neighbour solicitation into the adapter;
  4. expects the Windows IPv6 stack's neighbour advertisement to come back out, addressed to the peer's MAC.

  That covers both directions and the neighbour table. Neighbour discovery passes Windows Firewall on every network profile, so the firewall can't fail this test.

  **Status: not run yet.** TAP-Windows6 isn't installed on this PC; only Wintun is, from Tailscale.

## Installing TAP-Windows6 (your call: a kernel driver, needs admin)

From the official release at https://github.com/OpenVPN/tap-windows6/releases/tag/9.27.0, take `dist.win10.zip`, unzip it, and in an **administrator** terminal run:

```bash
cd dist.win10\amd64
devcon.exe install OemVista.inf tap0901
```

Then, optionally, rename the new adapter to `awdl0` (find its name with `Get-NetAdapter`):

```bash
Rename-NetAdapter -Name "<new adapter name>" -NewName awdl0
```

Run the round-trip test from an administrator terminal:

```bash
D:\pulse_it\windows-airdrop\build\owl_tests.exe --gtest_filter=netif.*
```

Remove it later with `devcon.exe remove tap0901`. Installing the OpenVPN client (`winget install OpenVPNTechnologies.OpenVPN`) also installs TAP-Windows6, but brings a whole VPN client with it.

## Known gaps

- **Windows Firewall will block AirDrop's inbound TCP** (the receiver listens on 8770/8771) on the "Unidentified network" profile the TAP adapter gets. That's for Gate C: add an allow rule scoped to this adapter.
- A fixed adapter MAC means a fixed AWDL address. iPhones randomize theirs. Rotating ours means changing the adapter property and restarting the adapter.
- Not yet tested whether the adapter loads with Memory Integrity (HVCI) on.
