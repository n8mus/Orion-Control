// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QImage>
#include <QRhiWidget>
#include <QVector>
#include <cstdint>
#include <memory>
#include <vector>

class QRhi;
class QRhiBuffer;
class QRhiGraphicsPipeline;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;

namespace ttc {

class PanadapterWidget;

// The panadapter on the GPU (QRhi over OpenGL here), AetherSDR's design: the
// waterfall lives on the card as a raw-dB float texture (the palette and the
// view window are shader parameters, so zoom/tune/contrast are free), the
// fill and trace are drawn per frame from one level per pixel column, and
// everything with text — grid, scales, VFOs, spots, the boxes — is the
// SAME QPainter code as the CPU paint, rendered into cached layer images
// only when something changed and composited by the GPU. Owned by, and
// stacked under the events of, PanadapterWidget; any failure falls back
// to the CPU paint (renderFailed).
class PanGpuView : public QRhiWidget {
    Q_OBJECT
public:
    explicit PanGpuView(PanadapterWidget* pan);
    ~PanGpuView() override;

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

private:
    struct Layer {
        QImage img;
        QRhiTexture* tex = nullptr;
        QRhiShaderResourceBindings* srb = nullptr;
        bool dirty = true;
    };
    void ensureLayer(Layer& l, const QSize& sz);
    void ensureHist(int bins);
    void freeAll();

    PanadapterWidget* pan_;
    QRhi* rhi_ = nullptr;
    QSize size_;
    // Geometry
    QRhiBuffer* quadVbo_ = nullptr;
    QRhiBuffer* lineVbo_ = nullptr;
    int lineVboCap_ = 0;
    QRhiBuffer* wideVbo_ = nullptr;                // wide/heat trace quads
    int wideVboCap_ = 0;
    // One uniform block per draw (they differ per frame).
    enum Ubo { uUnder, uWf, uFill, uMid, uLabels, uPeak, uTrace, uTop, uCursor, u3D, uCount };
    QRhiBuffer* ubo_[uCount] = {};
    QRhiSampler* linear_ = nullptr;
    QRhiSampler* nearest_ = nullptr;
    // Waterfall history + palette + per-column levels.
    QRhiTexture* hist_ = nullptr;
    int histBins_ = 0;
    uint64_t histSerial_ = 0, histReset_ = ~0ull;
    QRhiTexture* palette_ = nullptr;
    int paletteId_ = -1;
    QRhiTexture* cols_ = nullptr;
    int colsW_ = 0;
    // Layers (premultiplied QPainter images).
    Layer under_, mid_, labels_, top_, cursor_;
    QRect cursorDrawn_;                            // widget px, last frame's cursor
    // Pipelines
    QRhiGraphicsPipeline* overlayPipe_ = nullptr;
    QRhiGraphicsPipeline* wfPipe_ = nullptr;
    QRhiGraphicsPipeline* fillPipe_ = nullptr;
    QRhiGraphicsPipeline* linePipe_ = nullptr;
    QRhiGraphicsPipeline* widePipe_ = nullptr;
    QRhiShaderResourceBindings* wfSrb_ = nullptr;
    QRhiShaderResourceBindings* fillSrb_ = nullptr;
    QRhiShaderResourceBindings* peakSrb_ = nullptr;
    QRhiShaderResourceBindings* traceSrb_ = nullptr;
    // 3D stacked traces: history ring texture + the static perspective mesh.
    QRhiTexture* hist3d_ = nullptr;
    int hist3dBins_ = 0;
    uint64_t hist3dSerial_ = 0, hist3dReset_ = ~0ull;
    QRhiBuffer* mesh3d_ = nullptr;
    int mesh3dVerts_ = 0;
    QRhiGraphicsPipeline* pipe3d_ = nullptr;
    QRhiShaderResourceBindings* srb3d_ = nullptr;
    bool pipesBuilt_ = false, quadUploaded_ = false;
};

} // namespace ttc
