// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <iterator>
#include "radio/RadioController.h"

namespace ttc {

// US amateur HF/6m bands the Orion covers. Defaults are where a band button
// lands the first time; after that the per-band last frequency/mode memory
// (QSettings) takes over.
struct BandDef {
    const char* label;      // button text
    uint64_t loHz, hiHz;    // band edges (used to highlight the current band)
    uint64_t defaultHz;
    Mode     defaultMode;
};

inline constexpr BandDef kBands[] = {
    {"160", 1800000,  2000000,  1900000,  Mode::LSB},
    {"80",  3500000,  4000000,  3850000,  Mode::LSB},
    {"60",  5250000,  5450000,  5358500,  Mode::USB},
    {"40",  7000000,  7300000,  7200000,  Mode::LSB},
    {"30",  10100000, 10150000, 10120000, Mode::CWU},
    {"20",  14000000, 14350000, 14200000, Mode::USB},
    {"17",  18068000, 18168000, 18130000, Mode::USB},
    {"15",  21000000, 21450000, 21300000, Mode::USB},
    {"12",  24890000, 24990000, 24950000, Mode::USB},
    {"10",  28000000, 29700000, 28400000, Mode::USB},
    {"6",   50000000, 54000000, 50125000, Mode::USB},
};
inline constexpr int kBandCount = static_cast<int>(std::size(kBands));

inline int bandIndexOf(uint64_t hz) {
    for (int i = 0; i < kBandCount; ++i)
        if (hz >= kBands[i].loHz && hz <= kBands[i].hiHz) return i;
    return -1;
}

} // namespace ttc
