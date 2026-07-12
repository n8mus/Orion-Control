# Appearance upgrade TODO

Review vs KE9NS PowerSDR (meter options) and Thetis (world map, solar data,
compass rose) — Jon's screenshot reference, 2026-07-12. Rules of the road:
meters are drawn as **vector QPainter faces** (no bitmap skins copied from
anyone), map imagery is fetched from **NASA Visible Earth directly** (public
domain), no third-party asset files enter the repo.

## 1. S-meter suite (the "bland meter" fix) — biggest visual win
- [ ] **Meter style picker** (DISPLAY menu or click the meter to cycle),
      persisted: `Bar` (current), `Analog needle`, `Edge needle`, `Digital`.
- [ ] **Analog needle face**: ivory/backlit cross-needle look drawn in
      QPainter — arc scale S1–S9+60 dB, red zone above S9, needle with
      ballistics (fast attack ~50 ms, slow decay ~500 ms), subtle glass
      highlight. TX face swaps to PO (W) with SWR/ALC secondary arcs.
- [ ] **Digital readout row** under/next to the meter, Thetis style:
      `-74.4 dBm   S 9   42.6 µV` — all three derived from the same
      calibrated dB-rel-S9 value we already compute.
- [ ] **RX meter modes**: Signal / Signal Average / Signal Peak (KE9NS
      MeterRXMode) — mode label shown on the face.
- [ ] **TX meter modes**: Forward / SWR / Combo (needle = watts, inset
      digits = SWR) — we already flip faces on T/R, extend to the new styles.

## 2. World map backgrounds (map pack + controls)
- [ ] **Bundle 3–4 NASA equirectangular basemaps** (all public domain,
      downloaded from NASA Visible Earth / Blue Marble):
      - Blue Marble Next Generation (vegetation/topo — the "Thetis look")
      - Black Marble / Earth at Night (city lights — great on a dark console)
      - Topography/bathymetry relief
      - current Blue Marble classic (keep)
- [ ] **Map brightness slider** in DISPLAY (current fixed 78 %/19 % dimming is
      too dark next to the Thetis shot) — separate day/night levels.
- [ ] **Custom image loader** (Thetis-style): DISPLAY → "Load background…"
      file picker, any user image, persisted path.
- [ ] Keep the live grayline/terminator on every map variant; soften the
      terminator band edge (gradient instead of the current hard ramp).

## 3. Solar / space-weather layer
- [ ] **Sun marker on the map** at the subsolar point (we already compute it
      for the grayline): small orange sun icon with `SFI nnn  A nn  Kn`
      caption, KE9NS/Thetis style.
- [ ] **Solar-Terrestrial data panel** (green-on-black text block, corner of
      the panadapter or a DISPLAY toggle): SFI, sunspot number, A-index,
      K-index, X-ray class. Source: NOAA SWPC JSON (or hamqsl.com XML);
      poll ~15 min, cache last good values, never block the UI.
- [ ] Optional: band-condition hint line derived from the same data.

## 4. Azimuth compass rose (Thetis bottom-left widget)
- [ ] Small azimuthal-equidistant world disc centered on Jon's QTH with a
      beam-heading pointer and degree readout.
- [ ] Click a spot label (or the map) → rose shows the great-circle bearing
      to it. Pure display for now — no rotor control wired.

## 5. VFO / top-strip cosmetics
- [ ] Band label under each VFO readout ("40m band"), auto from kBands.
- [ ] Larger amber digit option for the VFO readouts (Thetis-scale digits),
      sized via a DISPLAY setting.
- [ ] Split badge: when B carries TX, show a "SPLIT" tag by the readouts
      (we color-code today, but the word reads faster mid-pileup).

## 6. Smaller polish items spotted in the comparison
- [ ] Meter smoothing option (Sig Avg) applied to the bar style too.
- [ ] Local time + UTC clock line in the top strip (Thetis shows both).
- [ ] Frequency-scale band-plan shading (CW/data/phone segments tinted under
      the scale strip, like the 60 m channel boxes we already draw).
- [ ] Spectrum trace color / line-width options in DISPLAY.

Suggested order: 1 → 2 → 3 (map + solar feel like one feature), then 5, 4, 6.
