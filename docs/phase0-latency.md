# Phase 0 — Orion CAT latency measurement

Measured 2026-07-10 against the live Orion (firmware v3) on `/dev/orion`
(→ `/dev/ttyUSB0`, an **FTDI FT4232H** USB-serial bridge), 57600 8N1, read-only
queries via `tools/latency_probe.c`. 50 samples each.

## Results (FTDI latency_timer = 16 ms, the default)

| query  | response      | min    | median | mean    | p90     | max     |
|--------|---------------|--------|--------|---------|---------|---------|
| `?RMF` | `@RMF3000`    | 10.9ms | 26.9ms | 47.8ms  | 107.0ms | 122.9ms |
| `?AF`  | `@AF07111900` | 26.8ms | 26.9ms | 55.2ms  | 122.9ms | 123.0ms |

Radio confirmed live: main filter 3000 Hz, VFO-A on 7.1119 MHz.

## Interpretation

- Round-trip is **bimodal**: a cluster near ~27 ms and a tail out past ~120 ms.
  That signature is the **FTDI 16 ms latency timer** beating against USB frame
  timing plus the Orion's DragonBall processing — not the 57600 link itself
  (a short command is only ~1.5 ms of wire time).
- **This does not gate the drag-to-filter feature.** Orion `*RMF`/`*RMP` sets are
  fire-and-forget (no ACK), so a drag just *streams* set commands (~1.5 ms TX each),
  coalesced to ~20-30/sec — well within budget. The round-trip figures above only
  bound how fast a *reconciliation query* confirms state, not the filter response.

## Recommended tuning

Drop the FTDI latency timer to 1 ms to collapse the tail and tighten reconciliation:

```sh
# one-shot (until replug):
echo 1 | sudo tee /sys/bus/usb-serial/devices/ttyUSB0/latency_timer

# persistent (udev): /etc/udev/rules.d/99-tentec-ftdi.rules
SUBSYSTEM=="usb-serial", DRIVER=="ftdi_sio", ATTR{latency_timer}="1"
```

Then re-run: `./latency_probe /dev/orion 50 ?RMF` — expect median to drop toward
single-digit ms and the >100 ms tail to largely vanish.

> Note: the Orion reaches the PC via an FTDI FT4232H, i.e. a USB-serial bridge —
> worth flagging since the earlier assumption was a native RS-232 UART. Doesn't
> change the driver, but the FTDI timer tuning above is specific to it.
