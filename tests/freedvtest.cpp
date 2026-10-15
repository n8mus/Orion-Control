// SPDX-License-Identifier: GPL-3.0-or-later
// FreeDV Reporter frame handling, offline (no socket):
//   cmake --build build --target freedvtest && ./build/freedvtest
// Engine.IO open -> view-only connect; ping -> pong; the station table
// from new_connection/freq_change; spots from a live tx_report and an
// rx_report (heard call at the RECEIVER's frequency); a bulk_update's
// old tx_report does not spot; remove_connection; HF sanity clamp.
#include <QCoreApplication>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

#include "net/FreeDvReporter.h"

using namespace ttc;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

static const Spot* find(const QVector<Spot>& v, const char* call) {
    for (const Spot& s : v)
        if (s.call == QLatin1String(call)) return &s;
    return nullptr;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc > 1 && QString(argv[1]) == "--live") {
        // Watch the real reporter for N seconds (view-only, nothing sent
        // about this station): ./freedvtest --live 30
        FreeDvReporter live;
        QObject::connect(&live, &FreeDvReporter::statusChanged,
                         [](const QString& s) { std::printf("  %s\n", qPrintable(s)); });
        live.setEnabled(true);
        QTimer::singleShot((argc > 2 ? atoi(argv[2]) : 30) * 1000, &app, [&] {
            std::printf("  %d stations online, %d spots\n", live.stationCount(),
                        int(live.spots().size()));
            for (const Spot& s : live.spots())
                std::printf("    %-10s %10.1f kHz  %s\n", qPrintable(s.call), s.hz / 1000.0,
                            qPrintable(s.comment));
            app.quit();
        });
        return app.exec();
    }
    FreeDvReporter r;
    QStringList out;
    r.send = [&](const QString& t) { out << t; };
    int changed = 0;
    QObject::connect(&r, &FreeDvReporter::spotsChanged, [&] { ++changed; });

    r.handleFrame(R"(0{"sid":"x","upgrades":[],"pingInterval":25000,"pingTimeout":20000})");
    check(out.value(0) == R"(40{"role":"view","protocol_version":2})", "open -> view-only connect");
    r.handleFrame("2");
    check(out.value(1) == "3", "ping -> pong");
    r.handleFrame(R"(40{"sid":"abc"})");

    // Snapshot: two stations; W1AW's old tx_report must NOT spot.
    r.handleFrame(R"(42["bulk_update",[)"
                  R"(["new_connection",{"sid":"s1","callsign":"w1aw","grid_square":"FN31"}],)"
                  R"(["freq_change",{"sid":"s1","callsign":"W1AW","freq":14236000}],)"
                  R"(["tx_report",{"sid":"s1","callsign":"W1AW","mode":"RADEV1","transmitting":true}],)"
                  R"(["new_connection",{"sid":"s2","callsign":"VK5DGR","grid_square":"PF95"}],)"
                  R"(["freq_change",{"sid":"s2","callsign":"VK5DGR","freq":7177000}]]])");
    check(r.spots().isEmpty(), "bulk_update replay makes no spots");

    r.handleFrame(R"(42["tx_report",{"sid":"s1","callsign":"W1AW","mode":"RADEV1","transmitting":true}])");
    const Spot* w = find(r.spots(), "W1AW");
    check(w && w->hz == 14236000 && w->kind == 'V', "live tx_report spots the station at its freq");
    check(w && w->comment == "RADEV1" && w->grid == "FN31", "  with its mode and grid");

    r.handleFrame(R"(42["rx_report",{"sid":"s2","receiver_callsign":"VK5DGR","callsign":"ZL2AB","snr":7.4,"mode":"RADEV1"}])");
    const Spot* z = find(r.spots(), "ZL2AB");
    check(z && z->hz == 7177000, "rx_report spots the heard call at the receiver's freq");
    check(z && z->comment.contains("VK5DGR") && z->comment.contains("7 dB"), "  with receiver and SNR");
    r.handleFrame(R"(42["rx_report",{"sid":"s2","receiver_callsign":"VK5DGR","callsign":"","snr":3}])");
    check(r.spots().size() == 2, "rx_report without a decoded call is ignored");

    r.handleFrame(R"(42["freq_change",{"sid":"s1","freq":3700000000}])");
    r.handleFrame(R"(42["tx_report",{"sid":"s1","mode":"RADEV1","transmitting":true}])");
    check(find(r.spots(), "W1AW")->hz == 14236000, "out-of-band frequency is not spotted");

    r.handleFrame(R"(42["remove_connection",{"sid":"s2","callsign":"VK5DGR"}])");
    r.handleFrame(R"(42["rx_report",{"sid":"s2","callsign":"K1ABC","snr":5}])");
    check(!find(r.spots(), "K1ABC"), "removed station's reports have no frequency");
    check(changed >= 2, "spotsChanged fires");
    r.handleFrame("42[garbage");
    r.handleFrame("6");
    check(r.spots().size() == 2, "junk frames are ignored");

    if (fails == 0) std::printf("freedvtest: all checks passed\n");
    return fails ? 1 : 0;
}
