// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QHash>
#include <QString>
#include <vector>

namespace ttc {

// Callsign -> approximate location via the AD1C cty.dat country file
// (bundled as a Qt resource). Exact-call entries ("=N8EM") win, then the
// longest matching prefix. Coordinates come back east-positive (cty.dat
// stores longitude west-positive; the sign is flipped on load).
class CtyLookup {
public:
    bool load(const QString& path = ":/cty.dat");
    bool loaded() const { return !prefixes_.empty(); }
    bool lookup(const QString& call, double& lat, double& lon) const;

    // Maidenhead grid (4 or 6 chars) -> center of the square, east-positive.
    static bool gridToLatLon(const QString& grid, double& lat, double& lon);

private:
    struct Ent { QString pfx; float lat, lon; };
    std::vector<Ent> prefixes_;                    // all aliases, all countries
    QHash<QString, QPair<float, float>> exact_;    // "=CALL" overrides
};

} // namespace ttc
