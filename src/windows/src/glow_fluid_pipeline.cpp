#include "bafx/windows/glow_fluid_pipeline.hpp"

#include "bafx/windows/error.hpp"

#include "glow_fluid_shaders.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#pragma comment(lib, "d3dcompiler.lib")

namespace bafx::windows
{

namespace
{
constexpr std::uint32_t kPressureSteps = 20U;
constexpr std::uint32_t kBloomLevels = 5U;
constexpr float kVelocityDamping = 1.2F;
constexpr float kDyeDecayRate = 4.6F;
constexpr std::uint32_t kMinimumTargetSide = 2U;

struct QualityResolution final
{
    std::uint32_t simulation{0U};
    std::uint32_t dye{0U};
};

[[nodiscard]] QualityResolution resolutionFor(GlowSimulationQuality quality)
{
    switch (quality)
    {
    case GlowSimulationQuality::Low:
        return QualityResolution{96U, 384U};
    case GlowSimulationQuality::Medium:
        return QualityResolution{128U, 576U};
    case GlowSimulationQuality::High:
    default:
        return QualityResolution{160U, 768U};
    }
}

[[nodiscard]] float modeAsFloat(GlowSplatMode mode)
{
    switch (mode)
    {
    case GlowSplatMode::PointAdditive:
        return 0.0F;
    case GlowSplatMode::PointMix:
        return 1.0F;
    case GlowSplatMode::SegmentAdditive:
        return 2.0F;
    case GlowSplatMode::SegmentMix:
    default:
        return 3.0F;
    }
}

// Constant buffer mirrors. Offsets follow HLSL cbuffer packing (float3 is
// 16-byte aligned); glow_fluid_shader_tests.cpp pins them via reflection.
struct TransformConstants final
{
    float texelX{0.0F};
    float texelY{0.0F};
    float pad0{0.0F};
    float pad1{0.0F};
};

struct AdvectConstants final
{
    float velocityTexelX{0.0F};
    float velocityTexelY{0.0F};
    float dt{0.0F};
    float pad0{0.0F};
    float decayX{1.0F};
    float decayY{1.0F};
    float decayZ{1.0F};
    float decayW{1.0F};
};

struct MaccormackConstants final
{
    float velocityTexelX{0.0F};
    float velocityTexelY{0.0F};
    float sourceTexelX{0.0F};
    float sourceTexelY{0.0F};
    float dt{0.0F};
    float pad0{0.0F};
    float pad1{0.0F};
    float pad2{0.0F};
    float decayX{1.0F};
    float decayY{1.0F};
    float decayZ{1.0F};
    float decayW{1.0F};
};

struct ScaleConstants final
{
    float value{0.0F};
    float pad0{0.0F};
    float pad1{0.0F};
    float pad2{0.0F};
};

struct PrefilterConstants final
{
    float dyeTexelX{0.0F};  // 0
    float dyeTexelY{0.0F};  // 4
    float pad0{0.0F};       // 8
    float pad1{0.0F};       // 12
    float colorR{0.0F};     // 16
    float colorG{0.0F};     // 20
    float colorB{0.0F};     // 24
    float pad2{0.0F};       // 28
    float secondaryR{0.0F}; // 32
    float secondaryG{0.0F}; // 36
    float secondaryB{0.0F}; // 40
    float intensity{0.0F};  // 44
    float hotspot{0.0F};    // 48
    float pad4{0.0F};       // 52
    float pad5{0.0F};       // 56
    float pad6{0.0F};       // 60
};

struct TexelConstants final
{
    float texelX{0.0F};
    float texelY{0.0F};
    float pad0{0.0F};
    float pad1{0.0F};
};

struct DisplayConstants final
{
    float colorR{0.0F};     // 0
    float colorG{0.0F};     // 4
    float colorB{0.0F};     // 8
    float pad0{0.0F};       // 12
    float secondaryR{0.0F}; // 16
    float secondaryG{0.0F}; // 20
    float secondaryB{0.0F}; // 24
    float intensity{0.0F};  // 28
    float hotspot{0.0F};    // 32
    float glow{0.0F};       // 36
    float grain{0.0F};      // 40
    float time{0.0F};       // 44
    float light{0.0F};      // 48
    float pad2{0.0F};       // 52
    float pad3{0.0F};       // 56
    float pad4{0.0F};       // 60
};

static_assert(sizeof(TransformConstants) == 16U);
static_assert(sizeof(AdvectConstants) == 32U);
static_assert(sizeof(MaccormackConstants) == 48U);
static_assert(sizeof(ScaleConstants) == 16U);
static_assert(sizeof(PrefilterConstants) == 64U);
static_assert(sizeof(TexelConstants) == 16U);
static_assert(sizeof(DisplayConstants) == 64U);

[[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> compileShader(
    const std::string& source,
    const char* entryPoint,
    const char* profile)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    Microsoft::WRL::ComPtr<ID3DBlob> byteCode;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source.data(),
        source.size(),
        "glow_fluid_pipeline.cpp",
        nullptr,
        nullptr,
        entryPoint,
        profile,
        flags,
        0,
        &byteCode,
        &errors);
    if (FAILED(result))
    {
        std::string message = "D3DCompile failed";
        if (errors != nullptr && errors->GetBufferPointer() != nullptr)
        {
            message += ": ";
            message.append(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        throw std::runtime_error(message);
    }
    return byteCode;
}

} // namespace

// Declared (incomplete) in glow_fluid_pipeline.hpp for the splat helper
// signature; defined here at namespace scope so the declaration and the
// definition name the same type. Layout follows HLSL cbuffer packing: values
// pack tightly at 4-byte alignment and may only start a new 16-byte row when
// they would otherwise cross one (so float3 skips from 28 to 32).
struct SplatConstants final
{
    float aspect{0.0F};    // 0
    float aX{0.0F};        // 4
    float aY{0.0F};        // 8
    float pad0{0.0F};      // 12 (uB would cross the 16-byte row from here)
    float bX{0.0F};        // 16
    float bY{0.0F};        // 20
    float radius{0.0F};    // 24
    float wide{0.0F};      // 28
    float valueX{0.0F};    // 32
    float valueY{0.0F};    // 36
    float valueZ{0.0F};    // 40
    float mode{0.0F};      // 44
    float strength{0.0F};  // 48
    float pad1{0.0F};      // 52
    float pad2{0.0F};      // 56
    float pad3{0.0F};      // 60
};

static_assert(sizeof(SplatConstants) == 64U);

struct GlowFluidPipeline::Target final
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture{};
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv{};
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv{};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    float texelX{0.0F};
    float texelY{0.0F};
};

struct GlowFluidPipeline::TargetPair final
{
    Target read{};
    Target write{};

