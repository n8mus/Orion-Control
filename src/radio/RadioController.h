// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>

namespace ttc {

enum class Rx { Main, Sub };

enum class Mode { USB, LSB, CWU, CWL, AM, FM };

// What a given radio can actually do with its receive filter. Drives how the
// panadapter passband-drag gesture is quantized. See docs/cat-command-reference.md.
struct CapabilityProfile {
    std::string name;
    int   bandwidthMinHz   = 100;
    int   bandwidthMaxHz   = 6000;
    int   bandwidthStepHz  = 1;      // Orion: 1 (continuous). Omni VII: preset ladder.
    int   pbtRangeHz       = 8000;   // ± range
    bool  continuousFilter = true;   // false => snap width to a preset table
    bool  dualReceiver     = true;
    bool  needsHwHandshake = false;  // Omni VII serial requires RTS/CTS
};

// Radio-agnostic control surface. The Ten-Tec serial drivers implement this;
// the rigctld server and the panadapter UI talk only to this interface.
class RadioController {
public:
    virtual ~RadioController() = default;

    virtual const CapabilityProfile& caps() const = 0;
    virtual bool connected() const = 0;

    virtual void setFrequencyHz(Rx rx, uint64_t hz) = 0;
    virtual void setMode(Rx rx, Mode mode) = 0;

    // The flagship abstraction: two panadapter filter edges, as offsets in Hz
    // from the carrier/pitch. Implementations decompose to bandwidth + PBT.
    virtual void setPassband(Rx rx, int loEdgeHz, int hiEdgeHz) = 0;

    // Lower-level knobs (also used by setPassband internally).
    virtual void setBandwidthHz(Rx rx, int bwHz) = 0;
    virtual void setPbtHz(Rx rx, int pbtHz) = 0;
};

} // namespace ttc
