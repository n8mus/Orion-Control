// SPDX-License-Identifier: GPL-2.0-or-later
// The contest registry. One entry per contest, readable top-to-bottom
// like the rules it encodes. Sources: each sponsor's published rules and
// the WWROF Cabrillo tag registry — never another logger's code.
#include "contest/ContestDef.h"

#include "util/CtyLookup.h"

namespace ttc {

namespace {

// Mult keys: "C:<country>|<band>" per-band country, "Z:<zone>|<band>"
// per-band zone, "CALL:<call>" whole-contest unique call. The band rides
// inside the key so computeScore needs no scope logic of its own.
QString bandOf(const QString& multKey) {
    const int bar = multKey.lastIndexOf('|');
    return bar < 0 ? QString() : multKey.mid(bar + 1);
}

// F12 is WIPE everywhere — a reserved action key, not a macro (operator's
// ruling: no chorded shortcuts in contest mode, so wipe rides an F-key).
const QHash<int, QString> kBaseRun = {
    {1, "CQ|cq {MYCALL} {MYCALL}"},
    {2, "His Call|{HISCALL}"},
    {3, "Exch|{SNT} {EXCH}"},
    {4, "TU|tu {MYCALL}"},
    {5, "My Call|{MYCALL}"},
    {9, "AGN|agn"},
    {10, "NR?|nr?"},
};

QHash<int, QString> withOverrides(QHash<int, QString> base,
                                  const QHash<int, QString>& over) {
    for (auto it = over.constBegin(); it != over.constEnd(); ++it)
        base.insert(it.key(), it.value());
    return base;
}

// ---------------------------------------------------------------- CW Open
ContestDef makeCwOpen() {
    ContestDef d;
    d.id = "CW-OPEN";
    d.title = "CW Open (CWops)";
    d.cabrilloName = "CW-OPEN";
    d.modeCategory = "CW";
    d.dupe = DupeScope::PerBand;
    d.hasRst = false;
    d.sentSerial = true;
    d.sentExchDefault = "Jon";
    d.fields = {
        {ExchCol::SerialR, "RCV NR", 6, "", true, ""},
        {ExchCol::Exch1, "NAME", 10, "", true, "name"},
    };
    d.points = [](const CQsoValues&, const CtyInfo&, bool,
                  const ContestContext&) { return 1; };
    d.mults = [](const CQsoValues& q, const CtyInfo&, bool,
                 const ContestContext&) {
        return QStringList{"CALL:" + q.call};
    };
    d.cabExch = {"serial", "exch"};
    d.fkeyRun = withOverrides(kBaseRun, {
        {1, "CQ|cq cwo {MYCALL}"},
        {3, "Exch|{SENTNR} {EXCH}"},
        {8, "NAME?|name?"},
    });
    return d;
}

// -------------------------------------------------------------------- CWT
ContestDef makeCwt() {
    ContestDef d;
    d.id = "CW-OPS";
    d.title = "CWops CWT";
    d.cabrilloName = "CW-OPS";
    d.modeCategory = "CW";
    d.dupe = DupeScope::PerBand;
    d.hasRst = false;
    d.sentSerial = false;
    d.sentExchDefault = "Jon MI";   // non-member: name + state
    d.fields = {
        {ExchCol::Exch1, "NAME", 10, "", true, "name"},
        {ExchCol::Exch2, "NR / STATE", 8, "", true, "exch1"},
    };
    d.points = [](const CQsoValues&, const CtyInfo&, bool,
                  const ContestContext&) { return 1; };
    d.mults = [](const CQsoValues& q, const CtyInfo&, bool,
                 const ContestContext&) {
        return QStringList{"CALL:" + q.call};
    };
    d.cabExch = {"exch"};
    d.fkeyRun = withOverrides(kBaseRun, {
        {1, "CQ|cq cwt {MYCALL}"},
        {3, "Exch|{EXCH}"},
        {8, "NAME?|name?"},
    });
    return d;
}

// --------------------------------------------------------------- ICWC MST
ContestDef makeMst() {
    ContestDef d;
    d.id = "ICWC-MST";
    d.title = "ICWC MST (Medium Speed Test)";
    d.cabrilloName = "ICWC-MST";
    d.modeCategory = "CW";
    d.dupe = DupeScope::PerBand;
    d.hasRst = false;
    d.sentSerial = true;
    d.sentExchDefault = "Jon";
    d.fields = {
        {ExchCol::Exch1, "NAME", 10, "", true, "name"},
        {ExchCol::SerialR, "RCV NR", 6, "", true, ""},
    };
    d.points = [](const CQsoValues&, const CtyInfo&, bool,
                  const ContestContext&) { return 1; };
    d.mults = [](const CQsoValues& q, const CtyInfo&, bool,
                 const ContestContext&) {
        return QStringList{"CALL:" + q.call};
    };
    d.cabExch = {"exch", "serial"};
    // MST is the skill-building crowd — plain digits, no cut numbers.
    d.cutNumbers = false;
    d.serialPad = 1;
    d.fkeyRun = withOverrides(kBaseRun, {
        {1, "CQ|cq mst {MYCALL}"},
        {3, "Exch|{EXCH} {SENTNR}"},
        {8, "NAME?|name?"},
    });
    return d;
}

// --------------------------------------------------------------- CQ WW CW
ContestDef makeCqWwCw() {
    ContestDef d;
    d.id = "CQ-WW-CW";
    d.title = "CQ WW DX CW";
    d.cabrilloName = "CQ-WW-CW";
    d.modeCategory = "CW";
    d.dupe = DupeScope::PerBand;
    d.hasRst = true;
    d.sentSerial = false;
    d.sentExchDefault = "4";        // my CQ zone
    d.fields = {
        {ExchCol::RstR, "RCV", 4, "599", true},
        {ExchCol::Exch1, "ZONE", 4, "", true},
    };
    // Own country 0 (still logs, still counts the mults), same continent
    // 1 — except NA-to-NA which CQ pays 2 — other continent 3.
    d.points = [](const CQsoValues&, const CtyInfo& ci, bool ok,
                  const ContestContext& ctx) {
        if (!ok) return 3;          // cty miss: assume the best, robot decides
        if (ci.country == ctx.myCountry) return 0;
        if (ci.cont == ctx.myCont)
            return ci.cont == QLatin1String("NA") ? 2 : 1;
        return 3;
    };
    // Zone AND country, both per band, both weight 1. Zone comes from the
    // COPIED exchange (that is what the robot cross-checks), country from
    // cty; zero-point own-country QSOs still claim both.
    d.mults = [](const CQsoValues& q, const CtyInfo& ci, bool ok,
                 const ContestContext&) {
        QStringList keys;
        const QString z = q.exch1.trimmed();
        if (!z.isEmpty()) keys << "Z:" + z + "|" + q.band;
        if (ok) keys << "C:" + ci.country + "|" + q.band;
        return keys;
    };
    d.cabExch = {"rst", "exch"};
    d.fkeyRun = withOverrides(kBaseRun, {
        {1, "CQ|cq test {MYCALL} {MYCALL}"},
    });
    return d;
}

// -------------------------------------------------------------- WAE DX CW
// QSO/mult rules only in phase 1 — the QTC machinery is its own build.
ContestDef makeWaeCw() {
    ContestDef d;
    d.id = "DARC-WAEDC-CW";
    d.title = "WAE DX CW";
    d.cabrilloName = "DARC-WAEDC-CW";
    d.modeCategory = "CW";
    d.dupe = DupeScope::PerBand;
    d.hasRst = true;
    d.sentSerial = true;
    d.sentExchDefault = "";
    d.fields = {
        {ExchCol::RstR, "RCV", 4, "599", true},
        {ExchCol::SerialR, "RCV NR", 6, "", true},
    };
    // Exactly one side European, and 160 m is not a WAE band. From the US
    // that means EU only; everything else logs at zero.
    const auto valid = [](const CtyInfo& ci, bool ok,
                          const ContestContext& ctx, const QString& band) {
        if (!ok || band == QLatin1String("160M")) return false;
        const bool theirsEu = ci.cont == QLatin1String("EU");
        const bool mineEu = ctx.myCont == QLatin1String("EU");
        return theirsEu != mineEu;
    };
    d.points = [valid](const CQsoValues& q, const CtyInfo& ci, bool ok,
                       const ContestContext& ctx) {
        return valid(ci, ok, ctx, q.band) ? 1 : 0;
    };
    d.mults = [valid](const CQsoValues& q, const CtyInfo& ci, bool ok,
                      const ContestContext& ctx) {
        if (!valid(ci, ok, ctx, q.band)) return QStringList{};
        return QStringList{"C:" + ci.country + "|" + q.band};
    };
    d.multWeight = [](const QString& key) {
        const QString b = bandOf(key);
        if (b == QLatin1String("80M")) return 4;
        if (b == QLatin1String("40M")) return 3;
        return 2;                   // 20/15/10
    };
    d.cabExch = {"rst", "serial"};
    d.fkeyRun = withOverrides(kBaseRun, {
        {1, "CQ|cq wae {MYCALL} {MYCALL} wae"},
        {3, "Exch|{SNT} {SENTNR}"},
        {4, "TU|tu {MYCALL} wae"},
        {6, "QTC?|qtc?"},
        {7, "QRV|qrv"},
        {8, "QSL|qsl"},
        {11, "Serial|{SENTNR}"},
    });
    return d;
}

// -------------------------------------------------- generic RST + serial
// The day-of fallback for a contest with no definition yet: correct
// dupes, a usable log, a parseable generic Cabrillo. Its proper
// definition gets written the week after.
ContestDef makeGeneric() {
    ContestDef d;
    d.id = "GENERIC-SERIAL";
    d.title = "Generic (RST + serial)";
    d.cabrilloName = "UNKNOWN";
    d.modeCategory = "CW";
    d.dupe = DupeScope::PerBand;
    d.hasRst = true;
    d.sentSerial = true;
    d.sentExchDefault = "";
    d.fields = {
        {ExchCol::RstR, "RCV", 4, "599", true},
        {ExchCol::SerialR, "RCV NR", 6, "", false},
    };
    d.points = [](const CQsoValues&, const CtyInfo&, bool,
                  const ContestContext&) { return 1; };
    d.mults = [](const CQsoValues& q, const CtyInfo& ci, bool ok,
                 const ContestContext&) {
        if (!ok) return QStringList{};
        return QStringList{"C:" + ci.country + "|" + q.band};
    };
    d.cabExch = {"rst", "serial"};
    d.fkeyRun = kBaseRun;
    return d;
}

} // namespace

const QList<const ContestDef*>& contestDefs() {
    static const ContestDef cwOpen = makeCwOpen();
    static const ContestDef cwt = makeCwt();
    static const ContestDef mst = makeMst();
    static const ContestDef cqww = makeCqWwCw();
    static const ContestDef wae = makeWaeCw();
    static const ContestDef generic = makeGeneric();
    static const QList<const ContestDef*> all = {
        &cqww, &wae, &cwOpen, &cwt, &mst, &generic,
    };
    return all;
}

const ContestDef* contestDef(const QString& id) {
    for (const ContestDef* d : contestDefs())
        if (d->id == id) return d;
    return nullptr;
}

} // namespace ttc