    void swap() noexcept
    {
        std::swap(read, write);
    }
};

GlowFluidPipeline::GlowFluidPipeline(
    ID3D11Device* device,
    ID3D11DeviceContext* context)
    : device_{device}
    , context_{context}
{
    if (device == nullptr || context == nullptr)
    {
        throw std::invalid_argument(
            "GlowFluidPipeline requires a device and context");
    }
    resourcesFailed_ = !createResources();
}

GlowFluidPipeline::~GlowFluidPipeline() = default;

void GlowFluidPipeline::setQuality(const GlowSimulationQuality quality) noexcept
{
    quality_ = quality;
}

void GlowFluidPipeline::setViewportSize(
    const float widthPx,
    const float heightPx) noexcept
{
    viewportWidth_ = std::max(widthPx, 1.0F);
    viewportHeight_ = std::max(heightPx, 1.0F);
}

void GlowFluidPipeline::setAppearance(
    const GlowFluidAppearance& appearance) noexcept
{
    appearance_ = appearance;
}

void GlowFluidPipeline::setLinger(const float seconds) noexcept
{
    linger_ = std::max(seconds, 0.1F);
}

bool GlowFluidPipeline::createResources()
{
    try
    {
        const std::string shade{kGlowFluidShadeChunk};

        const Microsoft::WRL::ComPtr<ID3DBlob> vertexByteCode =
            compileShader(kGlowFluidVertexShader, "main", "vs_5_0");
        throwIfFailed(
            device_->CreateVertexShader(
                vertexByteCode->GetBufferPointer(),
                vertexByteCode->GetBufferSize(),
                nullptr,
                &vertexShader_),
            "CreateVertexShader(glow fluid)");

        // One immutable fullscreen triangle shared by every pass.
        const std::array<float, 6> triangle{
            -1.0F, -1.0F, 3.0F, -1.0F, -1.0F, 3.0F};
        D3D11_BUFFER_DESC triangleDescription{};
        triangleDescription.ByteWidth = static_cast<UINT>(sizeof(triangle));
        triangleDescription.Usage = D3D11_USAGE_IMMUTABLE;
        triangleDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA triangleData{};
        triangleData.pSysMem = triangle.data();
        throwIfFailed(
            device_->CreateBuffer(
                &triangleDescription, &triangleData, &fullscreenTriangle_),
            "CreateBuffer(glow fluid triangle)");
        const std::array<D3D11_INPUT_ELEMENT_DESC, 1U> layout{
            D3D11_INPUT_ELEMENT_DESC{
                "POSITION",
                0,
                DXGI_FORMAT_R32G32_FLOAT,
                0,
                0,
                D3D11_INPUT_PER_VERTEX_DATA,
                0}};
        throwIfFailed(
            device_->CreateInputLayout(
                layout.data(),
                static_cast<UINT>(layout.size()),
                vertexByteCode->GetBufferPointer(),
                vertexByteCode->GetBufferSize(),
                &inputLayout_),
            "CreateInputLayout(glow fluid)");

        const auto buildPixelShader =
            [this](const std::string& source, ID3D11PixelShader** shader)
        {
            // Every pass shares the PSInput contract from the pixel header;
            // the prefilter and display passes additionally concatenate the
            // shared shade chunk, mirroring the reference ${HEADER}${SHADE}
            // composition.
            const std::string combined{kGlowFluidPixelHeader + source};
            const Microsoft::WRL::ComPtr<ID3DBlob> byteCode =
                compileShader(combined, "main", "ps_5_0");
            throwIfFailed(
                device_->CreatePixelShader(
                    byteCode->GetBufferPointer(),
                    byteCode->GetBufferSize(),
                    nullptr,
                    shader),
                "CreatePixelShader(glow fluid)");
        };
        buildPixelShader(kGlowFluidSplatShader, &splatShader_);
        buildPixelShader(kGlowFluidAdvectShader, &advectShader_);
        buildPixelShader(kGlowFluidMaccormackShader, &maccormackShader_);
        buildPixelShader(kGlowFluidDivergenceShader, &divergenceShader_);
        buildPixelShader(kGlowFluidScaleShader, &scaleShader_);
        buildPixelShader(kGlowFluidPressureShader, &pressureShader_);
        buildPixelShader(kGlowFluidGradientShader, &gradientShader_);
        buildPixelShader(
            shade + kGlowFluidPrefilterShader, &prefilterShader_);
        buildPixelShader(kGlowFluidDownShader, &downShader_);
        buildPixelShader(kGlowFluidUpShader, &upShader_);
        buildPixelShader(
            shade + kGlowFluidDisplayShader, &displayShader_);

        const auto createConstantBuffer =
            [this](UINT byteWidth, ID3D11Buffer** buffer)
        {
            D3D11_BUFFER_DESC description{};
            description.ByteWidth = byteWidth;
            description.Usage = D3D11_USAGE_DYNAMIC;
            description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            throwIfFailed(
                device_->CreateBuffer(&description, nullptr, buffer),
                "CreateBuffer(glow fluid constants)");
        };
        createConstantBuffer(sizeof(TransformConstants), &vertexConstants_);
        createConstantBuffer(sizeof(SplatConstants), &splatConstants_);
        createConstantBuffer(sizeof(AdvectConstants), &advectConstants_);
        createConstantBuffer(sizeof(MaccormackConstants), &maccormackConstants_);
        createConstantBuffer(sizeof(ScaleConstants), &scaleConstants_);
        createConstantBuffer(sizeof(PrefilterConstants), &prefilterConstants_);
        createConstantBuffer(sizeof(TexelConstants), &downConstants_);
        createConstantBuffer(sizeof(TexelConstants), &upConstants_);
        createConstantBuffer(sizeof(DisplayConstants), &displayConstants_);

        D3D11_SAMPLER_DESC linearDescription{};
        linearDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        linearDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        linearDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        linearDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        throwIfFailed(
            device_->CreateSamplerState(&linearDescription, &linearSampler_),
            "CreateSamplerState(glow fluid linear)");

        D3D11_SAMPLER_DESC pointDescription{};
        pointDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        pointDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        pointDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        pointDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        throwIfFailed(
            device_->CreateSamplerState(&pointDescription, &pointSampler_),
            "CreateSamplerState(glow fluid point)");

        D3D11_BLEND_DESC disabledDescription{};
        disabledDescription.RenderTarget[0].BlendEnable = FALSE;
        disabledDescription.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;
        throwIfFailed(
            device_->CreateBlendState(&disabledDescription, &blendDisabled_),
            "CreateBlendState(glow fluid disabled)");

        // Every pass draws the same fullscreen triangle; its winding is
        // counter-clockwise in NDC, which the default back-face cull would
        // discard. Cull none, exactly like the authored FX fullscreen passes.
        D3D11_RASTERIZER_DESC rasterizerDescription{};
        rasterizerDescription.FillMode = D3D11_FILL_SOLID;
        rasterizerDescription.CullMode = D3D11_CULL_NONE;
        rasterizerDescription.DepthClipEnable = TRUE;
        throwIfFailed(
            device_->CreateRasterizerState(
                &rasterizerDescription, &rasterizer_),
            "CreateRasterizerState(glow fluid)");

        // The bloom up-chain accumulates additively into the previous level.
        D3D11_BLEND_DESC additiveDescription{};
        additiveDescription.RenderTarget[0].BlendEnable = TRUE;
        additiveDescription.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        additiveDescription.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        additiveDescription.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        additiveDescription.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        additiveDescription.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        additiveDescription.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        additiveDescription.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;
        throwIfFailed(
            device_->CreateBlendState(&additiveDescription, &blendAdditive_),
            "CreateBlendState(glow fluid additive)");
        return true;
    }
    catch (const std::exception&)
    {
        // Resource creation failures degrade to a disabled style; the Host
        // keeps running the BA path and reports the style as inactive.
        return false;
    }
}

void GlowFluidPipeline::allocate()
{
    if (resourcesFailed_ || viewportWidth_ <= 0.0F || viewportHeight_ <= 0.0F)
    {
        return;
    }
    const auto width = static_cast<std::uint32_t>(viewportWidth_);
    const auto height = static_cast<std::uint32_t>(viewportHeight_);
    if (velocity_ != nullptr
        && allocatedQuality_ == static_cast<std::uint32_t>(quality_)
        && allocatedWidth_ == width && allocatedHeight_ == height)
    {
        return;
    }

    release();
    allocatedQuality_ = static_cast<std::uint32_t>(quality_);
    allocatedWidth_ = width;
    allocatedHeight_ = height;

    const QualityResolution resolution = resolutionFor(quality_);
    const float aspect = viewportWidth_ / viewportHeight_;
    const auto longSide = [aspect](std::uint32_t base)
    {
        return static_cast<std::uint32_t>(std::lround(
            static_cast<float>(base)
            * (aspect >= 1.0F ? aspect : 1.0F / aspect)));
    };
    const auto shortSide = [](std::uint32_t base)
    {
        return static_cast<std::uint32_t>(base);
    };

    std::uint32_t simulationWidth =
        aspect >= 1.0F ? longSide(resolution.simulation)
                       : shortSide(resolution.simulation);
    std::uint32_t simulationHeight =
        aspect >= 1.0F ? shortSide(resolution.simulation)
                       : longSide(resolution.simulation);
    std::uint32_t dyeWidth =
        aspect >= 1.0F ? longSide(resolution.dye) : shortSide(resolution.dye);
    std::uint32_t dyeHeight =
        aspect >= 1.0F ? shortSide(resolution.dye) : longSide(resolution.dye);
    // Dye never exceeds the swap-chain resolution, mirroring the reference
    // shrink against the canvas pixel size.
    const float shrink = std::min(
        1.0F,
        std::min(
            viewportWidth_ / static_cast<float>(dyeWidth),
            viewportHeight_ / static_cast<float>(dyeHeight)));
    dyeWidth = std::max(
        kMinimumTargetSide,
        static_cast<std::uint32_t>(std::lround(
            static_cast<float>(dyeWidth) * shrink)));
    dyeHeight = std::max(
        kMinimumTargetSide,
        static_cast<std::uint32_t>(std::lround(
            static_cast<float>(dyeHeight) * shrink)));

    const auto createTarget =
        [this](
            std::uint32_t targetWidth,
            std::uint32_t targetHeight,
            DXGI_FORMAT format) -> std::unique_ptr<Target>
    {
        auto target = std::make_unique<Target>();
        target->width = targetWidth;
        target->height = targetHeight;
        target->texelX = 1.0F / static_cast<float>(targetWidth);
        target->texelY = 1.0F / static_cast<float>(targetHeight);

        D3D11_TEXTURE2D_DESC description{};
        description.Width = targetWidth;
        description.Height = targetHeight;
        description.MipLevels = 1U;
        description.ArraySize = 1U;
        description.Format = format;
        description.SampleDesc.Count = 1U;
        description.SampleDesc.Quality = 0U;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags =
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        throwIfFailed(
            device_->CreateTexture2D(&description, nullptr, &target->texture),
            "CreateTexture2D(glow fluid target)");
        throwIfFailed(
            device_->CreateRenderTargetView(
                target->texture.Get(),
                nullptr,
                &target->rtv),
            "CreateRenderTargetView(glow fluid target)");
        throwIfFailed(
            device_->CreateShaderResourceView(
                target->texture.Get(),
                nullptr,
                &target->srv),
            "CreateShaderResourceView(glow fluid target)");

        // Fresh textures have undefined contents; zero them so the first
        // frame and any never-written bloom level read as empty.
        const std::array<float, 4> transparent{0.0F, 0.0F, 0.0F, 0.0F};
        context_->ClearRenderTargetView(target->rtv.Get(), transparent.data());
        return target;
    };
    const auto createPair =
        [&createTarget](
            std::uint32_t targetWidth,
            std::uint32_t targetHeight,
            DXGI_FORMAT format) -> std::unique_ptr<TargetPair>
    {
        auto pair = std::make_unique<TargetPair>();
        pair->read = *createTarget(targetWidth, targetHeight, format);
        pair->write = *createTarget(targetWidth, targetHeight, format);
        return pair;
    };

    try
    {
        velocity_ = createPair(
            simulationWidth,
            simulationHeight,
            DXGI_FORMAT_R16G16_FLOAT);
        dye_ = createPair(
            dyeWidth,
            dyeHeight,
            DXGI_FORMAT_R16G16B16A16_FLOAT);
        pressure_ = createPair(
            simulationWidth,
            simulationHeight,
            DXGI_FORMAT_R16_FLOAT);
        divergence_ = createTarget(
            simulationWidth,
            simulationHeight,
            DXGI_FORMAT_R16_FLOAT);
        forward_ = createTarget(
            dyeWidth,
            dyeHeight,
            DXGI_FORMAT_R16G16B16A16_FLOAT);
        back_ = createTarget(
            dyeWidth,
            dyeHeight,
            DXGI_FORMAT_R16G16B16A16_FLOAT);
        std::uint32_t bloomWidth = std::max(1U, dyeWidth >> 1U);
        std::uint32_t bloomHeight = std::max(1U, dyeHeight >> 1U);
        bloom_.reserve(kBloomLevels);
        for (std::uint32_t level = 0U; level < kBloomLevels; ++level)
        {
            bloom_.push_back(createTarget(
                bloomWidth,
                bloomHeight,
                DXGI_FORMAT_R16G16B16A16_FLOAT));
            bloomWidth = std::max(1U, bloomWidth >> 1U);
            bloomHeight = std::max(1U, bloomHeight >> 1U);
        }
    }
    catch (const std::exception&)
    {
        // A partially built target set is unusable; drop everything so the
        // style degrades instead of rendering with missing textures.
        release();
        renderFailed_ = true;
    }
}

void GlowFluidPipeline::release() noexcept
{
    velocity_.reset();
    dye_.reset();
    pressure_.reset();
    divergence_.reset();
    forward_.reset();
    back_.reset();
    bloom_.clear();
    allocatedWidth_ = 0U;
    allocatedHeight_ = 0U;
    allocatedQuality_ = 0U;
}

bool GlowFluidPipeline::updateVertexConstants(
    const float texelX,
    const float texelY)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(
            vertexConstants_.Get(),
            0,
            D3D11_MAP_WRITE_DISCARD,
            0,
            &mapped)))
    {
        renderFailed_ = true;
        return false;
    }
    const TransformConstants constants{texelX, texelY, 0.0F, 0.0F};
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context_->Unmap(vertexConstants_.Get(), 0);
    return true;
}

