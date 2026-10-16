// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/PanGpuView.h"

#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <rhi/qrhi.h>

#include "ui/PanadapterWidget.h"

namespace ttc {

namespace {
QShader loadShader(const char* name) {
    QFile f(QString(":/shaders/%1.qsb").arg(name));
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
}
constexpr int kQuadUbo = 64 + 3 * 16;              // mat4 + 3 vec4 (std140)
constexpr int kLineUbo = 64 + 2 * 16;              // mat4 + 2 vec4
constexpr int kLabelStripPx = 160;                 // waterfall label strip width
constexpr int k3dCols = 768;                       // 3D mesh columns (pan_3d.vert: 767 steps)
// TTC_PANPROF=1: GPU frame cost on the CPU side (layer rebuilds + uploads
// + command recording), same cadence as the CPU path's [panprof].
struct GpuProf {
    bool on = qEnvironmentVariableIsSet("TTC_PANPROF");
    QElapsedTimer window;
    double renderMs = 0, layerMs = 0;
    int frames = 0, rebuilds = 0;
    QElapsedTimer gap;                             // render-to-render: GUI stalls
    double maxGapMs = 0;
    int stalls = 0;
    void add(double r, double l, bool rebuilt) {
        if (!on) return;
        if (!window.isValid()) window.start();
        if (gap.isValid()) {
            const double g = gap.nsecsElapsed() / 1e6;
            maxGapMs = std::max(maxGapMs, g);
            stalls += g > 50.0;
        }
        gap.restart();
        renderMs += r; layerMs += l; ++frames; rebuilds += rebuilt;
        if (window.elapsed() < 5000) return;
        const double secs = window.elapsed() / 1000.0;
        std::fprintf(stderr, "[panprof-gpu] %.1f fps | %.2f ms/frame on the CPU (layers %.2f, "
                             "%d rebuilds) | %.0f%% of a core | FFT %.1f/s, dropped %.1f/s, "
                             "shown %.1f/s | worst gap %.0f ms, %d gaps > 50 ms | max queue %d, max IQ backlog %d ms\n",
                     frames / secs, renderMs / frames, layerMs / frames, rebuilds,
                     100.0 * renderMs / (secs * 1000.0),
                     PanadapterWidget::profEmitted.exchange(0) / secs,
                     PanadapterWidget::profDropped.exchange(0) / secs,
                     PanadapterWidget::profTaken.exchange(0) / secs, maxGapMs, stalls,
                     PanadapterWidget::profMaxQueue.exchange(0), PanadapterWidget::profMaxIqMs.exchange(0));
        maxGapMs = 0;
        stalls = 0;
        renderMs = layerMs = 0; frames = rebuilds = 0;
        window.restart();
    }
};
GpuProf gGpuProf;
}  // namespace

PanGpuView::PanGpuView(PanadapterWidget* pan) : QRhiWidget(pan), pan_(pan) {
    setApi(QRhiWidget::Api::OpenGL);
    setAttribute(Qt::WA_TransparentForMouseEvents);   // the panadapter keeps its events
    setFocusPolicy(Qt::NoFocus);
}

PanGpuView::~PanGpuView() { freeAll(); }

void PanGpuView::releaseResources() { freeAll(); }

void PanGpuView::freeAll() {
    auto del = [](auto*& p) { delete p; p = nullptr; };
    for (Layer* l : {&under_, &mid_, &labels_, &top_, &cursor_}) {
        del(l->tex);
        del(l->srb);
        l->img = QImage();
        l->dirty = true;
    }
    del(overlayPipe_); del(wfPipe_); del(fillPipe_); del(linePipe_); del(widePipe_); del(pipe3d_);
    del(wfSrb_); del(fillSrb_); del(peakSrb_); del(traceSrb_); del(srb3d_);
    del(quadVbo_); del(lineVbo_); del(wideVbo_); del(mesh3d_);
    for (auto*& u : ubo_) del(u);
    del(linear_); del(nearest_);
    del(hist_); del(palette_); del(cols_); del(hist3d_);
    histBins_ = colsW_ = lineVboCap_ = wideVboCap_ = hist3dBins_ = mesh3dVerts_ = 0;
    hist3dReset_ = ~0ull;
    histReset_ = ~0ull;
    paletteId_ = -1;
    pipesBuilt_ = false;
    rhi_ = nullptr;
}

void PanGpuView::initialize(QRhiCommandBuffer*) {
    if (rhi_ != rhi()) { freeAll(); rhi_ = rhi(); }
    if (pipesBuilt_) return;
    QRhi* r = rhi_;
    // Quad: triangle strip, (ndc x, y, u, v); uv (0,0) = top-left.
    static const float quad[] = {-1.f, -1.f, 0.f, 1.f,  1.f, -1.f, 1.f, 1.f,
                                 -1.f,  1.f, 0.f, 0.f,  1.f,  1.f, 1.f, 0.f};
    quadVbo_ = r->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof quad);
    quadVbo_->create();
    for (int i = 0; i < uCount; ++i) {
        const int sz = (i == uPeak || i == uTrace) ? kLineUbo : kQuadUbo;
        ubo_[i] = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sz);
        ubo_[i]->create();
    }
    linear_ = r->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                            QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    linear_->create();
    nearest_ = r->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                             QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    nearest_->create();
    palette_ = r->newTexture(QRhiTexture::RGBA8, QSize(256, 1));
    palette_->create();
    quadUploaded_ = false;                         // render() uploads it
    // Placeholders so every binding is valid before the first data arrives.
    ensureHist(2);
    cols_ = r->newTexture(QRhiTexture::R32F, QSize(2, 1));
    cols_->create();
    colsW_ = 2;
    for (Layer* l : {&under_, &mid_, &labels_, &top_, &cursor_}) ensureLayer(*l, QSize(2, 2));
    pipesBuilt_ = true;
}

