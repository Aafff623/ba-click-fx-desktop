#include "bafx/windows/glow_fluid_renderer.hpp"

#include "bafx/windows/error.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

namespace bafx::windows
{

namespace
{

constexpr std::size_t verticesPerSample = 6U;
constexpr std::size_t maximumSamples = 1024U;

// Reference emit() constants: velocity splats blend toward the measured
// emitter velocity, dye splats inject energy/heat/gas additively.
constexpr float kFlowStrength = 0.45F; // FLOW * 0.6
constexpr float kDyeEnergy = 5.6F;
constexpr float kDyeHeat = 5.6F;
constexpr float kDyeGas = 0.7F;
constexpr float kVelocityRadiusScale = 4.0F;
constexpr float kVelocityRadiusOffset = 18.0F;
constexpr float kDyeWideScale = 5.0F;
constexpr float kDyeWideOffset = 6.0F;
constexpr float kMinimumMotion = 0.05F;
constexpr float kSpringBaseOmega = 10.0F;
constexpr float kSpringFollowScale = 50.0F;
constexpr float kMaximumSpringStep = 0.004F;
constexpr float kRestingSpeed = 1.0F;

// Reference burst() constants: six radial jets plus a central dye puff.
constexpr std::uint32_t kBurstJets = 6U;
constexpr float kBurstJetInset = 3.0F;
constexpr float kBurstReachBase = 20.0F;
constexpr float kBurstReachSpan = 14.0F;
constexpr float kBurstVelocitySpeed = 760.0F;
constexpr float kBurstVelocityOffset = 0.6F;
constexpr float kBurstVelocityStrength = 0.9F;
constexpr float kBurstVelocityRadius = 18.0F;
constexpr float kBurstDyeEnergy = 2.4F;
constexpr float kBurstDyeHeat = 2.4F;
constexpr float kBurstDyeGas = 0.3F;
constexpr float kBurstCenterEnergy = 1.8F;
constexpr float kBurstCenterHeat = 1.8F;
constexpr float kBurstCenterGas = 0.4F;
constexpr float kBurstCenterRadiusScale = 2.2F;
constexpr float kBurstCenterWideScale = 7.0F;
constexpr float kBurstCenterWideOffset = 10.0F;

constexpr float kMinimumFrameDelta = 1.0F / 240.0F;
constexpr float kMaximumFrameDelta = 1.0F / 30.0F;

constexpr char kVertexShaderSource[] = R"hlsl(
cbuffer Transform : register(b0)
{
    float2 uViewportSize;
    float2 uPadding;
}

struct VSInput
{
    float2 position : POSITION;
    float alpha : ALPHA;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float alpha : ALPHA;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    // Input pixels are window-local with y down; NDC is y up.
    float2 ndc = (input.position / uViewportSize) * 2.0 - 1.0;
    output.position = float4(ndc.x, -ndc.y, 0.0, 1.0);
    output.alpha = input.alpha;
    return output;
}
)hlsl";

constexpr char kPixelShaderSource[] = R"hlsl(
cbuffer Shade : register(b0)
{
    float3 uColor;
    float uIntensity;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float alpha : ALPHA;
};

float4 main(PSInput input) : SV_TARGET
{
    // Premultiplied additive contribution; the blend state adds it into the
    // FP16 swap-chain surface exactly like the authored FX path.
    float3 rgb = uColor * (uIntensity * input.alpha);
    return float4(rgb, input.alpha);
}
)hlsl";

