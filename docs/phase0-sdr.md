# Phase 0 — SDRplay RSP2 streaming (verified live)

Implemented `src/sdr/SdrPlaySource.cpp` against the SDRplay API v3.15 and verified
end-to-end on the live RSP2 (`1df7:3010`) with `tools/sdr_probe`, 2026-07-10.
Fed from the Orion's spare-antenna jack (TX-isolated), centered on 40 m.

## Configuration used

Zero-IF, 1.536 MHz analog BW, 2.0 MS/s, fixed gain (AGC off), Antenna A.

## Result

```
center 7.1500 MHz, requested fs 2.0 MS/s
samples 3,878,784 in 2.0s  =>  1.939 MS/s achieved   (matches request)
API version 3.15
```

## Key finding: the shared-antenna feed is HOT

The whole 40 m band arrives at the RSP2, so default gain overloads the ADC:

| gRdB | LNAstate | mean power | peak power | verdict          |
|------|----------|-----------|-----------|------------------|
| 40   | 0        | −6.1 dBFS | +0.7 dBFS | **ADC overload / clipping** |
| 40   | 6        | −44.7 dBFS| −21.7 dBFS| **healthy — chosen default** |
| 50   | 8        | −62.3 dBFS| −33.1 dBFS| conservative     |

`SdrPlaySource` now defaults to **gRdB 40 / LNAstate 6**. A single overload event
fires during `Init` gain-settling; steady state is clean. `setGain(gRdB, lnaState)`
overrides. When the panadapter UI lands, expose gain as a slider and watch the
`PowerOverloadChange` event to warn/auto-attenuate.

## Streaming API path (for reference)

Open → LockDeviceApi → GetDevices (match `SDRPLAY_RSP2_ID`) → SelectDevice →
UnlockDeviceApi → GetDeviceParams → set fs/rf/bw/if/gain/antenna → Init(callbacks) →
stream. `setCenterFrequency` uses `Update(Tuner_A, Update_Tuner_Frf)`. The stream
callback normalizes short I/Q by 1/32768 into `std::complex<float>` and runs on the
SDRplay thread — GUI consumers must marshal to the UI thread (queued signal).

## Next

Wire the IQ callback → FFT → `PanadapterWidget::setSpectrum` (marshaled to the GUI
thread) so the spectrum renders live around the Orion's dial.