void PanGpuView::ensureLayer(Layer& l, const QSize& sz) {
    if (l.tex && l.tex->pixelSize() == sz) return;
    delete l.tex;
    delete l.srb;
    if (&l == &under_) { delete overlayPipe_; overlayPipe_ = nullptr; }   // made with this SRB
    l.tex = rhi_->newTexture(QRhiTexture::RGBA8, sz);
    l.tex->create();
    l.img = QImage(sz, QImage::Format_RGBA8888_Premultiplied);
    l.img.fill(Qt::transparent);
    l.dirty = true;
    const int ui = &l == &under_ ? uUnder : &l == &mid_ ? uMid : &l == &labels_ ? uLabels
                 : &l == &top_ ? uTop : uCursor;
    l.srb = rhi_->newShaderResourceBindings();
    l.srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage
                                                        | QRhiShaderResourceBinding::FragmentStage,
                                                 ubo_[ui]),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                  l.tex, linear_)});
    l.srb->create();
}

void PanGpuView::ensureHist(int bins) {
    if (hist_ && histBins_ == bins) return;
    delete hist_;
    hist_ = rhi_->newTexture(QRhiTexture::R32F, QSize(bins, PanadapterWidget::kWfHistRows));
    hist_->create();
    histBins_ = bins;
    histReset_ = ~0ull;                            // full upload next frame
    delete wfSrb_;
    wfSrb_ = nullptr;
    delete wfPipe_;
    wfPipe_ = nullptr;
}

