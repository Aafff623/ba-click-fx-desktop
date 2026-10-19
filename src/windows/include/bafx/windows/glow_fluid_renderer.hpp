#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "bafx/windows/glow_fluid_pipeline.hpp"
#include "bafx/windows/glow_trail_state.hpp"

namespace bafx::windows
{

// Settings for the Glow style slice. Color arrives as sRGB bytes in [0, 1];
// the fluid pipeline owns the shading. Defaults are the 余烬 screenshot
// preset so an unconfigured style already matches the reference visual.
struct GlowRenderSettings final
{
    float colorR{0.486F};
    float colorG{0.227F};
    float colorB{0.929F};
    float intensity{0.3F};
    float radiusPx{3.0F};
    float linger{0.3F};
    float secondaryR{0.659F};
    float secondaryG{0.333F};
    float secondaryB{0.969F};
    float glow{0.5F};
    float grain{0.15F};
    float hotspot{0.0F};
    GlowSimulationQuality quality{GlowSimulationQuality::High};
};

// GPU renderer for the Glow style. It shares the CompositionRenderer device
// and renders into the same swap-chain render target, so the Windows底座
// (window, DComp, present, device recovery) stays untouched.
//
// The visual side is the fluid pipeline: pointer samples become velocity and
// dye splats, the simulation advances, and the display pass composites into
// the swap chain. The additive-quad placeholder remains as the degraded path
// when the pipeline cannot be created, and GlowTrailState keeps driving the
// idle contract (linger expiry, one transparent frame, then sleep).
class GlowFluidRenderer final
{
public:
    GlowFluidRenderer(
        ID3D11Device* device,
        ID3D11DeviceContext* context);

    GlowFluidRenderer(const GlowFluidRenderer&) = delete;
    GlowFluidRenderer& operator=(const GlowFluidRenderer&) = delete;

    void setSettings(const GlowRenderSettings& settings) noexcept;
    void setViewportSize(float widthPx, float heightPx) noexcept;

    // Window-local pixels, y down, seconds on a monotonic clock.
    void submitPointerSample(
        float x,
        float y,
        double nowSeconds) noexcept;

    // Expires trail samples against nowSeconds, then draws the current frame
    // into destination. When the trail just emptied, the target is cleared
    // transparent once so the overlay presents a transparent frame before
    // going idle.
    void render(ID3D11RenderTargetView* destination, double nowSeconds);

    [[nodiscard]] bool needsFrame() const noexcept;
    void reset() noexcept;

private:
    bool createResources();
    // Fluid path: consume new pointer samples as splats, advance the
    // simulation, composite into the swap chain.
    void renderFluid(ID3D11RenderTargetView* destination, double nowSeconds);
    void injectPendingSamples(float dt);
    // Degraded path used when the fluid pipeline failed to initialize.
    void renderPlaceholder(
        ID3D11RenderTargetView* destination, double nowSeconds);

    struct Vertex final
    {
        float x;
        float y;
        float alpha;
    };

    void appendSampleVertices(
        const GlowTrailSample& sample,
        double nowSeconds,
        std::vector<Vertex>& vertices) const noexcept;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> transformBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> shadeBuffer_;
    std::unique_ptr<GlowFluidPipeline> pipeline_{};

    GlowTrailState trail_{};
    GlowRenderSettings settings_{};
    float viewportWidth_{1.0F};
    float viewportHeight_{1.0F};
    std::size_t vertexCapacity_{0U};
    bool resourcesFailed_{false};
    // Fluid-frame bookkeeping: frame delta, consumed sample cursor and the
    // last injected position so motion becomes a segment splat.
    double lastFrameSeconds_{0.0};
    std::uint64_t consumedSequence_{0U};
    bool hasEmittedPosition_{false};
    float emittedX_{0.0F};
    float emittedY_{0.0F};
};

}
