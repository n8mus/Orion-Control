// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <iterator>
#include "radio/RadioController.h"

namespace ttc {

// Band stack registers, Orion-style: 4 per band (A-D). Pressing a band button
// recalls that band's last-used register; pressing it again cycles A->B->C->D.
// Each register stores frequency + mode + filter (bw/pbt) and is persisted in
// QSettings once used; the seeds below follow Jon's scheme:
//   A = CW segment, CW mode, 500 Hz filter
//   B = FT8 frequency, USB, 3 kHz, PBT 0
//   C = SSB low in the Extra phone segment, 3 kHz, sideband per band
//   D = SSB higher in the General phone segment, 3 kHz
struct StackDef {
    uint64_t hz;
    Mode     mode;
    int      bwHz;
    int      pbtHz;
};

struct BandDef {
    const char* label;      // button text
    uint64_t loHz, hiHz;    // band edges (used to highlight the current band)
    StackDef stack[4];      // A, B, C, D seeds
};

inline constexpr int kStackCount = 4;
inline constexpr char kStackNames[kStackCount] = {'A', 'B', 'C', 'D'};

inline constexpr BandDef kBands[] = {
    {"160", 1800000, 2000000,
     {{1825000, Mode::CWU, 500, 0},  {1840000, Mode::USB, 3000, 0},
      {1850000, Mode::LSB, 3000, 0}, {1900000, Mode::LSB, 3000, 0}}},
    {"80",  3500000, 4000000,
     {{3525000, Mode::CWU, 500, 0},  {3573000, Mode::USB, 3000, 0},
      {3610000, Mode::LSB, 3000, 0}, {3850000, Mode::LSB, 3000, 0}}},
    {"60",  5250000, 5450000,       // channelized, USB only — all registers USB
     {{5358500, Mode::USB, 2800, 0}, {5357000, Mode::USB, 3000, 0},
      {5358500, Mode::USB, 2800, 0}, {5403500, Mode::USB, 2800, 0}}},
    {"40",  7000000, 7300000,
     {{7025000, Mode::CWU, 500, 0},  {7074000, Mode::USB, 3000, 0},
      {7130000, Mode::LSB, 3000, 0}, {7200000, Mode::LSB, 3000, 0}}},
    {"30",  10100000, 10150000,     // CW/data only
     {{10110000, Mode::CWU, 500, 0}, {10136000, Mode::USB, 3000, 0},
      {10120000, Mode::CWU, 500, 0}, {10130000, Mode::CWU, 500, 0}}},
    {"20",  14000000, 14350000,
     {{14025000, Mode::CWU, 500, 0},  {14074000, Mode::USB, 3000, 0},
      {14160000, Mode::USB, 3000, 0}, {14250000, Mode::USB, 3000, 0}}},
    {"17",  18068000, 18168000,     // no Extra/General phone split
     {{18080000, Mode::CWU, 500, 0},  {18100000, Mode::USB, 3000, 0},
      {18120000, Mode::USB, 3000, 0}, {18140000, Mode::USB, 3000, 0}}},
    {"15",  21000000, 21450000,
     {{21025000, Mode::CWU, 500, 0},  {21074000, Mode::USB, 3000, 0},
      {21210000, Mode::USB, 3000, 0}, {21300000, Mode::USB, 3000, 0}}},
    {"12",  24890000, 24990000,
     {{24900000, Mode::CWU, 500, 0},  {24915000, Mode::USB, 3000, 0},
      {24940000, Mode::USB, 3000, 0}, {24960000, Mode::USB, 3000, 0}}},
    {"10",  28000000, 29700000,
     {{28025000, Mode::CWU, 500, 0},  {28074000, Mode::USB, 3000, 0},
      {28320000, Mode::USB, 3000, 0}, {28400000, Mode::USB, 3000, 0}}},
    {"6",   50000000, 54000000,
     {{50090000, Mode::CWU, 500, 0},  {50313000, Mode::USB, 3000, 0},
      {50125000, Mode::USB, 3000, 0}, {50200000, Mode::USB, 3000, 0}}},
};
inline constexpr int kBandCount = static_cast<int>(std::size(kBands));

inline int bandIndexOf(uint64_t hz) {
    for (int i = 0; i < kBandCount; ++i)
        if (hz >= kBands[i].loHz && hz <= kBands[i].hiHz) return i;
    return -1;
}

} // namespace ttc
