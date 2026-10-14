// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <array>
#include <vector>

#include "dsp/ClientEq.h"

namespace ttc {

// Voice-keyer EQ (phase 1 of the PC voice chain): the operator's recorded
// VK messages are shaped offline, at record time and whenever the curve
// changes, with the ClientEq parametric EQ ported from AetherSDR. Each
// slot keeps its untouched take (vkN.raw.wav) and the console renders the
// message that actually plays and transmits (vkN.wav) from it, so the
// curve can be re-tuned and re-auditioned without re-recording.
//
// The layout is fixed and voice-shaped rather than free-form — eight
// slots in the order a broadcast EQ runs them:
//   0 high-pass   1 low shelf   2..5 peaks   6 high shelf   7 low-pass
// The Orion still applies its own TX bandwidth (900-3900 Hz) and LF
// rolloff after this, so the EQ shapes tone; the radio sets the edges.
struct VoiceEqSettings {
    static constexpr int kBands = 8;
    bool enabled = false;
    std::array<ClientEq::BandParams, kBands> bands{};

    // Slot types never change; a stored band of the wrong type is ignored.
    static ClientEq::FilterType slotType(int i);
    static QString slotName(int i);                // "High-pass", "Peak 1" …

    // Starting points to tune by ear on the MON, not measured answers.
    // DxPunch is fitted to the Heil DX target inside a 2.4 kHz TX filter;
    // EssbSmooth keeps flat lows and a broad high lift for 50-3900 Hz;
    // Shure444 is fitted to the Shure 444's response (the RE20/RE320 are
    // near-flat, so this lays the 444's colour over them).
    enum class Preset { Flat = 0, SsbVoice = 1, WideSsb = 2, DxPunch = 3,
                        EssbSmooth = 4, Shure444 = 5 };
    static QStringList presetNames();
    static VoiceEqSettings preset(Preset p);

    QJsonObject toJson() const;
    static VoiceEqSettings fromJson(const QJsonObject& o);
    static VoiceEqSettings load();                 // QSettings vk/eq (default SSB)
    void save() const;

    float responseDb(float hz) const;              // whole curve, for the plot
};

// Reference mic/speech target curves drawn behind the EQ plot as a shape
// to steer toward (display only — never applied). Data from AetherSDR's
// src/gui/ClientEqCurveWidget.cpp (GPL-3, KK7GWY and contributors), which
// digitised them from their sources: AT&T 1959 speech-transmission
// optimum, Heil's DX/contest target, and the Astatic D-104, Shure 444 and
// Heil HC-5 responses. Index 0 = none.
struct VoiceEqTarget {
    struct Pt { float hz, db; };
    QString name;
    std::vector<Pt> pts;
    float dbAt(float hz) const;                    // log-frequency interpolation
};
const std::vector<VoiceEqTarget>& voiceEqTargets();

// Render inWav (any 16-bit PCM WAV) through the EQ to outWav, then peak-
// normalize the result to targetPeak so the EQ never changes TX drive
// (cuts would starve the ALC, boosts would clip). With the EQ disabled the
// output is the input, normalized. The output is written to a temp file
// and renamed into place, so a message is never left half-written.
bool renderVoiceEq(const QString& inWav, const QString& outWav,
                   const VoiceEqSettings& s, double targetPeak = 0.7,
                   QString* error = nullptr);

} // namespace ttc
