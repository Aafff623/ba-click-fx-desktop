#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace bafx::windows
{

// Simulation quality tiers from the reference implementation. The enum lives
// here, not in the configuration schema, so this library keeps its
// independence from bafx::config (the Host maps between the two).
enum class GlowSimulationQuality : std::uint8_t
{
    Low,
    Medium,
    High,
};

// Resolved appearance values; colors are sRGB bytes in [0, 1] exactly like
// the reference CSS colors (no linear-space conversion is applied, matching
// the WebGL reference visual).
struct GlowFluidAppearance final
{
    float colorR{0.486F};
    float colorG{0.227F};
    float colorB{0.929F};
    float secondaryR{0.659F};
    float secondaryG{0.333F};
    float secondaryB{0.969F};
    float intensity{0.3F};
    float glow{0.5F};
    float grain{0.15F};
    float hotspot{0.0F};
    // Physical-pixel edge fade width; the reference fades a fixed 2% of its
    // web container, which is wrong for a fullscreen desktop overlay.
    float edgeFadePx{2.0F};
};

// Splat payload in the target's native channel meaning: velocity splats use
// (gridVx, gridVy, 0), dye splats use (energy, heat, gas).
struct GlowSplatValue final
{
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

// Mirrors the reference splat shader's uMode contract.
enum class GlowSplatMode : std::uint8_t
{
    PointAdditive,
    PointMix,
    SegmentAdditive,
    SegmentMix,
};

// GPU fluid pipeline for the Glow style: ping-pong velocity/dye/pressure
// pairs plus single-target divergence, MacCormack forward/back and a five
// level bloom chain, all in the render owner's device. It shares the
// CompositionRenderer device, context and swap-chain render target, so the
// Windows底座 (window, DComp, present, device recovery) stays untouched.
//
// The class owns its whole GPU resource domain: allocation follows the
// viewport size and quality tier, every read/write pair is swapped after a
// pass so no texture is ever bound as SRV and RTV at the same time, and
// release() drops every target. All rendering happens on the caller's thread
// (the Host render thread), same as the rest of the composition renderer.
class GlowFluidPipeline final
{
public:
    GlowFluidPipeline(
        ID3D11Device* device,
        ID3D11DeviceContext* context);
    ~GlowFluidPipeline();

    GlowFluidPipeline(const GlowFluidPipeline&) = delete;
    GlowFluidPipeline& operator=(const GlowFluidPipeline&) = delete;

    void setQuality(GlowSimulationQuality quality) noexcept;
    void setViewportSize(float widthPx, float heightPx) noexcept;
    void setAppearance(const GlowFluidAppearance& appearance) noexcept;
    // Dye decay follows the reference: the three dye channels fade with
    // linger-scaled time constants, so this is the only simulation input
    // that comes from settings rather than per-frame calls.
    void setLinger(float seconds) noexcept;

    // Window-local pixels, y down. Velocity values are screen-space pixels
    // per second and are converted to grid units internally.
    void splatVelocity(
        float ax,
        float ay,
        float bx,
        float by,
        float radiusPx,
        GlowSplatValue value,
        GlowSplatMode mode,
        float strength) noexcept;
    void splatDye(
        float ax,
        float ay,
        float bx,
        float by,
        float radiusPx,
        float wideRadiusPx,
        GlowSplatValue value,
        GlowSplatMode mode,
        float strength) noexcept;

    // Advances the simulation by dtSeconds and composites the current state
    // into destination (the swap-chain render target). Both are no-ops
    // before the first allocation or after an internal GPU failure.
    void step(float dtSeconds) noexcept;
    void display(ID3D11RenderTargetView* destination) noexcept;

    // Zeroes the velocity, dye and pressure pairs, matching the reference's
    // clearTargets when the pointer goes quiet.
    void clearSimulation() noexcept;
    // Clears the simulation and the accumulated display time.
    void reset() noexcept;

    [[nodiscard]] bool healthy() const noexcept;
    [[nodiscard]] bool allocated() const noexcept;

    // Introspection for diagnostics and tests.
    [[nodiscard]] std::uint32_t simulationWidth() const noexcept;
    [[nodiscard]] std::uint32_t simulationHeight() const noexcept;
    [[nodiscard]] std::uint32_t dyeWidth() const noexcept;
    [[nodiscard]] std::uint32_t dyeHeight() const noexcept;
    [[nodiscard]] std::uint32_t bloomLevelCount() const noexcept;
    [[nodiscard]] std::uint32_t bloomWidth(std::uint32_t level) const noexcept;
    [[nodiscard]] ID3D11Texture2D* velocityReadTexture() const noexcept;
    [[nodiscard]] ID3D11Texture2D* dyeReadTexture() const noexcept;

private:
    struct Target;
    struct TargetPair;

    bool createResources();
    void allocate();
    void release() noexcept;

    // Maps the vertex-stage texel uniform; false marks the pipeline failed.
    bool updateVertexConstants(float texelX, float texelY);
    // Draws the fullscreen triangle into target with blendState applied.
    void drawPass(
        ID3D11PixelShader* shader,
        ID3D11Buffer* constants,
        const std::array<ID3D11ShaderResourceView*, 4>& views,
        const std::array<ID3D11SamplerState*, 2>& samplers,
        std::size_t samplerCount,
        ID3D11BlendState* blendState,
        const Target& target);
    void drawPassToRtv(
        ID3D11PixelShader* shader,
        ID3D11Buffer* constants,
        const std::array<ID3D11ShaderResourceView*, 4>& views,
        const std::array<ID3D11SamplerState*, 2>& samplers,
        ID3D11RenderTargetView* destination,
        float width,
        float height);
    // Shared splat execution for both ping-pong pairs.
    void splat(
        ID3D11PixelShader* shader,
        ID3D11Buffer* constants,
        ID3D11ShaderResourceView* source,
        ID3D11SamplerState* sampler,
        const TargetPair& pair,
        const struct SplatConstants& data);
    // Advects source into destination (or viewportTarget when null).
    void advect(
        ID3D11RenderTargetView* destination,
        const Target& viewportTarget,
        ID3D11ShaderResourceView* velocityView,
        ID3D11ShaderResourceView* sourceView,
        float dt,
        float decayX,
        float decayY,
        float decayZ,
        float decayW);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> fullscreenTriangle_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexConstants_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> splatShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> advectShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> maccormackShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> divergenceShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> scaleShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pressureShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> gradientShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> prefilterShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> downShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> upShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> displayShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> splatConstants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> advectConstants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> maccormackConstants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> scaleConstants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> prefilterConstants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> downConstants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> upConstants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> displayConstants_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearSampler_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> pointSampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendDisabled_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendAdditive_;

    std::unique_ptr<TargetPair> velocity_;
    std::unique_ptr<TargetPair> dye_;
    std::unique_ptr<TargetPair> pressure_;
    std::unique_ptr<Target> divergence_;
    std::unique_ptr<Target> forward_;
    std::unique_ptr<Target> back_;
    std::vector<std::unique_ptr<Target>> bloom_;

    GlowSimulationQuality quality_{GlowSimulationQuality::High};
    GlowFluidAppearance appearance_{};
    float linger_{0.3F};
    float viewportWidth_{0.0F};
    float viewportHeight_{0.0F};
    float timeSeconds_{0.0F};
    std::uint32_t allocatedQuality_{0U};
    std::uint32_t allocatedWidth_{0U};
    std::uint32_t allocatedHeight_{0U};
    bool resourcesFailed_{false};
    bool renderFailed_{false};
};

} // namespace bafx::windows
