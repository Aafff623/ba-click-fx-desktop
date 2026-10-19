#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstddef>
#include <vector>

#include "bafx/windows/glow_trail_state.hpp"

namespace bafx::windows
{

// Placeholder settings for the Glow style slice. Color arrives as sRGB bytes
// in [0, 1]; the real fluid pipeline owns its own shading in the next ticket.
struct GlowRenderSettings final
{
    float colorR{0.486F};
    float colorG{0.227F};
    float colorB{0.929F};
    float intensity{0.3F};
    float radiusPx{3.0F};
    float linger{0.3F};
};

// GPU placeholder for the Glow style: additive trail quads along the pointer
// path, faded by the linger window. It shares the CompositionRenderer device
// and renders into the same swap-chain render target, so the Windows底座
// (window, DComp, present, device recovery) stays untouched. The fluid
// simulation replaces the visual side without changing this seam.
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

    // Expires trail samples against nowSeconds, then draws the trail into
    // destination. When the trail just emptied, the target is cleared
    // transparent once so the overlay presents a transparent frame before
    // going idle.
    void render(ID3D11RenderTargetView* destination, double nowSeconds);

    [[nodiscard]] bool needsFrame() const noexcept;
    void reset() noexcept;

private:
    bool createResources();

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

    GlowTrailState trail_{};
    GlowRenderSettings settings_{};
    float viewportWidth_{1.0F};
    float viewportHeight_{1.0F};
    std::size_t vertexCapacity_{0U};
    bool resourcesFailed_{false};
};

}
