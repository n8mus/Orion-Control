// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/CtyLookup.h"

#include <QFile>

namespace ttc {

// cty.dat: a country header line
//   "United States:  05:  08:  NA:   39.00:    98.00:     5.0:  K:"
// followed by continuation lines of comma-separated aliases ending in ';'.
// Aliases may carry decorations — (cq)[itu]<lat/lon>{cont}~tz~ — which are
// stripped; "=CALL" marks an exact-callsign entry.
bool CtyLookup::load(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    prefixes_.clear();
    exact_.clear();
    float lat = 0.0f, lon = 0.0f;
    bool haveCountry = false;
    while (!f.atEnd()) {
        const QString line = QString::fromLatin1(f.readLine());
        if (line.isEmpty()) continue;
        if (!line.startsWith(' ') && line.contains(':')) {   // country header
            const QStringList fields = line.split(':');
            if (fields.size() >= 7) {
                lat = fields[4].trimmed().toFloat();
                lon = -fields[5].trimmed().toFloat();        // west-positive -> east
                haveCountry = true;
            }
            continue;
        }
        if (!haveCountry) continue;
        for (QString tok : line.trimmed().remove(';').split(',', Qt::SkipEmptyParts)) {
            // Strip per-alias override decorations.
            for (const QChar cut : {QChar('('), QChar('['), QChar('<'),
                                    QChar('{'), QChar('~')}) {
                const int i = tok.indexOf(cut);
                if (i >= 0) tok.truncate(i);
            }
            tok = tok.trimmed().toUpper();
            if (tok.isEmpty()) continue;
            if (tok.startsWith('=')) exact_.insert(tok.mid(1), {lat, lon});
            else                     prefixes_.push_back({tok, lat, lon});
        }
    }
    return !prefixes_.empty();
}

bool CtyLookup::lookup(const QString& call, double& lat, double& lon) const {
    const QString c = call.toUpper();
    if (const auto it = exact_.constFind(c); it != exact_.constEnd()) {
        lat = it->first;
        lon = it->second;
        return true;
    }
    int bestLen = 0;
    const Ent* best = nullptr;
    for (const Ent& e : prefixes_)
        if (e.pfx.size() > bestLen && c.startsWith(e.pfx)) {
            best = &e;
            bestLen = e.pfx.size();
        }
    if (!best) return false;
    lat = best->lat;
    lon = best->lon;
    return true;
}

bool CtyLookup::gridToLatLon(const QString& grid, double& lat, double& lon) {
    const QString g = grid.trimmed().toUpper();
    if (g.size() < 4 || !g[0].isLetter() || !g[1].isLetter()
        || !g[2].isDigit() || !g[3].isDigit())
        return false;
    lon = (g[0].toLatin1() - 'A') * 20.0 - 180.0 + (g[2].toLatin1() - '0') * 2.0;
    lat = (g[1].toLatin1() - 'A') * 10.0 - 90.0  + (g[3].toLatin1() - '0') * 1.0;
    if (g.size() >= 6 && g[4].isLetter() && g[5].isLetter()) {
        lon += (g[4].toLatin1() - 'A') * 2.0 / 24.0 + 1.0 / 24.0;
        lat += (g[5].toLatin1() - 'A') * 1.0 / 24.0 + 0.5 / 24.0;
    } else {
        lon += 1.0;                                // center of the 4-char square
        lat += 0.5;
    }
    return true;
}

} // namespace ttc
