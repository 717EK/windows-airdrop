# Upstream patch series

CMake applies the pinned omdrop-owl series, then sorted `win-*.patch` files.

- win-01: trim received data before Ethernet conversion.
- win-02: initialize receive flags.
- win-03: peer liveness support.
- win-04: receive output capacity bounds.
- win-05: narrowly allow final zero action padding.
- win-06: reject oversized Ethernet output payloads.

OWL-derived patches retain GPL-3.0-or-later licensing. The separate
`devourer-01-mingw-txdemo-fork.patch` is GPL-2.0-only and is applied by
`tools/build-devourer.sh`. See the root license map and architecture guide.
