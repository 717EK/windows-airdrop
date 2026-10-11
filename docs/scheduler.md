# AWDL scheduler

`sched/` replaces the timing half of OWL's `daemon/core.c`. The rest of core.c (libev, libpcap, the TAP device, nl80211) is Linux plumbing, and doesn't come along.

## What it does

The scheduler keeps six timers, each with the same rule as in core.c:

| Timer | When it fires | What it does |
|---|---|---|
| Channel | At every availability window | Moves the radio to the slot's channel. It only records the move if the radio accepts it. |
| PSF | Every `psf_interval` TU | Sends a periodic synchronization frame |
| MIF | In the middle of every extended availability window | Sends a master indication frame, when the current channel is set |
| Peers | Every `clean_interval` | Drops stale peers, runs the election, then adopts a channel sequence (`-S` strategies: verbatim, intersect, widen, rotate, pin) |
| Unicast | When the peer is on our channel, outside the 3 TU guard | Sends the one pending frame. A frame to a stranger is dropped, and a frame to ourselves loops back. |
| Multicast | In a multicast window, outside the 16 TU guard (3 TU with `-F`) | Sends from a 16-frame queue |

The channel-sequence adoption code is copied verbatim from the patched core.c, with only the state type renamed. Everything else is a direct port.

## How to drive it

The caller owns the loop, so there's no event library:

```c
awdl_sched_init(&s, "name", &mac, CHAN_OPCLASS_6, &radio_backend, radio_ctx, deliver_cb, ctx);
for (;;) {
    uint64_t next = awdl_sched_tick(&s);            /* run what's due; returns next due time (us) */
    /* wait until `next`, a radio frame, or a frame from the virtual NIC, then: */
    awdl_sched_radio_rx(&s, frame, len);            /* radiotap + 802.11 from the radio */
    awdl_sched_host_tx(&s, eth, len);               /* Ethernet from awdl0; -1 means full, retry */
}
```

Time comes from `clock_time_us()`. `awdl_clock_set()` swaps in a simulated clock, which OWL's core also reads, so frame timestamps and scheduling decisions use the same time.

## Tests (`tests/test_sched.cpp`)

These tests run on a simulated clock: the loop jumps from one due time to the next, so 3 seconds of AWDL runs in about 1 ms.

- Two nodes started 37 ms apart find each other, agree on a sync master, and end up in the same availability window.
- PSF and MIF frames go out at the expected rate.
- A unicast frame reaches the peer byte for byte, and so does a multicast frame.
- When the peer is on our channel only in odd slots, 64 frames are injected at every phase of a slot. Each one waits for the peer's window and stays clear of the guard at its edges, and every one arrives. This is checked at the moment each frame leaves. If the send gate is removed, this test fails.
- A frame to a stranger is dropped, and a frame to ourselves loops back.
- A second unicast is refused while one is pending, and a frame too short for an Ethernet header is rejected.
- With an alternating channel sequence, the radio switches once per slot.
- A peer that goes quiet times out.

## Found along the way

`awdl_rx_data` in OWL hands the host a frame zero-padded to 1518 bytes. `patches/win-01-rx-data-trim.patch` fixes that, and `patches/README.md` explains it.

## Not done, and why

- **No real-time loop.** It would need a radio to wait on, and none exists yet (Gate A). When one does, it's about 30 lines: a high-resolution waitable timer plus the radio's and the TAP adapter's wait handles in `WaitForMultipleObjects`.
- **No stats timer or pcap dump.** Counters are still kept in `awdl_state.stats`, so the caller can log them.
- **The mock has no airtime, loss or ACKs,** so these tests prove the logic, not real timing. Whether Windows can hold AWDL's timing on real hardware remains unproven assumption 4 in the audit.
