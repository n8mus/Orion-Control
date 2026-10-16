// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/DisplayPanel.h"

#include <QGridLayout>
#include <QSlider>
#include <QLabel>
#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

namespace ttc {

namespace {
QLabel* makeCaption(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("color: #8fa3b8; font-size: 13px; font-weight: bold;");
    return l;
}
} // namespace

// The fleet folder: images whose FILENAME is the display name shown in the
// SHIP gallery ("Warp speed Mr Scott.jpg" -> "Warp speed Mr Scott").
QString DisplayPanel::shipDir() {
    return QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
           + "/starships";
}

// Rescan the folder into the gallery combo, keeping the current pick (from
// display/shipImagePath) selected. Called at build, when BACKGND flips to
// Ship, and after an import — images dropped into the folder by any other
// means (a file manager, the assistant fetching one) appear on the next flip.
void DisplayPanel::reloadShips() {
    const QSignalBlocker b(ship_);
    ship_->clear();
    QDir d(shipDir());
    const QStringList files = d.entryList(
        {"*.jpg", "*.jpeg", "*.png", "*.bmp", "*.webp", "*.avif"},
        QDir::Files, QDir::Name);
    for (const QString& f : files)
        ship_->addItem(QFileInfo(f).completeBaseName(), d.filePath(f));
    ship_->addItem(QStringLiteral("Add ship…"));
    const QString curName = QFileInfo(
        QSettings().value("display/shipImagePath").toString())
                                .completeBaseName();
    const int i = ship_->findText(curName);
    if (i >= 0) ship_->setCurrentIndex(i);
}

// Sized ~150% (like the AUDIO popup): these get adjusted mid-operation, so
// bigger targets beat compactness.
DisplayPanel::DisplayPanel(QWidget* parent) : QWidget(parent) {
    setStyleSheet(
        "QWidget { background: #141b24; color: #c8d4e0; font-size: 14px; }"
        "QComboBox { background: #1c2430; border: 1px solid #2a3644; border-radius: 3px;"
        " padding: 4px 8px; }"
        "QComboBox QAbstractItemView { background: #1c2430; color: #c8d4e0;"
        " selection-background-color: #2a3644; }"
        "QCheckBox { spacing: 8px; }"
        "QCheckBox::indicator { width: 17px; height: 17px; }"
        "QLineEdit { background: #1c2430; border: 1px solid #2a3644; border-radius: 3px;"
        " padding: 4px 8px; }"
        "QSlider::groove:horizontal { height: 6px; background: #2a3644; border-radius: 3px; }"
        "QSlider::handle:horizontal { width: 18px; margin: -7px 0; border-radius: 9px;"
        " background: #6aa5d8; }");

    auto* g = new QGridLayout(this);
    g->setContentsMargins(16, 14, 16, 14);
    g->setHorizontalSpacing(12);
    g->setVerticalSpacing(11);

    // Reference level: the dB value at the TOP of the spectrum scale. Lower it
    // to pull weak signals up out of the floor, raise it for strong-signal work.
    g->addWidget(makeCaption("REF LVL", this), 0, 0);
    ref_ = new QSlider(Qt::Horizontal, this);
    ref_->setRange(-100, -20);
    ref_->setFixedWidth(225);
    refVal_ = new QLabel(this);
    refVal_->setFixedWidth(70);
    g->addWidget(ref_, 0, 1);
    g->addWidget(refVal_, 0, 2);

    // Range = scale height in dB (contrast): small range = punchy colors.
    g->addWidget(makeCaption("RANGE", this), 1, 0);
    range_ = new QSlider(Qt::Horizontal, this);
    range_->setRange(30, 120);
    range_->setFixedWidth(225);
    rangeVal_ = new QLabel(this);
    rangeVal_->setFixedWidth(70);
    g->addWidget(range_, 1, 1);
    g->addWidget(rangeVal_, 1, 2);

    g->addWidget(makeCaption("AVG", this), 2, 0);
    avg_ = new QComboBox(this);
    avg_->addItem("Off", 1);
    avg_->addItem("2", 2);
    avg_->addItem("4", 4);
    avg_->addItem("8", 8);
    avg_->addItem("16", 16);
    g->addWidget(avg_, 2, 1, 1, 2);

    g->addWidget(makeCaption("WF SPEED", this), 3, 0);
    speed_ = new QComboBox(this);
    speed_->addItem("Fast", 1);
    speed_->addItem("Normal", 2);
    speed_->addItem("Slow", 4);
    speed_->addItem("Slowest", 8);
    g->addWidget(speed_, 3, 1, 1, 2);

    g->addWidget(makeCaption("PALETTE", this), 4, 0);
    pal_ = new QComboBox(this);
    pal_->addItems(PanadapterWidget::paletteNames());
    g->addWidget(pal_, 4, 1, 1, 1);

    // Spectrum trace color rides next to the palette (same visual family).
    trace_ = new QComboBox(this);
    trace_->addItems({"Soft", "White", "Green", "Yellow", "Cyan"});
    trace_->setToolTip("Spectrum trace line color");
    g->addWidget(trace_, 4, 2, 1, 1);

    g->addWidget(makeCaption("BACKGND", this), 5, 0);
    bg_ = new QComboBox(this);
    bg_->addItems(PanadapterWidget::backgroundNames());
    g->addWidget(bg_, 5, 1, 1, 2);

    // The fleet gallery (live when BACKGND = Ship): every image in
    // ~/Pictures/starships listed BY NAME — the filename is the name —
    // plus "Add ship…" to import a new one into the folder. No file
    // dialog on ordinary swaps (operator's spec).
    g->addWidget(makeCaption("SHIP", this), 6, 0);
    ship_ = new QComboBox(this);
    g->addWidget(ship_, 6, 1, 1, 2);
    reloadShips();

    // Map brightness (only live for the map backdrops): separate day-side
    // and night-side levels, so the map can pop without drowning the trace.
    g->addWidget(makeCaption("MAP DAY", this), 7, 0);
    mapDay_ = new QSlider(Qt::Horizontal, this);
    mapDay_->setRange(30, 120);
    mapDay_->setFixedWidth(225);
    mapDayVal_ = new QLabel(this);
    mapDayVal_->setFixedWidth(70);
    g->addWidget(mapDay_, 7, 1);
    g->addWidget(mapDayVal_, 7, 2);

    g->addWidget(makeCaption("MAP NIGHT", this), 8, 0);
    mapNight_ = new QSlider(Qt::Horizontal, this);
    mapNight_->setRange(0, 60);
    mapNight_->setFixedWidth(225);
    mapNightVal_ = new QLabel(this);
    mapNightVal_->setFixedWidth(70);
    g->addWidget(mapNight_, 8, 1);
    g->addWidget(mapNightVal_, 8, 2);

    g->addWidget(makeCaption("ROSE SIZE", this), 9, 0);
    roseR_ = new QSlider(Qt::Horizontal, this);
    roseR_->setRange(64, 160);
    roseR_->setFixedWidth(225);
    roseR_->setToolTip("Compass-rose disc radius. Sized up it is a full\n"
                       "azimuthal beam-heading map (it sits bottom-left of the\n"
                       "waterfall and hides when the waterfall is too short).");
    roseRVal_ = new QLabel(this);
    roseRVal_->setFixedWidth(70);
    g->addWidget(roseR_, 9, 1);
    g->addWidget(roseRVal_, 9, 2);

        // AetherSDR trace look: heat-map style + width, FFT Smooth and De-comb
    // (display only — the waterfall and decoders never see them).
    g->addWidget(makeCaption("TRACE", this), 10, 0);
    style_ = new QComboBox(this);
    style_->addItems({"Classic", "Heat map"});
    style_->setToolTip("Classic: palette fill + flat line.\n"
                       "Heat map (AetherSDR): line and fill coloured by strength,\n"
                       "blue at the noise up through green and yellow to red.");
    g->addWidget(style_, 10, 1, 1, 1);
    width_ = new QComboBox(this);
    width_->addItem("1 px", 1.0);
    width_->addItem("1.5 px", 1.5);
    width_->addItem("2 px", 2.0);
    width_->addItem("3 px", 3.0);
    width_->setToolTip("Spectrum trace line width");
    g->addWidget(width_, 10, 2, 1, 1);

    g->addWidget(makeCaption("SHAPE", this), 11, 0);
    smooth_ = new QComboBox(this);
    smooth_->addItem("Smooth off", 0);
    smooth_->addItem("Smooth 2", 2);
    smooth_->addItem("Smooth 4", 4);
    smooth_->addItem("Smooth 8", 8);
    smooth_->addItem("Smooth 16", 16);
    smooth_->setToolTip("FFT Smooth (AetherSDR): averages the trace across\n"
                        "neighbouring pixels in power, so the noise grass calms\n"
                        "down without the floor sinking.");
    g->addWidget(smooth_, 11, 1, 1, 1);
    decomb_ = new QComboBox(this);
    decomb_->addItem("De-comb off", 0);
    decomb_->addItem("De-comb 150 Hz", 150);
    decomb_->addItem("De-comb 250 Hz", 250);
    decomb_->addItem("De-comb 400 Hz", 400);
    decomb_->addItem("De-comb 600 Hz", 600);
    decomb_->setToolTip("FFT De-comb (AetherSDR): fills in the picket-fence comb\n"
                        "an SSB voice draws when zoomed in (its pitch harmonics),\n"
                        "showing the signal's outline. Leaves the noise alone;\n"
                        "works when zoomed in far enough to see the comb.");
    g->addWidget(decomb_, 11, 2, 1, 1);

fill_ = new QCheckBox("Fill spectrum", this);
    peak_ = new QCheckBox("Peak hold", this);
    grid_ = new QCheckBox("Grid lines", this);
    solar_ = new QCheckBox("Solar data panel", this);
    solar_->setToolTip("SFI / sunspots / A / K / X-ray from NOAA, refreshed "
                       "every 15 minutes; the map backdrops also get the sun "
                       "marker with the same numbers");
    tuneBox_ = new QCheckBox("Tuner / amp settings box", this);
    tuneBox_->setToolTip("Your saved 238C tuner + Titan III settings nearest\n"
                         "the TX frequency, for the antenna picked in the\n"
                         "SWR menu. Click the box to save what you set;\n"
                         "right-click for the full table.");
    voacap_ = new QCheckBox("VOACAP overlay (map backdrops)", this);
    voacap_->setToolTip(
        "Real VOACAP propagation contours on the world-map backdrops:\n"
        "coverage rings colored by predicted S-level (blue S9+, green S7,\n"
        "yellow S5, orange S3, gray S1), computed locally by voacapl from\n"
        "the live sunspot number, your grid, band and TX power.");
    rose_ = new QCheckBox("Compass rose", this);
    rose_->setToolTip("Azimuthal world disc centered on your grid square.\n"
                      "Click a spot callsign to swing the pointer to that "
                      "station;\nclick inside the rose for a manual heading, "
                      "right-click clears.");
    plan_ = new QCheckBox("Band-plan shading", this);
    plan_->setToolTip("Tint the frequency scale to the ARRL band chart:\n"
                      "red = RTTY/data, green = phone, blue = CW-only, "
                      "cyan = 60 m USB");
    priv_ = new QCheckBox("License privileges", this);
    priv_->setToolTip("Guide lines across the spectrum at US license-class "
                      "phone edges,\ntagged E / A / G / T — where each class "
                      "may transmit on this band");
    bigVfo_ = new QCheckBox("Large VFO digits", this);
    clock_ = new QCheckBox("Clock (radio panel)", this);
    cursor_ = new QCheckBox("Cursor line + zap preview", this);
    cursor_->setToolTip("Dashed line under the mouse with the exact "
                        "frequency,\nplus a green tick where a CW-zap "
                        "click would land");
    wfTime_ = new QCheckBox("Waterfall timestamps", this);
    wfTime_->setToolTip("UTC time labels down the waterfall's left edge —\n"
                        "read scrolled-back history like a log");
    zap_ = new QCheckBox("CW zap (snap to carrier)", this);
    zap_->setToolTip(
        "In CW modes a panadapter click snaps to the strongest signal\n"
        "within 150 Hz and puts the dial exactly on its carrier — the\n"
        "note lands right on your sidetone/SPOT pitch.\n"
        "Shift+click: tune exactly where clicked (no snap).\n"
        "Z key: zero-beat the strongest signal already in the passband.\n"
        "X key: CW⇄CWR flip — if the note's pitch doesn't change,\n"
        "you are perfectly zero-beat (by ear, no spectrum needed).");
    call_ = new QCheckBox("Callsign watermark", this);
    g->addWidget(fill_, 12, 0, 1, 3);
    g->addWidget(peak_, 13, 0, 1, 3);
    g->addWidget(grid_, 14, 0, 1, 3);
    g->addWidget(solar_, 15, 0, 1, 3);
    g->addWidget(tuneBox_, 16, 0, 1, 3);
    g->addWidget(rose_, 17, 0, 1, 3);
    g->addWidget(plan_, 18, 0, 1, 3);
    g->addWidget(priv_, 19, 0, 1, 3);
    g->addWidget(bigVfo_, 20, 0, 1, 3);
    g->addWidget(clock_, 21, 0, 1, 3);
    g->addWidget(wfTime_, 22, 0, 1, 3);
    g->addWidget(cursor_, 23, 0, 1, 3);
    g->addWidget(zap_, 24, 0, 1, 3);
    g->addWidget(voacap_, 25, 0, 1, 3);
    g->addWidget(call_, 26, 0, 1, 3);

    g->addWidget(makeCaption("CALL", this), 27, 0);
    callEdit_ = new QLineEdit(this);
    callEdit_->setMaxLength(12);
    g->addWidget(callEdit_, 27, 1, 1, 2);

    // Station grid square: centers the compass rose (and any future
    // bearing/distance math). 4 or 6 characters.
    g->addWidget(makeCaption("GRID", this), 28, 0);
    gridEdit_ = new QLineEdit(this);
    gridEdit_->setMaxLength(6);
    gridEdit_->setToolTip("Your Maidenhead grid square (4 or 6 chars), e.g. EN82fq");
    g->addWidget(gridEdit_, 28, 1, 1, 2);

    auto updateLabels = [this] {
        refVal_->setText(QString("%1 dB").arg(ref_->value()));
        rangeVal_->setText(QString("%1 dB").arg(range_->value()));
        mapDayVal_->setText(QString("%1 %").arg(mapDay_->value()));
        mapNightVal_->setText(QString("%1 %").arg(mapNight_->value()));
        roseRVal_->setText(QString("%1 px").arg(roseR_->value()));
        const int bg = bg_->currentIndex();
        const bool isMap = bg >= 2 && bg <= 5;
        const bool isGlobe = bg == 7;              // Globe: QTH
        mapDay_->setEnabled(isMap || isGlobe || bg == 6);  // ship: brightness
        mapNight_->setEnabled(isMap || isGlobe);   // globe: city-light level
        ship_->setEnabled(bg == 6);
    };
    connect(ref_,   &QSlider::valueChanged, this, [this, updateLabels] {
        updateLabels();
        emitChanged();
    });
    connect(range_, &QSlider::valueChanged, this, [this, updateLabels] {
        updateLabels();
        emitChanged();
    });
    connect(mapDay_, &QSlider::valueChanged, this, [this, updateLabels] {
        updateLabels();
        emitChanged();
    });
    connect(mapNight_, &QSlider::valueChanged, this, [this, updateLabels] {
        updateLabels();
        emitChanged();
    });
    connect(roseR_, &QSlider::valueChanged, this, [this, updateLabels] {
        updateLabels();
        emitChanged();
    });
    connect(avg_,   &QComboBox::currentIndexChanged, this, &DisplayPanel::emitChanged);
    connect(speed_, &QComboBox::currentIndexChanged, this, &DisplayPanel::emitChanged);
    connect(pal_,   &QComboBox::currentIndexChanged, this, &DisplayPanel::emitChanged);
    for (QComboBox* c : {style_, width_, smooth_, decomb_})
        connect(c, &QComboBox::currentIndexChanged, this, &DisplayPanel::emitChanged);
    connect(bg_, &QComboBox::currentIndexChanged, this, [this, updateLabels](int idx) {
        // Index 5 = "Map: Custom…" (a FIXED index): ask for the image right
        // here. Cancel with no previous pick bounces back to the prior
        // backdrop. Index 6 = "Ship" — no dialog; the SHIP gallery row
        // below picks the artwork.
        if (idx == 5) {
            QSettings s;
            const QString cur = s.value("display/mapCustomPath").toString();
            const QString f = QFileDialog::getOpenFileName(
                this, "Choose a world map image",
                cur.isEmpty() ? QDir::homePath() : cur,
                "Images (*.jpg *.jpeg *.png *.bmp *.webp *.avif)");
            if (!f.isEmpty()) {
                s.setValue("display/mapCustomPath", f);
            } else if (cur.isEmpty()) {
                const QSignalBlocker b(bg_);
                bg_->setCurrentIndex(prevBg_);
                updateLabels();
                return;
            }
        }
        if (idx == 6) reloadShips();               // folder may have grown
        prevBg_ = idx;
        updateLabels();
        emitChanged();
    });
    connect(ship_, &QComboBox::activated, this, [this](int idx) {
        // Last entry = "Add ship…": import an image into the fleet folder
        // (copied in, so the gallery survives the source file moving).
        if (idx == ship_->count() - 1) {
            const QString f = QFileDialog::getOpenFileName(
                this, "Add a ship to the fleet folder", QDir::homePath(),
                "Images (*.jpg *.jpeg *.png *.bmp *.webp *.avif)");
            QString sel;
            if (!f.isEmpty()) {
                QDir().mkpath(shipDir());
                const QString dst = shipDir() + "/" + QFileInfo(f).fileName();
                QFile::copy(f, dst);
                QSettings().setValue("display/shipImagePath", dst);
                sel = QFileInfo(f).completeBaseName();
            }
            reloadShips();                         // re-sync + restore choice
            if (!sel.isEmpty()) {
                const int i = ship_->findText(sel);
                if (i >= 0) { const QSignalBlocker b(ship_); ship_->setCurrentIndex(i); }
            }
        } else {
            QSettings().setValue("display/shipImagePath",
                                 ship_->itemData(idx).toString());
        }
        emitChanged();
    });
    connect(fill_,  &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(peak_,  &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(grid_,  &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(solar_, &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(tuneBox_, &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(rose_,  &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(voacap_, &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(plan_,  &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(priv_,  &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(bigVfo_, &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(clock_, &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(wfTime_, &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(cursor_, &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(zap_,   &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(trace_, &QComboBox::currentIndexChanged, this,
            &DisplayPanel::emitChanged);
    connect(call_,  &QCheckBox::toggled, this, &DisplayPanel::emitChanged);
    connect(callEdit_, &QLineEdit::textEdited, this, [this](const QString& t) {
        emit callsignChanged(t.trimmed().toUpper());
    });
    connect(gridEdit_, &QLineEdit::textEdited, this, [this](const QString& t) {
        emit gridChanged(t.trimmed().toUpper());
    });

    setSettings(DisplaySettings{});                // defaults until owner restores
}

DisplaySettings DisplayPanel::settings() const {
    DisplaySettings s;
    s.refDb     = static_cast<float>(ref_->value());
    s.rangeDb   = static_cast<float>(range_->value());
    s.avgFrames = avg_->currentData().toInt();
    s.wfSpeed   = speed_->currentData().toInt();
    s.palette   = pal_->currentIndex();
    s.fillTrace  = fill_->isChecked();
    s.peakHold   = peak_->isChecked();
    s.background = bg_->currentIndex();
    s.mapDay     = mapDay_->value();
    s.mapNight   = mapNight_->value();
    s.showGrid   = grid_->isChecked();
    s.showCall   = call_->isChecked();
    s.showSolar  = solar_->isChecked();
    s.showTuneBox = tuneBox_->isChecked();
    s.showRose   = rose_->isChecked();
    s.roseR      = roseR_->value();
    s.showVoacap = voacap_->isChecked();
    s.showBandPlan = plan_->isChecked();
    s.showPrivileges = priv_->isChecked();
    s.traceColor = trace_->currentIndex();
    s.traceStyle = style_->currentIndex();
    s.lineWidth  = float(width_->currentData().toDouble());
    s.smoothPx   = smooth_->currentData().toInt();
    s.decombHz   = decomb_->currentData().toInt();
    s.bigVfo     = bigVfo_->isChecked();
    s.showClock  = clock_->isChecked();
    s.showWfTime = wfTime_->isChecked();
    s.showCursor = cursor_->isChecked();
    s.cwZap      = zap_->isChecked();
    s.split      = split_;
    s.wfRefDb    = wfRef_;
    return s;
}

void DisplayPanel::setSettings(const DisplaySettings& s) {
    split_ = s.split;
    wfRef_ = s.wfRefDb;
    const QSignalBlocker b1(ref_), b2(range_), b3(avg_), b4(speed_), b5(pal_),
        b6(fill_), b7(peak_), b8(bg_), b9(grid_), b10(call_),
        b11(mapDay_), b12(mapNight_), b13(solar_), b14(rose_),
        b15(plan_), b16(bigVfo_), b17(trace_), b18(clock_), b19(zap_),
        b20(wfTime_), b21(cursor_), b22(priv_), b23(roseR_), b24(voacap_),
        b25(tuneBox_), b26(style_), b27(width_), b28(smooth_), b29(decomb_);
    ref_->setValue(static_cast<int>(s.refDb));
    range_->setValue(static_cast<int>(s.rangeDb));
    const int ai = avg_->findData(s.avgFrames);
    avg_->setCurrentIndex(ai >= 0 ? ai : 2);
    const int si = speed_->findData(s.wfSpeed);
    speed_->setCurrentIndex(si >= 0 ? si : 1);
    pal_->setCurrentIndex(std::clamp(s.palette, 0, pal_->count() - 1));
    bg_->setCurrentIndex(std::clamp(s.background, 0, bg_->count() - 1));
    prevBg_ = bg_->currentIndex();
    mapDay_->setValue(std::clamp(s.mapDay, mapDay_->minimum(), mapDay_->maximum()));
    mapNight_->setValue(std::clamp(s.mapNight, mapNight_->minimum(),
                                   mapNight_->maximum()));
    fill_->setChecked(s.fillTrace);
    peak_->setChecked(s.peakHold);
    grid_->setChecked(s.showGrid);
    call_->setChecked(s.showCall);
    solar_->setChecked(s.showSolar);
    tuneBox_->setChecked(s.showTuneBox);
    rose_->setChecked(s.showRose);
    roseR_->setValue(std::clamp(s.roseR, roseR_->minimum(), roseR_->maximum()));
    voacap_->setChecked(s.showVoacap);
    plan_->setChecked(s.showBandPlan);
    priv_->setChecked(s.showPrivileges);
    bigVfo_->setChecked(s.bigVfo);
    clock_->setChecked(s.showClock);
    wfTime_->setChecked(s.showWfTime);
    cursor_->setChecked(s.showCursor);
    zap_->setChecked(s.cwZap);
    trace_->setCurrentIndex(std::clamp(s.traceColor, 0, trace_->count() - 1));
    style_->setCurrentIndex(std::clamp(s.traceStyle, 0, style_->count() - 1));
    auto pick = [](QComboBox* c, const QVariant& v) {   // nearest stored value
        int best = 0;
        for (int i = 0; i < c->count(); ++i)
            if (std::abs(c->itemData(i).toDouble() - v.toDouble())
                < std::abs(c->itemData(best).toDouble() - v.toDouble())) best = i;
        c->setCurrentIndex(best);
    };
    pick(width_, double(s.lineWidth));
    pick(smooth_, s.smoothPx);
    pick(decomb_, s.decombHz);
    refVal_->setText(QString("%1 dB").arg(ref_->value()));
    rangeVal_->setText(QString("%1 dB").arg(range_->value()));
    mapDayVal_->setText(QString("%1 %").arg(mapDay_->value()));
    mapNightVal_->setText(QString("%1 %").arg(mapNight_->value()));
    roseRVal_->setText(QString("%1 px").arg(roseR_->value()));
    const int bg = bg_->currentIndex();
    const bool isMap = bg >= 2 && bg <= 5;
    mapDay_->setEnabled(isMap || bg == 6);         // ship art: Day = brightness
    mapNight_->setEnabled(isMap);
    reloadShips();                                 // folder may have grown
    ship_->setEnabled(bg == 6);
}

void DisplayPanel::setCallsign(const QString& call) {
    const QSignalBlocker b(callEdit_);
    callEdit_->setText(call);
}

void DisplayPanel::setGrid(const QString& grid) {
    const QSignalBlocker b(gridEdit_);
    gridEdit_->setText(grid);
}

void DisplayPanel::emitChanged() {
    emit settingsChanged(settings());
}

} // namespace ttc