void GlowFluidPipeline::drawPass(
    ID3D11PixelShader* shader,
    ID3D11Buffer* constants,
    const std::array<ID3D11ShaderResourceView*, 4>& views,
    const std::array<ID3D11SamplerState*, 2>& samplers,
    const std::size_t samplerCount,
    ID3D11BlendState* blendState,
    const Target& target)
{
    if (renderFailed_)
    {
        return;
    }
    if (constants != nullptr)
    {
        ID3D11Buffer* constantBuffer = constants;
        context_->PSSetConstantBuffers(0, 1, &constantBuffer);
    }
    else
    {
        ID3D11Buffer* noConstantBuffer = nullptr;
        context_->PSSetConstantBuffers(0, 1, &noConstantBuffer);
    }
    context_->OMSetRenderTargets(1, target.rtv.GetAddressOf(), nullptr);
    const D3D11_VIEWPORT viewport{
        0.0F,
        0.0F,
        static_cast<float>(target.width),
        static_cast<float>(target.height),
        0.0F,
        1.0F};
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizer_.Get());
    context_->OMSetBlendState(blendState, nullptr, 0xFFFFFFFFU);
    context_->PSSetShader(shader, nullptr, 0);
    context_->PSSetShaderResources(
        0, static_cast<UINT>(views.size()), views.data());
    context_->PSSetSamplers(
        0, static_cast<UINT>(samplerCount), samplers.data());
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    ID3D11Buffer* vertexConstantBuffer = vertexConstants_.Get();
    context_->VSSetConstantBuffers(0, 1, &vertexConstantBuffer);
    context_->IASetInputLayout(inputLayout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const std::array<ID3D11Buffer*, 1> triangleBuffers{fullscreenTriangle_.Get()};
    const std::array<UINT, 1> strides{sizeof(float) * 2U};
    const std::array<UINT, 1> offsets{0U};
    context_->IASetVertexBuffers(
        0, 1, triangleBuffers.data(), strides.data(), offsets.data());
    context_->Draw(3, 0);
    // Drop SRV references so a later pass may bind the same texture as RTV.
    const std::array<ID3D11ShaderResourceView*, 4> noViews{};
    context_->PSSetShaderResources(0, 4, noViews.data());
}

