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
// pointer velocity, dye splats inject energy/heat/gas additively.
constexpr float kFlowStrength = 0.45F; // FLOW * 0.6
constexpr float kDyeEnergy = 5.6F;
constexpr float kDyeHeat = 5.6F;
constexpr float kDyeGas = 0.7F;
constexpr float kVelocityRadiusScale = 4.0F;
constexpr float kVelocityRadiusOffset = 18.0F;
constexpr float kDyeWideScale = 5.0F;
constexpr float kDyeWideOffset = 6.0F;
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
            settings.hotspot});
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
    trail_.addSample(x, y, nowSeconds);
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
    trail_.advance(nowSeconds);
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

    injectPendingSamples(deltaSeconds);
    pipeline_->step(deltaSeconds);
    pipeline_->display(destination);
}

void GlowFluidRenderer::injectPendingSamples(const float deltaSeconds)
{
    const std::vector<GlowTrailSample>& samples = trail_.samples();
    const GlowTrailSample* newest = nullptr;
    for (const GlowTrailSample& sample : samples)
    {
        if (sample.sequence > consumedSequence_)
        {
            newest = &sample;
        }
    }
    if (newest == nullptr)
    {
        return;
    }
    consumedSequence_ = newest->sequence;

    float fromX = newest->x;
    float fromY = newest->y;
    if (hasEmittedPosition_)
    {
        fromX = emittedX_;
        fromY = emittedY_;
    }
    emittedX_ = newest->x;
    emittedY_ = newest->y;
    hasEmittedPosition_ = true;

    // Reference lightRadius: never thinner than half the trail width, and at
    // least ~0.9 dye texels so the splat survives downsampling.
    const float dyeHeight = static_cast<float>(pipeline_->dyeHeight());
    const float radius = std::max(
        settings_.radiusPx * 0.5F,
        dyeHeight > 0.0F ? (viewportHeight_ / dyeHeight) * 0.9F : 1.0F);
    const float wideRadius = radius * kDyeWideScale + kDyeWideOffset;
    const float velocityRadius =
        radius * kVelocityRadiusScale + kVelocityRadiusOffset;

    const bool moving = std::hypot(newest->x - fromX, newest->y - fromY) > 0.05F;
    const GlowSplatMode velocityMode =
        moving ? GlowSplatMode::SegmentMix : GlowSplatMode::PointMix;
    const GlowSplatMode dyeMode =
        moving ? GlowSplatMode::SegmentAdditive : GlowSplatMode::PointAdditive;

    if (moving)
    {
        // Pointer velocity in screen pixels per second; the pipeline converts
        // it to simulation grid units.
        pipeline_->splatVelocity(
            fromX,
            fromY,
            newest->x,
            newest->y,
            velocityRadius,
            GlowSplatValue{
                (newest->x - fromX) / deltaSeconds,
                (newest->y - fromY) / deltaSeconds,
                0.0F},
            velocityMode,
            kFlowStrength);
    }
    pipeline_->splatDye(
        fromX,
        fromY,
        newest->x,
        newest->y,
        radius,
        wideRadius,
        GlowSplatValue{kDyeEnergy, kDyeHeat, kDyeGas},
        dyeMode,
        1.0F);
}

void GlowFluidRenderer::renderPlaceholder(
    ID3D11RenderTargetView* destination,
    const double nowSeconds)
{

    trail_.advance(nowSeconds);
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
    consumedSequence_ = 0U;
    hasEmittedPosition_ = false;
    emittedX_ = 0.0F;
    emittedY_ = 0.0F;
}

}
