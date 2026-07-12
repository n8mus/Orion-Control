// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>
#include <QElapsedTimer>

class QTimer;

namespace ttc {

// Multi-style S-meter / TX meter (KE9NS meter-suite idea, faces drawn
// entirely in QPainter — no bitmap skins). Click cycles the STYLE:
//   Bar     — the original compact SmartSDR-style bar
//   Analog  — ivory needle meter, S1..S9 black arc, over-S9 red arc
//   Edge    — edgewise meter: linear ivory window, vertical needle
//   Digital — Thetis-style triple readout: dBm / S-units / microvolts
// Right-click cycles the RX READING: Signal / Average (EMA) / Peak (hold
// then decay). Needle styles run ~30 fps ballistics (fast attack, slow
// decay) so the 2 Hz poll data moves like a real movement. TX flips every
// style to a power face (watts + SWR). Fed raw Orion ?S readings; converts
// to dB-relative-to-S9 via the hamlib TT565 V2 calibration table.
class SMeterWidget : public QWidget {
    Q_OBJECT
public:
    explicit SMeterWidget(QWidget* parent = nullptr);

public slots:
    void setRawLevel(int raw);            // raw @SRM units from the radio (RX)
    void setTxLevel(double fwdWatts, double refWatts, double swr);  // @STF (TX)

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(class QMouseEvent* e) override;

private:
    enum Style { Bar = 0, Analog, Edge, Digital, kStyleCount };
    enum RxMode { Sig = 0, Avg, Peak, kModeCount };

    static double rawToDbS9(int raw);     // piecewise-linear cal table
    double displayDb() const;             // reading after the RX-mode filter
    double scaleFrac(double db) const;    // dB -> 0..1 along the meter scale
    void   applyStyle();                  // size + tooltip for current style
    void   paintBarRx(class QPainter& p);
    void   paintBarTx(class QPainter& p);
    void   paintAnalog(class QPainter& p);  // RX + TX faces (needle arc)
    void   paintEdge(class QPainter& p);    // RX + TX faces (edgewise)
    void   paintDigital(class QPainter& p);
    QString sUnitsText(double db) const;  // "S7" / "S9+23"

    int  style_ = Analog;
    int  mode_  = Sig;
    QTimer* anim_ = nullptr;              // needle ballistics tick
    double needleDb_ = -120.0;            // animated needle position (RX)
    double needleW_  = 0.0;               // animated needle position (TX)

    bool tx_ = false;                     // which face the meter shows
    double dbS9_    = -120.0;             // current level, dB relative to S9
    double emaDb_   = -120.0;             // Average mode reading
    bool   emaInit_ = false;
    double peakDb_  = -120.0;             // Peak mode / peak-hold marker
    QElapsedTimer sincePeak_;
    bool haveReading_ = false;
    double fwdW_ = 0.0, refW_ = 0.0, swr_ = 1.0;
    double peakW_ = 0.0;                  // TX peak hold (watts)
    QElapsedTimer sinceTxPeak_;
};

} // namespace ttc
