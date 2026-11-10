#include "overlay/TextRim.h"
namespace sat {
namespace {
class RimRenderer final : public IDWriteTextRenderer {
    ULONG refs_{1};
    ID2D1RenderTarget* target_;
    ID2D1Factory* factory_;
    ID2D1SolidColorBrush* fill_;
    ID2D1SolidColorBrush* edge_;
    float stroke_;
    int style_;
    float scale_;
    // Round joins keep large strokes free of miter spikes, which otherwise shoot out of the sharp
    // corners a glyph outline carries.
    ComPtr<ID2D1StrokeStyle> strokeStyle_;

    // The edge sits outside the glyph fill, so stacked strokes of decreasing width read as a rough
    // or glowing rim while the fill stays crisp on top.
    void PaintEdge(ID2D1PathGeometry* geometry, const D2D1_MATRIX_3X2_F& base) {
        if (style_ == 1) {
            // Rough edge: five rims of different widths and small offsets. The offsets come from a
            // fixed table so the rim looks identical on every repaint instead of crawling.
            static const float kOffset[5][2] = {{0.34f, 0.12f}, {-0.29f, 0.41f}, {0.18f, -0.37f},
                                                {-0.23f, -0.16f}, {0.05f, 0.44f}};
            static const float kWidth[5] = {1.00f, 1.28f, 1.55f, 1.86f, 2.14f};
            static const float kAlpha[5] = {0.86f, 0.68f, 0.52f, 0.38f, 0.24f};
            for (int i = 0; i < 5; ++i) {
                const float dx = kOffset[i][0] * stroke_ * scale_;
                const float dy = kOffset[i][1] * stroke_ * scale_;
                target_->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy) * base);
                edge_->SetOpacity(kAlpha[i]);
                target_->DrawGeometry(geometry, edge_, 2 * stroke_ * kWidth[i], strokeStyle_.Get());
            }
            edge_->SetOpacity(1.f);
            return;
        }
        target_->SetTransform(base);
        if (style_ == 2) {
            // Glow: three rims from wide to narrow. The software render target offers no image
            // effects, so the falloff comes from stacking translucent strokes of the same colour.
            static const float kWidth[3] = {4.0f, 2.4f, 1.0f};
            static const float kAlpha[3] = {0.16f, 0.30f, 1.f};
            for (int i = 0; i < 3; ++i) {
                edge_->SetOpacity(kAlpha[i]);
                target_->DrawGeometry(geometry, edge_, 2 * stroke_ * kWidth[i], strokeStyle_.Get());
            }
            edge_->SetOpacity(1.f);
            return;
        }
        target_->DrawGeometry(geometry, edge_, 2 * stroke_, strokeStyle_.Get());
    }

public:
    RimRenderer(ID2D1RenderTarget* t, ID2D1Factory* f, ID2D1SolidColorBrush* fill,
                ID2D1SolidColorBrush* edge, float stroke, int style, float scale)
        : target_(t), factory_(f), fill_(fill), edge_(edge), stroke_(stroke), style_(style),
          scale_(scale) {
        f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                          D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND),
                              nullptr, 0, &strokeStyle_);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID i, void** out) override {
        if (!out)
            return E_POINTER;
        if (i == __uuidof(IUnknown) || i == __uuidof(IDWritePixelSnapping) ||
            i == __uuidof(IDWriteTextRenderer)) {
            *out = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto n = --refs_;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* value) override {
        *value = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* value) override {
        D2D1_MATRIX_3X2_F m;
        target_->GetTransform(&m);
        static_assert(sizeof(m) == sizeof(*value));
        memcpy(value, &m, sizeof(m));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* value) override {
        *value = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE,
                                           const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*,
                                           IUnknown*) override {
        ComPtr<ID2D1PathGeometry> geometry;
        HRESULT hr = factory_->CreatePathGeometry(&geometry);
        if (FAILED(hr))
            return hr;
        ComPtr<ID2D1GeometrySink> sink;
        hr = geometry->Open(&sink);
        if (FAILED(hr))
            return hr;
        hr = run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances,
                                               run->glyphOffsets, run->glyphCount, run->isSideways,
                                               run->bidiLevel % 2, sink.Get());
        if (FAILED(hr))
            return hr;
        hr = sink->Close();
        if (FAILED(hr))
            return hr;
        D2D1_MATRIX_3X2_F previous;
        target_->GetTransform(&previous);
        const auto base = D2D1::Matrix3x2F::Translation(x, y) * previous;
        if (stroke_ > 0)
            PaintEdge(geometry.Get(), base);
        // The rim styles move the transform, so the fill is placed again on the untranslated run.
        target_->SetTransform(base);
        target_->FillGeometry(geometry.Get(), fill_);
        target_->SetTransform(previous);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*,
                                            IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*,
                                                IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL,
                                               IUnknown*) override {
        return E_NOTIMPL;
    }
};
} // namespace
ComPtr<IDWriteTextRenderer> CreateRimRenderer(ID2D1RenderTarget* target, ID2D1Factory* factory,
                                              ID2D1SolidColorBrush* fill, ID2D1SolidColorBrush* edge,
                                              float stroke, int style, float scale) {
    ComPtr<IDWriteTextRenderer> renderer;
    renderer.Attach(new RimRenderer(target, factory, fill, edge, stroke, style, scale));
    return renderer;
}
} // namespace sat
