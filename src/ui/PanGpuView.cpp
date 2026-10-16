// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/PanGpuView.h"

#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
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
// TTC_PANPROF=1: GPU frame cost on the CPU side (layer rebuilds + uploads
// + command recording), same cadence as the CPU path's [panprof].
struct GpuProf {
    bool on = qEnvironmentVariableIsSet("TTC_PANPROF");
    QElapsedTimer window;
    double renderMs = 0, layerMs = 0;
    int frames = 0, rebuilds = 0;
    void add(double r, double l, bool rebuilt) {
        if (!on) return;
        if (!window.isValid()) window.start();
        renderMs += r; layerMs += l; ++frames; rebuilds += rebuilt;
        if (window.elapsed() < 5000) return;
        const double secs = window.elapsed() / 1000.0;
        std::fprintf(stderr, "[panprof-gpu] %.1f fps | %.2f ms/frame on the CPU (layers %.2f, "
                             "%d rebuilds) | %.0f%% of a core\n",
                     frames / secs, renderMs / frames, layerMs / frames, rebuilds,
                     100.0 * renderMs / (secs * 1000.0));
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
    for (Layer* l : {&under_, &mid_, &labels_, &top_}) {
        del(l->tex);
        del(l->srb);
        l->img = QImage();
        l->dirty = true;
    }
    del(overlayPipe_); del(wfPipe_); del(fillPipe_); del(linePipe_);
    del(wfSrb_); del(fillSrb_); del(peakSrb_); del(traceSrb_);
    del(quadVbo_); del(lineVbo_);
    for (auto*& u : ubo_) del(u);
    del(linear_); del(nearest_);
    del(hist_); del(palette_); del(cols_);
    histBins_ = colsW_ = lineVboCap_ = 0;
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
    for (Layer* l : {&under_, &mid_, &labels_, &top_}) ensureLayer(*l, QSize(2, 2));
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
    const int ui = &l == &under_ ? uUnder : &l == &mid_ ? uMid : &l == &labels_ ? uLabels : uTop;
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
    for (Layer* l : {&under_, &mid_, &top_}) ensureLayer(*l, px);
    ensureLayer(labels_, QSize(int(stripW * dpr), px.height()));
    if (p->gpuDirtyUnder_) under_.dirty = true;
    if (p->gpuDirtyMid_) mid_.dirty = true;
    if (p->gpuDirtyTop_) top_.dirty = true;
    if (p->gpuDirtyLabels_) labels_.dirty = true;
    p->gpuDirtyUnder_ = p->gpuDirtyMid_ = p->gpuDirtyTop_ = p->gpuDirtyLabels_ = false;
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
    paint(top_, [&](QPainter& qp) { p->paintTop(qp, hSpec, v.n); });
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
    for (int i : {uUnder, uMid, uLabels, uTop}) quadUbo(i, nullptr, nullptr, nullptr);
    const float wf0[4] = {float(v.binLo), float(v.binHi), float(px.width()),
                          float(std::max(1.0, (H - wfTop) * dpr))};
    const float wf1[4] = {float(p->wfSerial_ % PanadapterWidget::kWfHistRows),
                          float(p->wfCount_), float(PanadapterWidget::kWfHistRows), 0.f};
    const float wf2[4] = {p->wfDbMin_, p->wfDbMax_, 0.f, 0.f};
    quadUbo(uWf, wf0, wf1, wf2);
    // The fill shader indexes columns in WIDGET pixels (one level each).
    const float fl0[4] = {float(cw), float(hSpec), PanadapterWidget::kFillGamma, 0.f};
    quadUbo(uFill, fl0, nullptr, nullptr);
    auto lineUbo = [&](int i, const QColor& c, float alpha) {
        float b[24] = {};
        std::memcpy(b, mvp.constData(), 64);
        b[16] = float(c.redF() * alpha); b[17] = float(c.greenF() * alpha);
        b[18] = float(c.blueF() * alpha); b[19] = alpha;
        b[20] = float(W); b[21] = float(H);
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
        if (lines) {
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
    if (p->ds_.fillTrace && v.n >= 2 && hSpec > 1 && !colT.empty()) {
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
    if (traceN > 1) {
        const QRhiCommandBuffer::VertexInput lineIn(lineVbo_, 0);
        cb->setGraphicsPipeline(linePipe_);
        cb->setViewport(full);
        cb->setVertexInput(0, 1, &lineIn);
        if (peakN > 1) {
            cb->setShaderResources(peakSrb_);
            cb->draw(quint32(peakN));
        }
        cb->setShaderResources(traceSrb_);
        cb->draw(quint32(traceN), 1, quint32(peakN));
    }
    overlay(top_);
    cb->endPass();
    gGpuProf.add(t.nsecsElapsed() / 1e6, layerMs, rebuilt);
}

} // namespace ttc