void GlowFluidPipeline::drawPassToRtv(
    ID3D11PixelShader* shader,
    ID3D11Buffer* constants,
    const std::array<ID3D11ShaderResourceView*, 4>& views,
    const std::array<ID3D11SamplerState*, 2>& samplers,
    ID3D11RenderTargetView* destination,
    const float width,
    const float height)
{
    if (renderFailed_ || destination == nullptr)
    {
        return;
    }
    if (constants != nullptr)
    {
        ID3D11Buffer* constantBuffer = constants;
        context_->PSSetConstantBuffers(0, 1, &constantBuffer);
    }
    else
    {
        ID3D11Buffer* noConstantBuffer = nullptr;
        context_->PSSetConstantBuffers(0, 1, &noConstantBuffer);
    }
    context_->OMSetRenderTargets(1, &destination, nullptr);
    const D3D11_VIEWPORT viewport{
        0.0F, 0.0F, width, height, 0.0F, 1.0F};
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizer_.Get());
    context_->OMSetBlendState(blendDisabled_.Get(), nullptr, 0xFFFFFFFFU);
    context_->PSSetShader(shader, nullptr, 0);
    context_->PSSetShaderResources(
        0, static_cast<UINT>(views.size()), views.data());
    context_->PSSetSamplers(0, 1, samplers.data());
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    ID3D11Buffer* vertexConstantBuffer = vertexConstants_.Get();
    context_->VSSetConstantBuffers(0, 1, &vertexConstantBuffer);
    context_->IASetInputLayout(inputLayout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const std::array<ID3D11Buffer*, 1> triangleBuffers{fullscreenTriangle_.Get()};
    const std::array<UINT, 1> strides{sizeof(float) * 2U};
    const std::array<UINT, 1> offsets{0U};
    context_->IASetVertexBuffers(
        0, 1, triangleBuffers.data(), strides.data(), offsets.data());
    context_->Draw(3, 0);
    const std::array<ID3D11ShaderResourceView*, 4> noViews{};
    context_->PSSetShaderResources(0, 4, noViews.data());
}