void PanGpuView::render(QRhiCommandBuffer* cb) {
    QElapsedTimer t;
    t.start();
    QRhi* r = rhi_;
    PanadapterWidget* p = pan_;
    const QSize px = renderTarget()->pixelSize();
    const qreal dpr = devicePixelRatioF();
    const int W = p->width(), H = p->height();
    const int hSpec = p->spectrumHeight();
    const int wfTop = hSpec + PanadapterWidget::kScaleBandH;
    const PanadapterWidget::PanView v = p->panView();
    QRhiResourceUpdateBatch* u = r->nextResourceUpdateBatch();
    if (!quadUploaded_) {
        static const float quad[] = {-1.f, -1.f, 0.f, 1.f,  1.f, -1.f, 1.f, 1.f,
                                     -1.f,  1.f, 0.f, 0.f,  1.f,  1.f, 1.f, 0.f};
        u->uploadStaticBuffer(quadVbo_, quad);
        quadUploaded_ = true;
    }

    // ---- layers: the CPU paint's own QPainter code, only when changed ----
    QElapsedTimer lt;
    lt.start();
    bool rebuilt = false;
    // The waterfall timestamps + "WF n dB" label live in the left edge only:
    // their layer is a strip, so its per-row redraw uploads ~1/12 of a
    // screen, not all of it.
    const int stripW = std::min(kLabelStripPx, W);
    for (Layer* l : {&under_, &mid_, &top_, &cursor_}) ensureLayer(*l, px);
    ensureLayer(labels_, QSize(int(stripW * dpr), px.height()));
    if (p->gpuDirtyUnder_) under_.dirty = true;
    if (p->gpuDirtyMid_) mid_.dirty = true;
    if (p->gpuDirtyTop_) top_.dirty = true;
    if (p->gpuDirtyLabels_) labels_.dirty = true;
    const bool cursorDirty = p->gpuDirtyCursor_ || cursor_.dirty;
    p->gpuDirtyUnder_ = p->gpuDirtyMid_ = p->gpuDirtyTop_ = p->gpuDirtyLabels_ = false;
    p->gpuDirtyCursor_ = false;
    auto paint = [&](Layer& l, auto&& fn) {
        if (!l.dirty) return;
        l.img.setDevicePixelRatio(dpr);
        l.img.fill(Qt::transparent);
        {
            QPainter qp(&l.img);
            qp.setFont(p->font());
            fn(qp);
        }
        u->uploadTexture(l.tex, l.img);
        l.dirty = false;
        rebuilt = true;
    };
    paint(under_, [&](QPainter& qp) { p->paintUnder(qp, hSpec); });
    paint(mid_, [&](QPainter& qp) { p->paintMid(qp, hSpec); });
    paint(labels_, [&](QPainter& qp) { p->paintWfLabels(qp, hSpec, v); });
    paint(top_, [&](QPainter& qp) { p->paintTop(qp, hSpec, v.n, false); });
    // The hover cursor: its own layer, cleared and re-uploaded only where it
    // was and is (a strip around the line + its readout), not the screen.
    if (cursorDirty) {
        cursor_.img.setDevicePixelRatio(dpr);
        const QRect old = cursorDrawn_;
        {
            QPainter qp(&cursor_.img);
            qp.setFont(p->font());
            if (cursor_.dirty) {                   // new image: clear it all
                qp.setCompositionMode(QPainter::CompositionMode_Source);
                qp.fillRect(QRect(0, 0, W, H), Qt::transparent);
            } else if (!old.isEmpty()) {
                qp.setCompositionMode(QPainter::CompositionMode_Source);
                qp.fillRect(old, Qt::transparent);
            }
            qp.setCompositionMode(QPainter::CompositionMode_SourceOver);
            p->drawCursor(qp, hSpec);
        }
        cursorDrawn_ = p->cursorRect_;
        QRect up = (cursor_.dirty ? QRect(0, 0, W, H) : old.united(cursorDrawn_))
                       .intersected(QRect(0, 0, W, H));
        if (!up.isEmpty()) {
            const QRect pxr(int(up.x() * dpr), int(up.y() * dpr), int(std::ceil(up.width() * dpr)),
                            int(std::ceil(up.height() * dpr)));
            const QRect clip = pxr.intersected(cursor_.img.rect());
            QRhiTextureSubresourceUploadDescription d(cursor_.img);
            d.setSourceTopLeft(clip.topLeft());
            d.setSourceSize(clip.size());
            d.setDestinationTopLeft(clip.topLeft());
            u->uploadTexture(cursor_.tex, QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, d)));
        }
        cursor_.dirty = false;
        rebuilt = true;
    }
    const double layerMs = lt.nsecsElapsed() / 1e6;

    // ---- palette (the CPU path's own colour map) ----
    if (paletteId_ != p->ds_.palette) {
        QImage pal(256, 1, QImage::Format_RGBA8888);
        for (int i = 0; i < 256; ++i) pal.setPixel(i, 0, p->mapColor(i / 255.0f) | 0xff000000u);
        u->uploadTexture(palette_, pal);
        paletteId_ = p->ds_.palette;
    }

    // ---- waterfall history: only the rows that arrived ----
    const bool haveWf = p->wfBins_ >= 2 && v.n >= 2 && H > wfTop && p->wfCount_ > 0;
    if (p->wfBins_ >= 2) {
        ensureHist(p->wfBins_);
        const int rows = PanadapterWidget::kWfHistRows;
        const int bins = p->wfBins_;
        uint64_t from = histSerial_;
        if (histReset_ != p->wfReset_ || p->wfSerial_ - histSerial_ > uint64_t(rows)
            || p->wfSerial_ < histSerial_) {
            from = p->wfSerial_ > uint64_t(rows) ? p->wfSerial_ - rows : 0;
            histReset_ = p->wfReset_;
        }
        QVarLengthArray<QRhiTextureUploadEntry, 16> entries;
        for (uint64_t s = from; s < p->wfSerial_; ++s) {
            const int idx = int(s % rows);
            QRhiTextureSubresourceUploadDescription d(&p->wfHist_[size_t(idx) * bins],
                                                      quint32(bins * sizeof(float)));
            d.setSourceSize(QSize(bins, 1));
            d.setDestinationTopLeft(QPoint(0, idx));
            entries.append(QRhiTextureUploadEntry(0, 0, d));
        }
        if (qEnvironmentVariableIsSet("TTC_PANDBG"))
            std::fprintf(stderr, "[pandbg] serial %llu count %d bins %d: uploading %d rows (from %llu)\n",
                         (unsigned long long)p->wfSerial_, p->wfCount_, bins, int(entries.size()),
                         (unsigned long long)from);
        if (!entries.isEmpty()) {
            QRhiTextureUploadDescription desc;
            desc.setEntries(entries.cbegin(), entries.cend());
            u->uploadTexture(hist_, desc);
        }
        histSerial_ = p->wfSerial_;
    }

    // ---- per-column levels (fill + trace) ----
    const std::vector<float> colT = p->columnLevels(v);
    const int cw = std::max(2, int(colT.size()));
    if (!cols_ || colsW_ != cw) {
        delete cols_;
        cols_ = r->newTexture(QRhiTexture::R32F, QSize(cw, 1));
        cols_->create();
        colsW_ = cw;
        delete fillSrb_;
        fillSrb_ = nullptr;
        delete fillPipe_;
        fillPipe_ = nullptr;
    }
    if (!colT.empty()) {
        QRhiTextureSubresourceUploadDescription d(colT.data(), quint32(colT.size() * sizeof(float)));
        d.setSourceSize(QSize(int(colT.size()), 1));
        u->uploadTexture(cols_, QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, d)));
    }

    // ---- 3D stacked traces: history rows + the mesh (built once) ----
    const bool want3d = p->view3d_ && p->bins3d_ >= 2 && v.n >= 2 && hSpec > 40;
    if (want3d) {
        const int bins = p->bins3d_, rows = PanadapterWidget::k3dRows;
        if (!hist3d_ || hist3dBins_ != bins) {
            delete hist3d_;
            hist3d_ = r->newTexture(QRhiTexture::R32F, QSize(bins, rows));
            hist3d_->create();
            hist3dBins_ = bins;
            hist3dReset_ = ~0ull;
            delete srb3d_;
            srb3d_ = nullptr;
            delete pipe3d_;
            pipe3d_ = nullptr;
        }
        uint64_t from = hist3dSerial_;
        if (hist3dReset_ != p->reset3d_ || p->serial3d_ - hist3dSerial_ > uint64_t(rows)
            || p->serial3d_ < hist3dSerial_) {
            from = p->serial3d_ > uint64_t(rows) ? p->serial3d_ - rows : 0;
            hist3dReset_ = p->reset3d_;
        }
        QVarLengthArray<QRhiTextureUploadEntry, 8> entries;
        for (uint64_t s = from; s < p->serial3d_; ++s) {
            const int idx = int(s % rows);
            QRhiTextureSubresourceUploadDescription d(&p->hist3d_[size_t(idx) * bins],
                                                      quint32(bins * sizeof(float)));
            d.setSourceSize(QSize(bins, 1));
            d.setDestinationTopLeft(QPoint(0, idx));
            entries.append(QRhiTextureUploadEntry(0, 0, d));
        }
        if (!entries.isEmpty()) {
            QRhiTextureUploadDescription desc;
            desc.setEntries(entries.cbegin(), entries.cend());
            u->uploadTexture(hist3d_, desc);
        }
        hist3dSerial_ = p->serial3d_;
        if (!mesh3d_) {
            // Back row first: each row is a curtain (foot..top) then its
            // ridge line, two triangles each per column step.
            std::vector<float> m;
            m.reserve(size_t(rows) * (k3dCols - 1) * 36);
            auto vtx = [&](float uu, int rr, float tag) { m.push_back(uu); m.push_back(float(rr)); m.push_back(tag); };
            for (int rr = rows - 1; rr >= 0; --rr)
                for (int c = 0; c + 1 < k3dCols; ++c) {
                    const float u0 = float(c) / (k3dCols - 1), u1 = float(c + 1) / (k3dCols - 1);
                    for (float top : {1.f, 2.f}) {
                        const float bot = top == 1.f ? 0.f : 3.f;
                        vtx(u0, rr, top); vtx(u0, rr, bot); vtx(u1, rr, top);
                        vtx(u1, rr, top); vtx(u0, rr, bot); vtx(u1, rr, bot);
                    }
                }
            mesh3dVerts_ = int(m.size() / 3);
            mesh3d_ = r->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                                   quint32(m.size() * sizeof(float)));
            mesh3d_->create();
            u->uploadStaticBuffer(mesh3d_, m.data());
        }
    }

    // ---- trace + peak-hold vertices (widget pixels) ----
    std::vector<float> verts;
    int traceN = 0, peakN = 0;
    if (v.n >= 2) {
        if (p->ds_.peakHold && p->peaks_.size() == p->spectrum_.size()) {
            const int nv = v.binHi - v.binLo;
            const float range = p->dbMax_ - p->dbMin_;
            for (int i = 0; i < nv; ++i) {
                const float x = float(i) / float(nv - 1) * float(W);
                const float tt = (p->peaks_[size_t(v.binLo + i)] - p->dbMin_) / range;
                verts.push_back(x);
                verts.push_back(float(hSpec) * (1.f - std::clamp(tt, 0.f, 1.f)));
            }
            peakN = nv;
        }
        for (int x = 0; x < int(colT.size()); ++x) {
            verts.push_back(float(x) + 0.5f);
            verts.push_back(float(hSpec) * (1.f - colT[size_t(x)]));
        }
        traceN = int(colT.size());
    }
    // Wide or heat trace (AetherSDR look): one anti-aliased quad per segment,
    // extended half a width at each end so the joins close. The flat 1 px
    // classic line stays a plain line strip (the original look).
    const bool wide = p->ds_.traceStyle == 1 || p->ds_.lineWidth > 1.0f;
    std::vector<float> quads;
    if (wide && traceN > 1) {
        const float h = p->ds_.lineWidth * 0.5f + 0.5f;
        quads.reserve(size_t(traceN - 1) * 18);
        for (int i = 0; i + 1 < traceN; ++i) {
            const float ax = verts[size_t(2 * (peakN + i))], ay = verts[size_t(2 * (peakN + i) + 1)];
            const float bx = verts[size_t(2 * (peakN + i + 1))], by = verts[size_t(2 * (peakN + i + 1) + 1)];
            float dx = bx - ax, dy = by - ay;
            const float len = std::max(1e-4f, std::sqrt(dx * dx + dy * dy));
            dx /= len; dy /= len;
            const float nx = -dy * h, ny = dx * h;
            const float a0x = ax - dx * h, a0y = ay - dy * h, b0x = bx + dx * h, b0y = by + dy * h;
            const float q[18] = {a0x + nx, a0y + ny, 1.f,  a0x - nx, a0y - ny, -1.f,  b0x + nx, b0y + ny, 1.f,
                                 b0x + nx, b0y + ny, 1.f,  a0x - nx, a0y - ny, -1.f,  b0x - nx, b0y - ny, -1.f};
            quads.insert(quads.end(), q, q + 18);
        }
        const int need = int(quads.size() * sizeof(float));
        if (!wideVbo_ || wideVboCap_ < need) {
            delete wideVbo_;
            wideVboCap_ = need * 2;
            wideVbo_ = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, wideVboCap_);
            wideVbo_->create();
        }
        u->updateDynamicBuffer(wideVbo_, 0, need, quads.data());
    }
    const int needBytes = std::max(16, int(verts.size() * sizeof(float)));
    if (!lineVbo_ || lineVboCap_ < needBytes) {
        delete lineVbo_;
        lineVboCap_ = needBytes * 2;
        lineVbo_ = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, lineVboCap_);
        lineVbo_->create();
    }
    if (!verts.empty()) u->updateDynamicBuffer(lineVbo_, 0, int(verts.size() * sizeof(float)), verts.data());

    // ---- uniforms ----
    const QMatrix4x4 mvp = r->clipSpaceCorrMatrix();
    auto quadUbo = [&](int i, const float* p0, const float* p1, const float* p2) {
        float b[28] = {};
        std::memcpy(b, mvp.constData(), 64);
        if (p0) std::memcpy(b + 16, p0, 16);
        if (p1) std::memcpy(b + 20, p1, 16);
        if (p2) std::memcpy(b + 24, p2, 16);
        u->updateDynamicBuffer(ubo_[i], 0, kQuadUbo, b);
    };
    for (int i : {uUnder, uMid, uLabels, uTop, uCursor}) quadUbo(i, nullptr, nullptr, nullptr);
    const float wf0[4] = {float(v.binLo), float(v.binHi), float(px.width()),
                          float(std::max(1.0, (H - wfTop) * dpr))};
    const float wf1[4] = {float(p->wfSerial_ % PanadapterWidget::kWfHistRows),
                          float(p->wfCount_), float(PanadapterWidget::kWfHistRows), 0.f};
    const float wf2[4] = {p->wfDbMin_, p->wfDbMax_, 0.f, 0.f};
    quadUbo(uWf, wf0, wf1, wf2);
    // The fill shader indexes columns in WIDGET pixels (one level each).
    const float fl0[4] = {float(cw), float(hSpec), PanadapterWidget::kFillGamma,
                          float(p->ds_.traceStyle)};
    quadUbo(uFill, fl0, nullptr, nullptr);
    if (want3d) {
        const float d0[4] = {float(v.binLo), float(v.binHi),
                             float(p->serial3d_ % PanadapterWidget::k3dRows), float(p->count3d_)};
        const float d1[4] = {p->floor3dDb_ - 6.0f, p->dbMax_ - p->dbMin_, float(W), float(hSpec)};
        const float d2[4] = {float(PanadapterWidget::k3dRows), float(p->bins3d_),
                             p->scroll3dPhase(),
                             p->ds_.traceStyle == 1 ? 1.f : 0.f};
        quadUbo(u3D, d0, d1, d2);
    }
    auto lineUbo = [&](int i, const QColor& c, float alpha) {
        float b[24] = {};
        std::memcpy(b, mvp.constData(), 64);
        b[16] = float(c.redF() * alpha); b[17] = float(c.greenF() * alpha);
        b[18] = float(c.blueF() * alpha); b[19] = alpha;
        b[20] = float(W); b[21] = float(H);
        b[22] = (i == uTrace && p->ds_.traceStyle == 1) ? 1.f : 0.f;   // heat colour
        b[23] = float(hSpec);
        u->updateDynamicBuffer(ubo_[i], 0, kLineUbo, b);
    };
    lineUbo(uPeak, QColor(235, 235, 245), 150.f / 255.f);
    lineUbo(uTrace, PanadapterWidget::traceColor(p->ds_.traceColor), 1.f);

    // ---- pipelines (lazily, once the render pass is known) ----
    QRhiRenderPassDescriptor* rp = renderTarget()->renderPassDescriptor();
    QRhiGraphicsPipeline::TargetBlend premul;
    premul.enable = true;
    premul.srcColor = QRhiGraphicsPipeline::One;
    premul.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    premul.srcAlpha = QRhiGraphicsPipeline::One;
    premul.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    QRhiVertexInputLayout quadLayout;
    quadLayout.setBindings({{4 * sizeof(float)}});
    quadLayout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0},
                              {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)}});
    auto makePipe = [&](const char* vs, const char* fs, QRhiShaderResourceBindings* layoutSrb,
                        bool blend, bool lines) {
        QRhiGraphicsPipeline* gp = r->newGraphicsPipeline();
        gp->setShaderStages({{QRhiShaderStage::Vertex, loadShader(vs)},
                             {QRhiShaderStage::Fragment, loadShader(fs)}});
        if (std::strcmp(vs, "pan_3d.vert") == 0) {
            QRhiVertexInputLayout ml;
            ml.setBindings({{3 * sizeof(float)}});
            ml.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0}});
            gp->setVertexInputLayout(ml);
            gp->setTopology(QRhiGraphicsPipeline::Triangles);
        } else if (lines && std::strcmp(vs, "pan_trace.vert") == 0) {
            QRhiVertexInputLayout tl;
            tl.setBindings({{3 * sizeof(float)}});
            tl.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0}});
            gp->setVertexInputLayout(tl);
            gp->setTopology(QRhiGraphicsPipeline::Triangles);
        } else if (lines) {
            QRhiVertexInputLayout ll;
            ll.setBindings({{2 * sizeof(float)}});
            ll.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
            gp->setVertexInputLayout(ll);
            gp->setTopology(QRhiGraphicsPipeline::LineStrip);
        } else {
            gp->setVertexInputLayout(quadLayout);
            gp->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        }
        if (blend) gp->setTargetBlends({premul});
        gp->setShaderResourceBindings(layoutSrb);
        gp->setRenderPassDescriptor(rp);
        gp->create();
        return gp;
    };
    const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
    if (!wfSrb_) {
        wfSrb_ = r->newShaderResourceBindings();
        wfSrb_->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, stages, ubo_[uWf]),
                             QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, hist_, nearest_),
                             QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, palette_, linear_)});
        wfSrb_->create();
    }
    if (!fillSrb_) {
        fillSrb_ = r->newShaderResourceBindings();
        fillSrb_->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, stages, ubo_[uFill]),
                               QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, cols_, nearest_),
                               QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, palette_, linear_)});
        fillSrb_->create();
    }
    if (!peakSrb_) {
        peakSrb_ = r->newShaderResourceBindings();
        peakSrb_->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, stages, ubo_[uPeak])});
        peakSrb_->create();
        traceSrb_ = r->newShaderResourceBindings();
        traceSrb_->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, stages, ubo_[uTrace])});
        traceSrb_->create();
    }
    if (!overlayPipe_) overlayPipe_ = makePipe("pan_quad.vert", "pan_overlay.frag", under_.srb, true, false);
    if (!wfPipe_) wfPipe_ = makePipe("pan_quad.vert", "pan_waterfall.frag", wfSrb_, false, false);
    if (!fillPipe_) fillPipe_ = makePipe("pan_quad.vert", "pan_fill.frag", fillSrb_, true, false);
    if (!linePipe_) linePipe_ = makePipe("pan_line.vert", "pan_line.frag", traceSrb_, true, true);
    if (!widePipe_) widePipe_ = makePipe("pan_trace.vert", "pan_trace.frag", traceSrb_, true, true);
    if (want3d && !srb3d_) {
        srb3d_ = r->newShaderResourceBindings();
        srb3d_->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, stages, ubo_[u3D]),
                             QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::VertexStage, hist3d_, nearest_),
                             QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::VertexStage, palette_, linear_)});
        srb3d_->create();
    }
    if (want3d && !pipe3d_) pipe3d_ = makePipe("pan_3d.vert", "pan_3d.frag", srb3d_, true, false);

    // ---- the frame, bottom to top ----
    cb->beginPass(renderTarget(), QColor(12, 16, 22), {1.0f, 0}, u);
    const QRhiCommandBuffer::VertexInput quadIn(quadVbo_, 0);
    const QRhiViewport full(0, 0, float(px.width()), float(px.height()));
    auto overlay = [&](Layer& l) {
        cb->setGraphicsPipeline(overlayPipe_);
        cb->setViewport(full);
        cb->setShaderResources(l.srb);
        cb->setVertexInput(0, 1, &quadIn);
        cb->draw(4);
    };
    // QRhi viewports are bottom-left based.
    auto region = [&](int top, int height) {
        return QRhiViewport(0, float(px.height() - (top + height) * dpr), float(px.width()),
                            float(height * dpr));
    };
    overlay(under_);
    if (haveWf) {
        cb->setGraphicsPipeline(wfPipe_);
        cb->setViewport(region(wfTop, H - wfTop));
        cb->setShaderResources(wfSrb_);
        cb->setVertexInput(0, 1, &quadIn);
        cb->draw(4);
    }
    if (want3d) {
        cb->setGraphicsPipeline(pipe3d_);
        cb->setViewport(region(0, hSpec));
        cb->setShaderResources(srb3d_);
        const QRhiCommandBuffer::VertexInput meshIn(mesh3d_, 0);
        cb->setVertexInput(0, 1, &meshIn);
        cb->draw(quint32(mesh3dVerts_));
    } else if (p->ds_.fillTrace && v.n >= 2 && hSpec > 1 && !colT.empty()) {
        cb->setGraphicsPipeline(fillPipe_);
        cb->setViewport(region(0, hSpec));
        cb->setShaderResources(fillSrb_);
        cb->setVertexInput(0, 1, &quadIn);
        cb->draw(4);
    }
    overlay(mid_);
    cb->setGraphicsPipeline(overlayPipe_);
    cb->setViewport(QRhiViewport(0, 0, float(stripW * dpr), float(px.height())));
    cb->setShaderResources(labels_.srb);
    cb->setVertexInput(0, 1, &quadIn);
    cb->draw(4);
    if (traceN > 1 && !want3d) {
        const QRhiCommandBuffer::VertexInput lineIn(lineVbo_, 0);
        cb->setGraphicsPipeline(linePipe_);
        cb->setViewport(full);
        cb->setVertexInput(0, 1, &lineIn);
        if (peakN > 1) {
            cb->setShaderResources(peakSrb_);
            cb->draw(quint32(peakN));
        }
        if (!quads.empty()) {
            const QRhiCommandBuffer::VertexInput wideIn(wideVbo_, 0);
            cb->setGraphicsPipeline(widePipe_);
            cb->setVertexInput(0, 1, &wideIn);
            cb->setShaderResources(traceSrb_);
            cb->draw(quint32(quads.size() / 3));
        } else {
            cb->setShaderResources(traceSrb_);
            cb->draw(quint32(traceN), 1, quint32(peakN));
        }
    }
    overlay(top_);
    overlay(cursor_);
    cb->endPass();
    gGpuProf.add(t.nsecsElapsed() / 1e6, layerMs, rebuilt);
    // 3D scrolls continuously: draw again at the next display refresh.
    if (want3d) update();
}

} // namespace ttc