[[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> compileShader(
    const std::string_view source,
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
        "glow_fluid_renderer.cpp",
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

}

GlowFluidRenderer::GlowFluidRenderer(
    ID3D11Device* device,
    ID3D11DeviceContext* context)
    : device_{device}
    , context_{context}
{
    if (device == nullptr || context == nullptr)
    {
        throw std::invalid_argument("GlowFluidRenderer requires a device and context");
    }
    resourcesFailed_ = !createResources();
    if (!resourcesFailed_)
    {
        // The fluid pipeline rides the same device and resource domain. A
        // creation failure is not fatal: the additive-quad placeholder below
        // stays available as the degraded visual path.
        try
        {
            pipeline_ = std::make_unique<GlowFluidPipeline>(device, context);
        }
        catch (const std::exception&)
        {
            pipeline_.reset();
        }
    }
}

void GlowFluidRenderer::setSettings(const GlowRenderSettings& settings) noexcept
{
    settings_ = settings;
    trail_.setLinger(settings.linger);
    trail_.setRadius(settings.radiusPx);
    if (pipeline_ != nullptr)
    {
        pipeline_->setQuality(settings.quality);
        pipeline_->setLinger(settings.linger);
        pipeline_->setAppearance(GlowFluidAppearance{
            settings.colorR,
            settings.colorG,
            settings.colorB,
            settings.secondaryR,
            settings.secondaryG,
            settings.secondaryB,
            settings.intensity,
            settings.glow,
            settings.grain,
            settings.hotspot,
            2.0F});
    }
}

void GlowFluidRenderer::setViewportSize(
    const float widthPx,
    const float heightPx) noexcept
{
    viewportWidth_ = std::max(widthPx, 1.0F);
    viewportHeight_ = std::max(heightPx, 1.0F);
    if (pipeline_ != nullptr)
    {
        pipeline_->setViewportSize(viewportWidth_, viewportHeight_);
    }
}

void GlowFluidRenderer::submitPointerSample(
    const float x,
    const float y,
    const double nowSeconds) noexcept
{
    if (!pointerInside_)
    {
        // First sample after idle snaps the emitter to the pointer so no
        // streak reaches in from the previous session's last position.
        pointerX_ = x;
        pointerY_ = y;
        emitterX_ = x;
        emitterY_ = y;
        emitterVX_ = 0.0F;
        emitterVY_ = 0.0F;
        pointerInside_ = true;
    }
    else
    {
        pointerX_ = x;
        pointerY_ = y;
    }
    trail_.addSample(x, y, nowSeconds);
}

void GlowFluidRenderer::submitPointerClick(const float x, const float y) noexcept
{
    if (!settings_.clickBurst)
    {
        return;
    }
    pendingBursts_.push_back(PendingBurst{x, y});
}

bool GlowFluidRenderer::createResources()
{
    try
    {
        const Microsoft::WRL::ComPtr<ID3DBlob> vertexByteCode =
            compileShader(kVertexShaderSource, "main", "vs_5_0");
        const Microsoft::WRL::ComPtr<ID3DBlob> pixelByteCode =
            compileShader(kPixelShaderSource, "main", "ps_5_0");

        throwIfFailed(
            device_->CreateVertexShader(
                vertexByteCode->GetBufferPointer(),
                vertexByteCode->GetBufferSize(),
                nullptr,
                &vertexShader_),
            "CreateVertexShader(glow)");
        throwIfFailed(
            device_->CreatePixelShader(
                pixelByteCode->GetBufferPointer(),
                pixelByteCode->GetBufferSize(),
                nullptr,
                &pixelShader_),
            "CreatePixelShader(glow)");

        const std::array<D3D11_INPUT_ELEMENT_DESC, 2U> layout{
            D3D11_INPUT_ELEMENT_DESC{
                "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                D3D11_INPUT_PER_VERTEX_DATA, 0},
            D3D11_INPUT_ELEMENT_DESC{
                "ALPHA", 0, DXGI_FORMAT_R32_FLOAT, 0, 8,
                D3D11_INPUT_PER_VERTEX_DATA, 0}};
        throwIfFailed(
            device_->CreateInputLayout(
                layout.data(),
                static_cast<UINT>(layout.size()),
                vertexByteCode->GetBufferPointer(),
                vertexByteCode->GetBufferSize(),
                &inputLayout_),
            "CreateInputLayout(glow)");

        vertexCapacity_ = maximumSamples * verticesPerSample;
        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.ByteWidth = static_cast<UINT>(
            vertexCapacity_ * sizeof(Vertex));
        bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        throwIfFailed(
            device_->CreateBuffer(&bufferDesc, nullptr, &vertexBuffer_),
            "CreateBuffer(glow vertices)");

        D3D11_BUFFER_DESC transformDesc{};
        transformDesc.ByteWidth = 16U; // float2 size + float2 padding
        transformDesc.Usage = D3D11_USAGE_DYNAMIC;
        transformDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        transformDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        throwIfFailed(
            device_->CreateBuffer(&transformDesc, nullptr, &transformBuffer_),
            "CreateBuffer(glow transform)");

        D3D11_BUFFER_DESC shadeDesc{};
        shadeDesc.ByteWidth = 16U; // float3 color + float intensity
        shadeDesc.Usage = D3D11_USAGE_DYNAMIC;
        shadeDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        shadeDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        throwIfFailed(
            device_->CreateBuffer(&shadeDesc, nullptr, &shadeBuffer_),
            "CreateBuffer(glow shade)");

        D3D11_BLEND_DESC blendDesc{};
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;
        throwIfFailed(
            device_->CreateBlendState(&blendDesc, &blendState_),
            "CreateBlendState(glow)");
        return true;
    }
    catch (const std::exception&)
    {
        // Resource creation failures degrade to a disabled style; the Host
        // keeps running the BA path and reports the style as inactive.
        return false;
    }
}

void GlowFluidRenderer::appendSampleVertices(
    const GlowTrailSample& sample,
    const double nowSeconds,
    std::vector<Vertex>& vertices) const noexcept
{
    const float weight = GlowTrailState::fadeWeight(
        sample,
        nowSeconds,
        trail_.linger());
    if (weight <= 0.0F)
    {
        return;
    }
    const float radius = trail_.radius();
    const std::array<std::array<float, 2>, 6U> corners{{
        {{-radius, -radius}},
        {{radius, -radius}},
        {{-radius, radius}},
        {{-radius, radius}},
        {{radius, -radius}},
        {{radius, radius}}}};
    for (const auto& corner : corners)
    {
        vertices.push_back(
            Vertex{
                sample.x + corner[0],
                sample.y + corner[1],
                weight});
    }
}

void GlowFluidRenderer::render(
    ID3D11RenderTargetView* destination,
    const double nowSeconds)
{
    if (resourcesFailed_ || destination == nullptr)
    {
        return;
    }
    if (pipeline_ != nullptr && pipeline_->healthy())
    {
        renderFluid(destination, nowSeconds);
        return;
    }
    renderPlaceholder(destination, nowSeconds);
}

void GlowFluidRenderer::renderFluid(
    ID3D11RenderTargetView* destination,
    const double nowSeconds)
{
    // The reference sleeps only when the pointer has been quiet for the
    // linger-scaled window AND the emitter has come to rest.
    trail_.advance(nowSeconds, emitterResting());
    if (trail_.samples().empty())
    {
        if (trail_.pendingTransparentFrame())
        {
            // Present exactly one transparent frame so the overlay does not
            // keep the faded glow on screen after idle.
            const std::array<float, 4> transparent{0.0F, 0.0F, 0.0F, 0.0F};
            context_->ClearRenderTargetView(destination, transparent.data());
            trail_.clearPendingTransparentFrame();
        }
        // The trail is gone and the dye has faded with it: drop the field so
        // the next pointer entry starts from a clean simulation.
        pipeline_->clearSimulation();
        lastFrameSeconds_ = 0.0;
        pointerInside_ = false;
        return;
    }

    float deltaSeconds = 1.0F / 60.0F;
    if (lastFrameSeconds_ > 0.0)
    {
        deltaSeconds = std::clamp(
            static_cast<float>(nowSeconds - lastFrameSeconds_),
            kMinimumFrameDelta,
            kMaximumFrameDelta);
    }
    lastFrameSeconds_ = nowSeconds;

    advanceEmitter(deltaSeconds);
    emitFrame(deltaSeconds);
    processBursts();
    pipeline_->step(deltaSeconds);
    pipeline_->display(destination);
}

void GlowFluidRenderer::advanceEmitter(const float dt) noexcept
{
    emitterPreviousX_ = emitterX_;
    emitterPreviousY_ = emitterY_;
    if (settings_.followSpeed >= 1.0F)
    {
        // followSpeed 1 means the emitter position is the pointer position;
        // the simulation field still drags naturally.
        emitterX_ = pointerX_;
        emitterY_ = pointerY_;
        // The snap path has no spring velocity; the frame delta keeps the
        // resting check honest while the pointer moves.
        emitterVX_ = (emitterX_ - emitterPreviousX_) / dt;
        emitterVY_ = (emitterY_ - emitterPreviousY_) / dt;
        return;
    }
    const float omega =
        kSpringBaseOmega + settings_.followSpeed * kSpringFollowScale;
    const int steps =
        std::max(1, static_cast<int>(std::ceil(dt / kMaximumSpringStep)));
    const float h = dt / static_cast<float>(steps);
    for (int index = 0; index < steps; ++index)
    {
        emitterVX_ += (omega * omega * (pointerX_ - emitterX_)
                       - 2.0F * omega * emitterVX_)
            * h;
        emitterVY_ += (omega * omega * (pointerY_ - emitterY_)
                       - 2.0F * omega * emitterVY_)
            * h;
        emitterX_ += emitterVX_ * h;
        emitterY_ += emitterVY_ * h;
    }
}

void GlowFluidRenderer::emitFrame(const float dt) noexcept
{
    if (pipeline_ == nullptr)
    {
        return;
    }
    const float dx = emitterX_ - emitterPreviousX_;
    const float dy = emitterY_ - emitterPreviousY_;
    if (std::hypot(dx, dy) < kMinimumMotion)
    {
        return;
    }

    const float radius = lightRadius();
    const float wideRadius = radius * kDyeWideScale + kDyeWideOffset;
    const float velocityRadius =
        radius * kVelocityRadiusScale + kVelocityRadiusOffset;

    // Emitter velocity in screen pixels per second; the pipeline converts it
    // to simulation grid units.
    pipeline_->splatVelocity(
        emitterPreviousX_,
        emitterPreviousY_,
        emitterX_,
        emitterY_,
        velocityRadius,
        GlowSplatValue{dx / dt, dy / dt, 0.0F},
        GlowSplatMode::SegmentMix,
        kFlowStrength);
    pipeline_->splatDye(
        emitterPreviousX_,
        emitterPreviousY_,
        emitterX_,
        emitterY_,
        radius,
        wideRadius,
        GlowSplatValue{kDyeEnergy, kDyeHeat, kDyeGas},
        GlowSplatMode::SegmentAdditive,
        1.0F);
}

void GlowFluidRenderer::processBursts() noexcept
{
    if (pendingBursts_.empty() || pipeline_ == nullptr)
    {
        return;
    }
    const float radius = lightRadius();
    const float wideRadius = radius * kDyeWideScale + kDyeWideOffset;
    for (const PendingBurst& burst : pendingBursts_)
    {
        const float base = nextRandom() * 2.0F * 3.14159265F;
        for (std::uint32_t jet = 0U; jet < kBurstJets; ++jet)
        {
            const float angle = base
                + static_cast<float>(jet) / static_cast<float>(kBurstJets)
                      * 2.0F * 3.14159265F
                + (nextRandom() - 0.5F) * 0.5F;
            const float reach =
                kBurstReachBase + nextRandom() * kBurstReachSpan;
            const float cosAngle = std::cos(angle);
            const float sinAngle = std::sin(angle);
            const float ax = burst.x + cosAngle * kBurstJetInset;
            const float ay = burst.y + sinAngle * kBurstJetInset;
            const float bx = burst.x + cosAngle * reach;
            const float by = burst.y + sinAngle * reach;
            pipeline_->splatVelocity(
                ax,
                ay,
                bx,
                by,
                kBurstVelocityRadius,
                GlowSplatValue{
                    std::cos(angle + kBurstVelocityOffset) * kBurstVelocitySpeed,
                    std::sin(angle + kBurstVelocityOffset) * kBurstVelocitySpeed,
                    0.0F},
                GlowSplatMode::PointMix,
                kBurstVelocityStrength);
            pipeline_->splatDye(
                ax,
                ay,
                bx,
                by,
                radius * 1.1F,
                wideRadius,
                GlowSplatValue{
                    kBurstDyeEnergy, kBurstDyeHeat, kBurstDyeGas},
                GlowSplatMode::PointAdditive,
                1.0F);
        }
        pipeline_->splatDye(
            burst.x,
            burst.y,
            burst.x + 0.01F,
            burst.y,
            radius * kBurstCenterRadiusScale,
            radius * kBurstCenterWideScale + kBurstCenterWideOffset,
            GlowSplatValue{
                kBurstCenterEnergy, kBurstCenterHeat, kBurstCenterGas},
            GlowSplatMode::PointAdditive,
            1.0F);
    }
    pendingBursts_.clear();
}

float GlowFluidRenderer::lightRadius() const noexcept
{
    const float dyeHeight = static_cast<float>(pipeline_->dyeHeight());
    return std::max(
        settings_.radiusPx * 0.5F,
        dyeHeight > 0.0F ? (viewportHeight_ / dyeHeight) * 0.9F : 1.0F);
}

bool GlowFluidRenderer::emitterResting() const noexcept
{
    return std::hypot(emitterVX_, emitterVY_) < kRestingSpeed;
}

float GlowFluidRenderer::nextRandom() noexcept
{
    // xorshift32; deterministic so burst geometry is reproducible in tests.
    randomState_ ^= randomState_ << 13;
    randomState_ ^= randomState_ >> 17;
    randomState_ ^= randomState_ << 5;
    return static_cast<float>(randomState_ % 100000U) / 100000.0F;
}

void GlowFluidRenderer::renderPlaceholder(
    ID3D11RenderTargetView* destination,
    const double nowSeconds)
{

    // The degraded path has no emitter; treat it as permanently resting so
    // the shared idle contract still expires the trail.
    trail_.advance(nowSeconds, true);
    const bool pendingTransparent = trail_.pendingTransparentFrame();
    if (trail_.samples().empty() && !pendingTransparent)
    {
        return;
    }

    // The trail just emptied: present exactly one transparent frame so the
    // overlay does not keep the faded glow on screen after idle.
    if (trail_.samples().empty())
    {
        const std::array<float, 4> transparent{0.0F, 0.0F, 0.0F, 0.0F};
        context_->ClearRenderTargetView(destination, transparent.data());
        trail_.clearPendingTransparentFrame();
        return;
    }

    std::vector<Vertex> vertices;
    vertices.reserve(trail_.samples().size() * verticesPerSample);
    for (const auto& sample : trail_.samples())
    {
        appendSampleVertices(sample, nowSeconds, vertices);
    }
    if (vertices.empty())
    {
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(vertexBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        return;
    }
    const auto vertexCount = std::min(vertices.size(), vertexCapacity_);
    std::copy_n(vertices.begin(), vertexCount, static_cast<Vertex*>(mapped.pData));
    context_->Unmap(vertexBuffer_.Get(), 0);

    struct TransformData
    {
        float width;
        float height;
        float padding0;
        float padding1;
    };
    D3D11_MAPPED_SUBRESOURCE transformMapped{};
    if (FAILED(context_->Map(
            transformBuffer_.Get(),
            0,
            D3D11_MAP_WRITE_DISCARD,
            0,
            &transformMapped)))
    {
        return;
    }
    *static_cast<TransformData*>(transformMapped.pData) =
        TransformData{viewportWidth_, viewportHeight_, 0.0F, 0.0F};
    context_->Unmap(transformBuffer_.Get(), 0);

    // Shade constants ride a separate b0 slot for the pixel stage.
    struct ShadeData
    {
        float r;
        float g;
        float b;
        float intensity;
    };
    D3D11_MAPPED_SUBRESOURCE shadeMapped{};
    if (FAILED(context_->Map(
            shadeBuffer_.Get(),
            0,
            D3D11_MAP_WRITE_DISCARD,
            0,
            &shadeMapped)))
    {
        return;
    }
    *static_cast<ShadeData*>(shadeMapped.pData) = ShadeData{
        settings_.colorR,
        settings_.colorG,
        settings_.colorB,
        settings_.intensity};
    context_->Unmap(shadeBuffer_.Get(), 0);

    const std::array<float, 4> transparent{0.0F, 0.0F, 0.0F, 0.0F};
    context_->ClearRenderTargetView(destination, transparent.data());

    const std::array<ID3D11Buffer*, 1> vertexBuffers{vertexBuffer_.Get()};
    const std::array<UINT, 1> strides{sizeof(Vertex)};
    const std::array<UINT, 1> offsets{0U};
    context_->IASetInputLayout(inputLayout_.Get());
    context_->IASetVertexBuffers(
        0, 1, vertexBuffers.data(), strides.data(), offsets.data());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, transformBuffer_.GetAddressOf());
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, shadeBuffer_.GetAddressOf());

    const std::array<float, 4> blendFactor{1.0F, 1.0F, 1.0F, 1.0F};
    context_->OMSetBlendState(
        blendState_.Get(), blendFactor.data(), 0xFFFFFFFFU);
    context_->Draw(static_cast<UINT>(vertexCount), 0);
}

bool GlowFluidRenderer::needsFrame() const noexcept
{
    return trail_.needsFrame();
}

void GlowFluidRenderer::reset() noexcept
{
    trail_.reset();
    if (pipeline_ != nullptr)
    {
        pipeline_->reset();
    }
    lastFrameSeconds_ = 0.0;
    pointerInside_ = false;
    pointerX_ = 0.0F;
    pointerY_ = 0.0F;
    emitterX_ = 0.0F;
    emitterY_ = 0.0F;
    emitterVX_ = 0.0F;
    emitterVY_ = 0.0F;
    emitterPreviousX_ = 0.0F;
    emitterPreviousY_ = 0.0F;
    pendingBursts_.clear();
}

}
