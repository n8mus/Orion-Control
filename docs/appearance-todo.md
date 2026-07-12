# Appearance upgrade TODO

Review vs KE9NS PowerSDR (meter options) and Thetis (world map, solar data,
compass rose) — Jon's screenshot reference, 2026-07-12. Rules of the road:
meters are drawn as **vector QPainter faces** (no bitmap skins copied from
anyone), map imagery is fetched from **NASA Visible Earth directly** (public
domain), no third-party asset files enter the repo.

## 1. S-meter suite (the "bland meter" fix) — DONE (on-air validated)
- [x] **Meter style picker**: click the meter cycles Bar / Analog / Edge /
      Digital, persisted.
- [x] **Analog needle face**: vector ivory face, black S1–S9 arc, red over-S9
      arc, 30 fps ballistics (fast attack / slow decay), peak ghost needle.
      TX face: 0–120 W with red zone + SWR readout.
- [x] **Digital readout**: dBm / S-units / µV from the calibrated value.
- [x] **RX meter modes**: right-click cycles Signal / Average / Peak, chip on
      the face.
- [x] **TX faces** on every style (watts + SWR + reflected on digital).

## 2. World map backgrounds (map pack + controls) — DONE
- [x] **Bundled NASA basemaps**: Map: Classic (Blue Marble), Map: Vegetation
      (BMNG July topo/bathy — the "Thetis look"), Map: Night lights (Black
      Marble 2016). All fetched from NASA Visible Earth, public domain.
- [x] **MAP DAY / MAP NIGHT brightness sliders** in DISPLAY (30–120 % day,
      0–60 % night), enabled only when a map backdrop is active.
- [x] **Map: Custom…** entry opens a file picker (any image, persisted path;
      cancel bounces back to the previous backdrop).
- [x] Grayline on every variant; terminator smoothsteps widened for a soft
      Thetis-style blend.

## 3. Solar / space-weather layer — DONE
- [x] **Sun marker on the map** at the subsolar point: orange glow + core,
      `SFI nnn  A nn  K n.n` caption (flips side near the map edge).
- [x] **Solar-Terrestrial data panel** bottom-right of the spectrum
      (green-on-black, DISPLAY "Solar data panel" toggle): SFI, SSN, A, K,
      X-ray class. NOAA SWPC (wwv.txt + daily indices + GOES X-ray JSON),
      polled every 15 min, last good values kept, fully async.
- [ ] Optional: band-condition hint line derived from the same data.

## 4. Azimuth compass rose — DONE
- [x] Azimuthal-equidistant world disc bottom-left of the spectrum, centered
      on the station grid (DISPLAY → GRID field, station/grid setting),
      day/night tinted, sun-bearing tick on the ring, N/E/S/W labels.
- [x] Click a spot label → rose swings to that station with bearing° +
      distance km (POTA park coords from the API; DX calls placed via the
      bundled AD1C cty.dat). Click inside the rose = manual heading;
      right-click clears. DISPLAY "Compass rose" toggle, persisted.
      pointRoseAt() is public for future rotor integration.

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
