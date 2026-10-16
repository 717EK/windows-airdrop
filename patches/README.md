# Patches to OWL

CMake applies these on top of omdrop-owl's patches, which go on OWL at the commit omdrop-owl pins.

| Patch | Why |
|---|---|
| `win-01-rx-data-trim.patch` | `awdl_rx_data` allocates the received Ethernet frame at `ETHER_MAX_LEN` (1518 bytes) and never trims it, so every frame goes to the host zero-padded to 1518 bytes. Linux's IPv6 stack reads the payload length and ignores the padding, which is why it hasn't shown up there. This trims the frame to the bytes actually written. It's worth sending upstream. |
| `win-02-rx-flags-init.patch` | `awdl_rx` reads its radiotap `flags` uninitialized when the capture has no Flags field, so `check_fcs` sees garbage and drops valid frames as `RX_IGNORE_FAILED_CRC`. Found by replaying real iPhone captures from devourer: all 580 AWDL frames were rejected before this fix and accepted after it. Worth sending upstream. |

`devourer-01-mingw-txdemo-fork.patch` is for devourer, not OWL. `tools/build-devourer.sh` applies it (see docs/devourer-windows.md).