void GlowFluidPipeline::splat(
    ID3D11PixelShader* shader,
    ID3D11Buffer* constants,
    ID3D11ShaderResourceView* source,
    ID3D11SamplerState* sampler,
    const TargetPair& pair,
    const SplatConstants& data)
{
    if (renderFailed_)
    {
        return;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(
            constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        renderFailed_ = true;
        return;
    }
    std::memcpy(mapped.pData, &data, sizeof(data));
    context_->Unmap(constants, 0);

    if (!updateVertexConstants(pair.write.texelX, pair.write.texelY))
    {
        return;
    }
    const std::array<ID3D11ShaderResourceView*, 4> views{
        source, nullptr, nullptr, nullptr};
    const std::array<ID3D11SamplerState*, 2> samplers{sampler, nullptr};
    drawPass(shader, constants, views, samplers, 1U, blendDisabled_.Get(), pair.write);
}

void GlowFluidPipeline::splatVelocity(
    const float ax,
    const float ay,
    const float bx,
    const float by,
    const float radiusPx,
    const GlowSplatValue value,
    const GlowSplatMode mode,
    const float strength) noexcept
{
    if (resourcesFailed_ || renderFailed_)
    {
        return;
    }
    allocate();
    if (velocity_ == nullptr)
    {
        return;
    }

    SplatConstants constants{};
    constants.aspect = viewportWidth_ / viewportHeight_;
    constants.aX = ax / viewportWidth_;
    constants.aY = ay / viewportHeight_;
    constants.bX = bx / viewportWidth_;
    constants.bY = by / viewportHeight_;
    constants.radius = radiusPx / viewportHeight_;
    constants.wide = constants.radius;
    // Screen pixels per second become grid cells per second so the advect
    // pass moves the field at pointer speed in uv space.
    constants.valueX = value.x * static_cast<float>(velocity_->read.width)
        / viewportWidth_;
    constants.valueY = value.y * static_cast<float>(velocity_->read.height)
        / viewportHeight_;
    constants.mode = modeAsFloat(mode);
    constants.strength = strength;

    splat(
        splatShader_.Get(),
        splatConstants_.Get(),
        velocity_->read.srv.Get(),
        linearSampler_.Get(),
        *velocity_,
        constants);
    velocity_->swap();
}

void GlowFluidPipeline::splatDye(
    const float ax,
    const float ay,
    const float bx,
    const float by,
    const float radiusPx,
    const float wideRadiusPx,
    const GlowSplatValue value,
    const GlowSplatMode mode,
    const float strength) noexcept
{
    if (resourcesFailed_ || renderFailed_)
    {
        return;
    }
    allocate();
    if (dye_ == nullptr)
    {
        return;
    }

    SplatConstants constants{};
    constants.aspect = viewportWidth_ / viewportHeight_;
    constants.aX = ax / viewportWidth_;
    constants.aY = ay / viewportHeight_;
    constants.bX = bx / viewportWidth_;
    constants.bY = by / viewportHeight_;
    constants.radius = radiusPx / viewportHeight_;
    constants.wide = wideRadiusPx / viewportHeight_;
    constants.valueX = value.x;
    constants.valueY = value.y;
    constants.valueZ = value.z;
    constants.mode = modeAsFloat(mode);
    constants.strength = strength;

    splat(
        splatShader_.Get(),
        splatConstants_.Get(),
        dye_->read.srv.Get(),
        linearSampler_.Get(),
        *dye_,
        constants);
    dye_->swap();
}

void GlowFluidPipeline::advect(
    ID3D11RenderTargetView* destination,
    const Target& viewportTarget,
    ID3D11ShaderResourceView* velocityView,
    ID3D11ShaderResourceView* sourceView,
    const float dt,
    const float decayX,
    const float decayY,
    const float decayZ,
    const float decayW)
{
    if (renderFailed_)
    {
        return;
    }
    if (!updateVertexConstants(viewportTarget.texelX, viewportTarget.texelY))
    {
        return;
    }
    AdvectConstants constants{};
    constants.velocityTexelX = velocity_->read.texelX;
    constants.velocityTexelY = velocity_->read.texelY;
    constants.dt = dt;
    constants.decayX = decayX;
    constants.decayY = decayY;
    constants.decayZ = decayZ;
    constants.decayW = decayW;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(
            advectConstants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        renderFailed_ = true;
        return;
    }
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context_->Unmap(advectConstants_.Get(), 0);

    const std::array<ID3D11ShaderResourceView*, 4> views{
        velocityView, sourceView, nullptr, nullptr};
    const std::array<ID3D11SamplerState*, 2> samplers{
        linearSampler_.Get(), nullptr};
    if (destination == nullptr)
    {
        drawPass(
            advectShader_.Get(),
            advectConstants_.Get(),
            views,
            samplers,
            1U,
            blendDisabled_.Get(),
            viewportTarget);
    }
    else
    {
        drawPassToRtv(
            advectShader_.Get(),
            advectConstants_.Get(),
            views,
            samplers,
            destination,
            static_cast<float>(viewportTarget.width),
            static_cast<float>(viewportTarget.height));
    }
}

void GlowFluidPipeline::step(const float dtSeconds) noexcept
{
    if (resourcesFailed_ || renderFailed_)
    {
        return;
    }
    allocate();
    if (velocity_ == nullptr || dye_ == nullptr || pressure_ == nullptr
        || divergence_ == nullptr || forward_ == nullptr || back_ == nullptr
        || bloom_.size() != kBloomLevels)
    {
        return;
    }
    timeSeconds_ += dtSeconds;

    const std::array<ID3D11ShaderResourceView*, 4> noViews{};
    const std::array<ID3D11SamplerState*, 2> linearSamplers{
        linearSampler_.Get(), nullptr};
    const std::array<ID3D11SamplerState*, 2> pointSamplers{
        pointSampler_.Get(), nullptr};

    // Divergence of the current velocity field.
    if (!updateVertexConstants(divergence_->texelX, divergence_->texelY))
    {
        return;
    }
    {
        const std::array<ID3D11ShaderResourceView*, 4> views{
            velocity_->read.srv.Get(), nullptr, nullptr, nullptr};
        drawPass(
            divergenceShader_.Get(),
            nullptr,
            views,
            linearSamplers,
            1U,
            blendDisabled_.Get(),
            *divergence_);
    }

    // Pressure solve: damp the previous solution, then Jacobi iterations.
    if (!updateVertexConstants(pressure_->write.texelX, pressure_->write.texelY))
    {
        return;
    }
    {
        ScaleConstants constants{};
        constants.value = 0.8F;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(
                scaleConstants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            renderFailed_ = true;
            return;
        }
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context_->Unmap(scaleConstants_.Get(), 0);
        const std::array<ID3D11ShaderResourceView*, 4> views{
            pressure_->read.srv.Get(), nullptr, nullptr, nullptr};
        drawPass(
            scaleShader_.Get(),
            scaleConstants_.Get(),
            views,
            pointSamplers,
            1U,
            blendDisabled_.Get(),
            pressure_->write);
        pressure_->swap();
    }
    for (std::uint32_t iteration = 0U; iteration < kPressureSteps; ++iteration)
    {
        if (renderFailed_)
        {
            return;
        }
        if (!updateVertexConstants(
                pressure_->write.texelX, pressure_->write.texelY))
        {
            return;
        }
        const std::array<ID3D11ShaderResourceView*, 4> views{
            pressure_->read.srv.Get(),
            divergence_->srv.Get(),
            nullptr,
            nullptr};
        drawPass(
            pressureShader_.Get(),
            nullptr,
            views,
            pointSamplers,
            1U,
            blendDisabled_.Get(),
            pressure_->write);
        pressure_->swap();
    }

    // Project velocity onto its divergence-free part.
    if (!updateVertexConstants(velocity_->write.texelX, velocity_->write.texelY))
    {
        return;
    }
    {
        const std::array<ID3D11ShaderResourceView*, 4> views{
            pressure_->read.srv.Get(),
            velocity_->read.srv.Get(),
            nullptr,
            nullptr};
        const std::array<ID3D11SamplerState*, 2> samplers{
            pointSampler_.Get(), linearSampler_.Get()};
        drawPass(
            gradientShader_.Get(),
            nullptr,
            views,
            samplers,
            2U,
            blendDisabled_.Get(),
            velocity_->write);
        velocity_->swap();
    }

    // Velocity self-advection with mild damping.
    const float calm = std::exp(-dtSeconds * kVelocityDamping);
    advect(
        nullptr,
        velocity_->write,
        velocity_->read.srv.Get(),
        velocity_->read.srv.Get(),
        dtSeconds,
        calm,
        calm,
        1.0F,
        1.0F);
    velocity_->swap();

    // Dye advection: forward, backward, then the clamped MacCormack blend.
    if (renderFailed_)
    {
        return;
    }
    advect(
        nullptr,
        *forward_,
        velocity_->read.srv.Get(),
        dye_->read.srv.Get(),
        dtSeconds,
        1.0F,
        1.0F,
        1.0F,
        1.0F);
    if (renderFailed_)
    {
        return;
    }
    advect(
        nullptr,
        *back_,
        velocity_->read.srv.Get(),
        forward_->srv.Get(),
        -dtSeconds,
        1.0F,
        1.0F,
        1.0F,
        1.0F);
    if (renderFailed_)
    {
        return;
    }

    const float linger = std::max(linger_, 0.1F);
    MaccormackConstants constants{};
    constants.velocityTexelX = velocity_->read.texelX;
    constants.velocityTexelY = velocity_->read.texelY;
    constants.sourceTexelX = dye_->read.texelX;
    constants.sourceTexelY = dye_->read.texelY;
    constants.dt = dtSeconds;
    constants.decayX = std::exp(-dtSeconds * kDyeDecayRate / (linger * 0.8F));
    constants.decayY = std::exp(-dtSeconds * kDyeDecayRate / (linger * 0.2F));
    constants.decayZ = std::exp(-dtSeconds * kDyeDecayRate / (linger * 1.2F));
    constants.decayW = 1.0F;
    if (!updateVertexConstants(dye_->write.texelX, dye_->write.texelY))
    {
        return;
    }
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(
                maccormackConstants_.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped)))
        {
            renderFailed_ = true;
            return;
        }
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context_->Unmap(maccormackConstants_.Get(), 0);
    }
    {
        const std::array<ID3D11ShaderResourceView*, 4> views{
            velocity_->read.srv.Get(),
            dye_->read.srv.Get(),
            forward_->srv.Get(),
            back_->srv.Get()};
        const std::array<ID3D11SamplerState*, 2> samplers{
            linearSampler_.Get(), nullptr};
        drawPass(
            maccormackShader_.Get(),
            maccormackConstants_.Get(),
            views,
            samplers,
            1U,
            blendDisabled_.Get(),
            dye_->write);
        dye_->swap();
    }
}

