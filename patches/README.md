# Upstream patch series

CMake applies the pinned omdrop-owl series, then sorted `win-*.patch` files.

- win-01: trim received data before Ethernet conversion.
- win-02: initialize receive flags.
- win-03: peer liveness support.
- win-04: receive output capacity bounds.
- win-05: narrowly allow final zero action padding.
- win-06: reject oversized Ethernet output payloads.

OWL-derived patches retain GPL-3.0-or-later licensing. The separate
`devourer-0*.patch` files are GPL-2.0-only and are applied in order by
`tools/build-devourer.sh`. See the root license map and architecture guide.

- devourer-01: MinGW build fork of the txdemo example.
- devourer-02: Jaguar2 (8812BU/8822B) 40/80 MHz. The baseband and
  REG_CCKSB took devourer's slice position index where they expect the
  vendor sub-channel code, so the 80 MHz primary for channel 149 sat on
  157; the MAC half of the width (REG_DATA_SC, REG_WMAC_TRXPTCL_CTL RFMOD),
  which hardware ACK/BlockAck responses use, was never programmed.
- devourer-03: Jaguar2 receive digital filter values per width (the
  20 MHz values were always used), following the vendor
  `phydm_rxdfirpar_by_bw_8822b`.
