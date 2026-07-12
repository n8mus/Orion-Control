// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>
#include <QElapsedTimer>

class QTimer;

namespace ttc {

// Multi-style S-meter / TX meter — the meter-suite ideas from the KE9NS
// PowerSDR and Thetis lineages (GPL), all faces drawn in QPainter.
// Click cycles the STYLE:
//   Analog   — the Orion 565's own meter: amber face, black lettering,
//              dual S/watts scales, red instant + blue peak needles
//   Edge     — edgewise meter: linear ivory window, vertical needle
//   LED      — segmented LED bargraph, green/amber/red (Thetis LED item)
//   X-needle — Orion RX face; TX becomes a Daiwa-style cross-needle
//              face: FWD and REF needles crossing, SWR readout
//   Eye      — magic-eye tube (Thetis MAGIC_EYE): the green shadow wedge
//              closes as the signal rises, EM80 style
// Right-click cycles the RX READING: Signal / Average (EMA) / Peak (hold
// then decay). Needle/eye styles run ~30 fps ballistics (fast attack, slow
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
    enum Style { Analog = 0, Edge, Led, Cross, Eye, kStyleCount };
    enum RxMode { Sig = 0, Avg, Peak, kModeCount };

    static double rawToDbS9(int raw);     // piecewise-linear cal table
    double displayDb() const;             // reading after the RX-mode filter
    double scaleFrac(double db) const;    // dB -> 0..1 along the meter scale
    void   applyStyle();                  // size + tooltip for current style
    void   paintAnalog(class QPainter& p);  // RX + TX faces (Orion amber)
    void   paintEdge(class QPainter& p);    // RX + TX faces (edgewise)
    void   paintLed(class QPainter& p);     // segmented LED bargraph
    void   paintCross(class QPainter& p);   // TX cross-needle (RX = analog)
    void   paintEye(class QPainter& p);     // magic-eye tube
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
    double needleRef_ = 0.0;              // animated REF needle (cross style)
};

} // namespace ttc