void GlowFluidPipeline::display(ID3D11RenderTargetView* destination) noexcept
{
    if (resourcesFailed_ || renderFailed_ || destination == nullptr)
    {
        return;
    }
    allocate();
    if (dye_ == nullptr || bloom_.size() != kBloomLevels)
    {
        return;
    }

    const std::array<ID3D11ShaderResourceView*, 4> noViews{};
    const std::array<ID3D11SamplerState*, 2> linearSamplers{
        linearSampler_.Get(), nullptr};

    if (appearance_.glow > 0.0F)
    {
        // Bloom pyramid: shaded prefilter, five downsample levels, then an
        // additive upsample chain back to full dye resolution.
        if (!updateVertexConstants(
                bloom_.front()->texelX, bloom_.front()->texelY))
        {
            return;
        }
        PrefilterConstants prefilter{};
        prefilter.dyeTexelX = dye_->read.texelX;
        prefilter.dyeTexelY = dye_->read.texelY;
        prefilter.colorR = appearance_.colorR;
        prefilter.colorG = appearance_.colorG;
        prefilter.colorB = appearance_.colorB;
        prefilter.secondaryR = appearance_.secondaryR;
        prefilter.secondaryG = appearance_.secondaryG;
        prefilter.secondaryB = appearance_.secondaryB;
        prefilter.intensity = appearance_.intensity;
        prefilter.hotspot = appearance_.hotspot;
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context_->Map(
                    prefilterConstants_.Get(),
                    0,
                    D3D11_MAP_WRITE_DISCARD,
                    0,
                    &mapped)))
            {
                renderFailed_ = true;
                return;
            }
            std::memcpy(mapped.pData, &prefilter, sizeof(prefilter));
            context_->Unmap(prefilterConstants_.Get(), 0);
        }
        {
            const std::array<ID3D11ShaderResourceView*, 4> views{
                dye_->read.srv.Get(), nullptr, nullptr, nullptr};
            drawPass(
                prefilterShader_.Get(),
                prefilterConstants_.Get(),
                views,
                linearSamplers,
                1U,
                blendDisabled_.Get(),
                *bloom_.front());
        }
        for (std::uint32_t level = 1U; level < kBloomLevels; ++level)
        {
            if (renderFailed_)
            {
                return;
            }
            if (!updateVertexConstants(
                    bloom_[level]->texelX, bloom_[level]->texelY))
            {
                return;
            }
            TexelConstants texel{};
            texel.texelX = bloom_[level - 1U]->texelX;
            texel.texelY = bloom_[level - 1U]->texelY;
            {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (FAILED(context_->Map(
                        downConstants_.Get(),
                        0,
                        D3D11_MAP_WRITE_DISCARD,
                        0,
                        &mapped)))
                {
                    renderFailed_ = true;
                    return;
                }
                std::memcpy(mapped.pData, &texel, sizeof(texel));
                context_->Unmap(downConstants_.Get(), 0);
            }
            const std::array<ID3D11ShaderResourceView*, 4> views{
                bloom_[level - 1U]->srv.Get(), nullptr, nullptr, nullptr};
            drawPass(
                downShader_.Get(),
                downConstants_.Get(),
                views,
                linearSamplers,
                1U,
                blendDisabled_.Get(),
                *bloom_[level]);
        }
        for (std::uint32_t level = kBloomLevels; level-- > 1U;)
        {
            if (renderFailed_)
            {
                return;
            }
            if (!updateVertexConstants(
                    bloom_[level - 1U]->texelX, bloom_[level - 1U]->texelY))
            {
                return;
            }
            TexelConstants texel{};
            texel.texelX = bloom_[level]->texelX;
            texel.texelY = bloom_[level]->texelY;
            {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (FAILED(context_->Map(
                        upConstants_.Get(),
                        0,
                        D3D11_MAP_WRITE_DISCARD,
                        0,
                        &mapped)))
                {
                    renderFailed_ = true;
                    return;
                }
                std::memcpy(mapped.pData, &texel, sizeof(texel));
                context_->Unmap(upConstants_.Get(), 0);
            }
            const std::array<ID3D11ShaderResourceView*, 4> views{
                bloom_[level]->srv.Get(), nullptr, nullptr, nullptr};
            drawPass(
                upShader_.Get(),
                upConstants_.Get(),
                views,
                linearSamplers,
                1U,
                blendAdditive_.Get(),
                *bloom_[level - 1U]);
        }
    }

    DisplayConstants constants{};
    constants.colorR = appearance_.colorR;
    constants.colorG = appearance_.colorG;
    constants.colorB = appearance_.colorB;
    constants.secondaryR = appearance_.secondaryR;
    constants.secondaryG = appearance_.secondaryG;
    constants.secondaryB = appearance_.secondaryB;
    constants.intensity = appearance_.intensity;
    constants.hotspot = appearance_.hotspot;
    constants.glow = appearance_.glow > 0.0F
        ? (appearance_.glow * 1.4F) / static_cast<float>(kBloomLevels)
        : 0.0F;
    constants.grain = appearance_.grain;
    constants.time = timeSeconds_;
    constants.light = 0.0F;
    if (!updateVertexConstants(
            1.0F / viewportWidth_, 1.0F / viewportHeight_))
    {
        return;
    }
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(
                displayConstants_.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped)))
        {
            renderFailed_ = true;
            return;
        }
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context_->Unmap(displayConstants_.Get(), 0);
    }
    const std::array<ID3D11ShaderResourceView*, 4> views{
        dye_->read.srv.Get(), bloom_.front()->srv.Get(), nullptr, nullptr};
    drawPassToRtv(
        displayShader_.Get(),
        displayConstants_.Get(),
        views,
        linearSamplers,
        destination,
        viewportWidth_,
        viewportHeight_);
}

