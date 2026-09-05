// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include "contest/ContestDef.h"

namespace ttc {

class CtyLookup;

// Pure contest arithmetic — no database, no widgets, so contesttest can
// hammer it without Qt GUI or fixtures.

// Strip portable decorations so cty.dat answers for the COUNTRY being
// operated from: N8EM/DL and DL/N8EM both resolve as DL; a bare area
// digit (W1AW/4) keeps the call side because cty cannot resolve "4".
QString normalizeForCty(const QString& call);

// A call the log would accept: 3+ chars, at least one letter AND one
// digit. Everything else gets a fill request instead of a log action —
// upstream's silent save_contact drop, made visible.
bool loggableCall(const QString& call);

// "001" -> "TT1" on the air (cut zeros/nines, zero-padded). Cabrillo
// always receives plain digits — this is keying-side formatting only.
QString formatSerial(int n, bool cut, int pad);

// One row of a computed score.
struct BandCount {
    int qsos = 0, points = 0, mults = 0, weighted = 0;
};

struct ScoreBreakdown {
    int qsos = 0;                       // rows in the log
    int points = 0;                     // sum of per-QSO points
    int mults = 0;                      // distinct mult keys
    int weightedMults = 0;              // with the def's per-key weight
    qint64 total = 0;                   // points × weightedMults (min 1×)
    QHash<QString, BandCount> perBand;  // "40M" -> counts
    QSet<QString> multKeys;             // for "is this spot a new mult?"
};

// Recompute everything from row 1. A few thousand QSOs is microseconds;
// incremental patching is where mult bugs live (a stuck dupe flag once
// zeroed a whole log's points on the old system).
ScoreBreakdown computeScore(const ContestDef& def,
                            const QList<CQsoValues>& qsos,
                            const CtyLookup* cty,
                            const ContestContext& ctx);

// Would this call be a dupe against the worked list, under the def's
// scope? band/mode are the CURRENT rig state.
bool isDupe(const ContestDef& def, const QList<CQsoValues>& qsos,
            const QString& call, const QString& band, const QString& mode);

// Expand {MYCALL} {HISCALL} {SNT} {SENTNR} {EXCH} {NAME} in a macro.
// Unknown tokens key literally — an operator typo should be audible on
// the sidetone, not silently swallowed.
QString expandMacro(QString text, const ContestDef& def,
                    const ContestContext& ctx, const QString& hisCall,
                    const QString& sentExch, int sentSerial);

} // namespace ttc
