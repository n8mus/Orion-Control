# Ten-Tec SDR Console

A Flex/SmartSDR-style SDR console that gives the **Ten-Tec Orion (565/566)** and
**Omni VII (588)** full computer control plus a live **panadapter/waterfall** fed by
an **SDRplay RSP2** on the radio's SPARE antenna jack — while keeping WSJT-X, fldigi,
cqrlog and GridTracker working unchanged.

The flagship feature: **drag a passband edge on the panadapter and the radio's DSP
filter follows** (continuous on the Orion; stepped on the Omni VII — a hardware limit).

> Status: **skeleton**. The Orion CAT encoders and the `rigctld` interop server are
> real; the panadapter DSP/IQ path is stubbed pending the WDSP/SDRplay work (Phase 2).
> See [docs/architecture.md](docs/architecture.md) for the full plan and
> [docs/cat-command-reference.md](docs/cat-command-reference.md) for the protocol.

## Build (Linux / Arch)

Requires: `cmake`, `ninja`, a C++20 compiler, `qt6-base`. Optional: SDRplay API.

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/tentec-console
```

Enable the SDRplay RSP2 source (needs the SDRplay API service running):

```sh
cmake -S . -B build -G Ninja -DBUILD_SDRPLAY=ON
cmake --build build
```

## What works today

- **Ten-Tec Orion driver** (`src/radio/TenTecOrion.*`) — real CAT encoders for
  frequency, mode, bandwidth (`*RMF`), and PBT (`*RMP`), plus the `setPassband()`
  bandwidth+PBT decomposition. Serial via a POSIX wrapper integrated into the Qt loop.
- **rigctld emulation** (`src/net/RigctldServer.*`) — listens on TCP **4532** so
  cqrlog / WSJT-X / fldigi / GridTracker connect here instead of flrig. Handles
  `f/F/m/M/v/t/\chk_vfo/\dump_state` (partial — see TODOs).
- **Panadapter widget** (`src/ui/PanadapterWidget.*`) — placeholder spectrum with
  the flagship interactions wired: click-to-tune and drag-passband-edge signals.

## Layout

```
src/app/    Qt entry point + main window (wires the pieces together)
src/radio/  RadioController interface, Ten-Tec Orion driver, SerialPort
src/net/    rigctld-compatible TCP server (client interop seam)
src/sdr/    SdrSource interface + optional SDRplay RSP source
src/ui/     PanadapterWidget
```

## Next steps (Phase 0)

1. Wire the real Orion serial device (e.g. `/dev/ttyS0`) and **measure command
   round-trip latency** — it determines how the filter drag feels.
2. Turn on `-DBUILD_SDRPLAY=ON` and stream RSP2 IQ into the panadapter.
3. Vendor the NereusSDR/WDSP foundation for the real DSP + waterfall.

License: GPL-2.0-or-later (to reuse WDSP / the Thetis lineage). See [LICENSE](LICENSE).