void GlowFluidPipeline::clearSimulation() noexcept
{
    if (velocity_ == nullptr || dye_ == nullptr || pressure_ == nullptr)
    {
        return;
    }
    const std::array<float, 4> transparent{0.0F, 0.0F, 0.0F, 0.0F};
    const auto clearPair = [this, &transparent](const TargetPair& pair)
    {
        context_->ClearRenderTargetView(pair.read.rtv.Get(), transparent.data());
        context_->ClearRenderTargetView(
            pair.write.rtv.Get(), transparent.data());
    };
    clearPair(*velocity_);
    clearPair(*dye_);
    clearPair(*pressure_);
}

void GlowFluidPipeline::reset() noexcept
{
    clearSimulation();
    timeSeconds_ = 0.0F;
}

bool GlowFluidPipeline::healthy() const noexcept
{
    return !resourcesFailed_ && !renderFailed_;
}

bool GlowFluidPipeline::allocated() const noexcept
{
    return velocity_ != nullptr && dye_ != nullptr;
}

std::uint32_t GlowFluidPipeline::simulationWidth() const noexcept
{
    return velocity_ != nullptr ? velocity_->read.width : 0U;
}

std::uint32_t GlowFluidPipeline::simulationHeight() const noexcept
{
    return velocity_ != nullptr ? velocity_->read.height : 0U;
}

std::uint32_t GlowFluidPipeline::dyeWidth() const noexcept
{
    return dye_ != nullptr ? dye_->read.width : 0U;
}

std::uint32_t GlowFluidPipeline::dyeHeight() const noexcept
{
    return dye_ != nullptr ? dye_->read.height : 0U;
}

std::uint32_t GlowFluidPipeline::bloomLevelCount() const noexcept
{
    return static_cast<std::uint32_t>(bloom_.size());
}

std::uint32_t GlowFluidPipeline::bloomWidth(
    const std::uint32_t level) const noexcept
{
    return level < bloom_.size() ? bloom_[level]->width : 0U;
}

ID3D11Texture2D* GlowFluidPipeline::velocityReadTexture() const noexcept
{
    return velocity_ != nullptr ? velocity_->read.texture.Get() : nullptr;
}

ID3D11Texture2D* GlowFluidPipeline::dyeReadTexture() const noexcept
{
    return dye_ != nullptr ? dye_->read.texture.Get() : nullptr;
}

} // namespace bafx::windows
