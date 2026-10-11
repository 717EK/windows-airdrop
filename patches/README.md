# Patches to OWL

CMake applies these on top of omdrop-owl's patches, which go on OWL at the commit omdrop-owl pins.

| Patch | Why |
|---|---|
| `win-01-rx-data-trim.patch` | `awdl_rx_data` allocates the received Ethernet frame at `ETHER_MAX_LEN` (1518 bytes) and never trims it, so every frame goes to the host zero-padded to 1518 bytes. Linux's IPv6 stack reads the payload length and ignores the padding, which is why it hasn't shown up there. This trims the frame to the bytes actually written. It's worth sending upstream. |
