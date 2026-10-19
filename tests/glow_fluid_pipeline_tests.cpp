#include "glow_test_support.hpp"

#include "bafx/windows/glow_fluid_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace bafx::test;
using namespace bafx::windows;

namespace
{

// The shared readback helper only covers RGBA16F; the simulation targets use
// R16G16F, so the velocity field is staged and decoded locally.
struct VectorField
{
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::vector<float> x{};
    std::vector<float> y{};
};

[[nodiscard]] VectorField readbackVectorField(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source)
{
    if (source == nullptr)
    {
        throw std::invalid_argument("velocity readback requires a source");
    }
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_R16G16_FLOAT)
    {
        throw std::invalid_argument("velocity readback requires R16G16F");
    }
    D3D11_TEXTURE2D_DESC stagingDescription = description;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0U;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0U;
    ComPtr<ID3D11Texture2D> staging;
    bafx::windows::throwIfFailed(
        device->CreateTexture2D(&stagingDescription, nullptr, &staging),
        "CreateTexture2D(velocity staging)");
    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    bafx::windows::throwIfFailed(
        context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Map(velocity staging)");

    VectorField field{};
    field.width = description.Width;
    field.height = description.Height;
    field.x.resize(static_cast<std::size_t>(field.width) * field.height);
    field.y.resize(field.x.size());
    const auto* row = static_cast<const std::uint16_t*>(mapped.pData);
    const std::size_t pitchHalfs = mapped.RowPitch / sizeof(std::uint16_t);
    for (std::uint32_t line = 0U; line < field.height; ++line)
    {
        for (std::uint32_t column = 0U; column < field.width; ++column)
        {
            const std::size_t offset =
                static_cast<std::size_t>(line) * pitchHalfs
                + static_cast<std::size_t>(column) * 2U;
            const std::size_t index =
                static_cast<std::size_t>(line) * field.width + column;
            field.x[index] =
                bafx::windows::halfToFloat(row[offset]);
            field.y[index] =
                bafx::windows::halfToFloat(row[offset + 1U]);
        }
    }
    context->Unmap(staging.Get(), 0);
    return field;
}

struct DyeImage
{
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::vector<float> energy{};
    std::vector<float> heat{};
};

[[nodiscard]] DyeImage readbackDye(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source)
{
    const bafx::windows::Rgba16FloatImage raw =
        bafx::windows::readbackRgba16FloatTexture(context, source);
    DyeImage image{};
    image.width = raw.width;
    image.height = raw.height;
    image.energy.reserve(raw.pixels.size());
    image.heat.reserve(raw.pixels.size());
    for (const Rgba16FloatPixel& pixel : raw.pixels)
    {
        image.energy.push_back(bafx::windows::halfToFloat(pixel.red));
        image.heat.push_back(bafx::windows::halfToFloat(pixel.green));
    }
    return image;
}

struct Centroid
{
    float x{0.0F};
    float y{0.0F};
    float weight{0.0F};
};

[[nodiscard]] Centroid energyCentroid(const DyeImage& image)
{
    Centroid centroid{};
    for (std::uint32_t line = 0U; line < image.height; ++line)
    {
        for (std::uint32_t column = 0U; column < image.width; ++column)
        {
            const std::size_t index =
                static_cast<std::size_t>(line) * image.width + column;
            const float weight = std::max(image.energy[index], 0.0F);
            centroid.x += static_cast<float>(column) * weight;
            centroid.y += static_cast<float>(line) * weight;
            centroid.weight += weight;
        }
    }
    if (centroid.weight > 0.0F)
    {
        centroid.x /= centroid.weight;
        centroid.y /= centroid.weight;
    }
    return centroid;
}

[[nodiscard]] float maximumEnergy(const DyeImage& image)
{
    float maximum = 0.0F;
    for (const float value : image.energy)
    {
        maximum = std::max(maximum, value);
    }
    return maximum;
}

[[nodiscard]] float minimumEnergy(const DyeImage& image)
{
    float minimum = 0.0F;
    for (const float value : image.energy)
    {
        minimum = std::min(minimum, value);
    }
    return minimum;
}

// Mean absolute divergence of the velocity field, computed with central
// differences on the CPU so the projection result can be measured.
[[nodiscard]] float meanAbsoluteDivergence(const VectorField& field)
{
    double total = 0.0;
    std::size_t count = 0U;
    for (std::uint32_t line = 1U; line + 1U < field.height; ++line)
    {
        for (std::uint32_t column = 1U; column + 1U < field.width; ++column)
        {
            const std::size_t center =
                static_cast<std::size_t>(line) * field.width + column;
            const float dVx = field.x[center + 1U] - field.x[center - 1U];
            const float dVy =
                field.y[center + field.width] - field.y[center - field.width];
            total += std::abs(0.5 * (dVx + dVy));
            ++count;
        }
    }
    return count == 0U ? 0.0F : static_cast<float>(total / count);
}

[[nodiscard]] float meanSpeed(const VectorField& field)
{
    double total = 0.0;
    for (std::size_t index = 0U; index < field.x.size(); ++index)
    {
        total += std::sqrt(
            static_cast<double>(field.x[index]) * field.x[index]
            + static_cast<double>(field.y[index]) * field.y[index]);
    }
    return field.x.empty()
        ? 0.0F
        : static_cast<float>(total / static_cast<double>(field.x.size()));
}

// Runs a full pipeline cycle against a Debug Layer device and fails when
// the layer stored any ERROR or CORRUPTION message (SRV/RTV conflicts,
// resource hazards). Silently passes when the Graphics Tools feature is not
// installed, because then no debug device can be created at all.
void exercisePipelineForDebugMessages(
    ID3D11Device* device,
    GlowFluidPipeline& pipeline,
    ID3D11RenderTargetView* destination)
{
    Microsoft::WRL::ComPtr<ID3D11Debug> debug;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&debug))))
    {
        return;
    }
    pipeline.splatVelocity(
        64.0F,
        64.0F,
        140.0F,
        120.0F,
        40.0F,
        GlowSplatValue{300.0F, 120.0F, 0.0F},
        GlowSplatMode::SegmentMix,
        0.45F);
    pipeline.splatDye(
        64.0F,
        64.0F,
        140.0F,
        120.0F,
        3.0F,
        18.0F,
        GlowSplatValue{5.6F, 5.6F, 0.7F},
        GlowSplatMode::SegmentAdditive,
        1.0F);
    pipeline.step(0.016F);
    pipeline.display(destination);
    pipeline.clearSimulation();
    pipeline.display(destination);
    // Reallocation paths: resize and quality switches release and rebuild
    // every target.
    pipeline.setViewportSize(320.0F, 240.0F);
    pipeline.setQuality(GlowSimulationQuality::Medium);
    pipeline.splatDye(
        64.0F,
        64.0F,
        80.0F,
        64.0F,
        3.0F,
        12.0F,
        GlowSplatValue{5.6F, 5.6F, 0.7F},
        GlowSplatMode::SegmentAdditive,
        1.0F);
    pipeline.step(0.016F);
    pipeline.display(destination);
    pipeline.setQuality(GlowSimulationQuality::High);
    pipeline.step(0.016F);
    pipeline.display(destination);
}

} // namespace

BAFX_TEST(glow_pipeline_splat_writes_velocity_and_dye)
{
    const WarpDevice graphics = createWarpDevice();
    GlowFluidPipeline pipeline(graphics.device.Get(), graphics.context.Get());
    pipeline.setQuality(GlowSimulationQuality::Low);
    pipeline.setViewportSize(256.0F, 256.0F);
    pipeline.setLinger(10.0F);
    BAFX_CHECK(pipeline.healthy());
    BAFX_CHECK(pipeline.allocated() == false);

    pipeline.splatDye(
        128.0F,
        128.0F,
        128.0F,
        128.0F,
        8.0F,
        8.0F,
        GlowSplatValue{20.0F, 20.0F, 0.5F},
        GlowSplatMode::PointAdditive,
        1.0F);
    BAFX_CHECK(pipeline.healthy());
    BAFX_CHECK(pipeline.allocated());
    BAFX_CHECK(pipeline.dyeWidth() == 256U);
    BAFX_CHECK(pipeline.dyeHeight() == 256U);
    BAFX_CHECK(pipeline.simulationWidth() == 96U);
    BAFX_CHECK(pipeline.simulationHeight() == 96U);
    const FramePixel dye = readPixel(
        graphics.context.Get(), pipeline.dyeReadTexture(), 128U, 128U);
    BAFX_CHECK_NEAR(dye.red, 19.0F, 1.0F);
    BAFX_CHECK_NEAR(dye.alpha, 1.0F, 0.01F);

    pipeline.splatVelocity(
        128.0F,
        128.0F,
        128.0F,
        128.0F,
        400.0F,
        GlowSplatValue{500.0F, 0.0F, 0.0F},
        GlowSplatMode::PointMix,
        1.0F);
    BAFX_CHECK(pipeline.healthy());
    const VectorField velocity = readbackVectorField(
        graphics.device.Get(),
        graphics.context.Get(),
        pipeline.velocityReadTexture());
    BAFX_CHECK(velocity.x[48U * velocity.width + 48U] > 100.0F);
}

BAFX_TEST(glow_pipeline_allocates_reference_quality_dimensions)
{
    const WarpDevice graphics = createWarpDevice();
    GlowFluidPipeline pipeline(graphics.device.Get(), graphics.context.Get());
    BAFX_CHECK(pipeline.healthy());
    BAFX_CHECK(!pipeline.allocated());

    // 16:9 landscape: simulation 160x284, dye 768x1365 (no shrink needed).
    pipeline.setQuality(GlowSimulationQuality::High);
    pipeline.setViewportSize(1920.0F, 1080.0F);
    pipeline.step(0.016F);
    BAFX_CHECK(pipeline.simulationWidth() == 284U);
    BAFX_CHECK(pipeline.simulationHeight() == 160U);
    BAFX_CHECK(pipeline.dyeWidth() == 1365U);
    BAFX_CHECK(pipeline.dyeHeight() == 768U);
    BAFX_CHECK(pipeline.bloomLevelCount() == 5U);
    BAFX_CHECK(pipeline.bloomWidth(0U) == 682U);
    BAFX_CHECK(pipeline.bloomWidth(4U) == 42U);

    pipeline.setQuality(GlowSimulationQuality::Medium);
    pipeline.setViewportSize(1920.0F, 1080.0F);
    pipeline.step(0.016F);
    BAFX_CHECK(pipeline.simulationWidth() == 228U);
    BAFX_CHECK(pipeline.simulationHeight() == 128U);
    BAFX_CHECK(pipeline.dyeWidth() == 1024U);
    BAFX_CHECK(pipeline.dyeHeight() == 576U);

    pipeline.setQuality(GlowSimulationQuality::Low);
    pipeline.setViewportSize(1920.0F, 1080.0F);
    pipeline.step(0.016F);
    BAFX_CHECK(pipeline.simulationWidth() == 171U);
    BAFX_CHECK(pipeline.simulationHeight() == 96U);
    BAFX_CHECK(pipeline.dyeWidth() == 683U);
    BAFX_CHECK(pipeline.dyeHeight() == 384U);
    BAFX_CHECK(pipeline.bloomWidth(0U) == 341U);
    BAFX_CHECK(pipeline.bloomWidth(4U) == 21U);

    // Portrait viewport swaps the long side.
    pipeline.setQuality(GlowSimulationQuality::High);
    pipeline.setViewportSize(1080.0F, 1920.0F);
    pipeline.step(0.016F);
    BAFX_CHECK(pipeline.simulationWidth() == 160U);
    BAFX_CHECK(pipeline.simulationHeight() == 284U);
    BAFX_CHECK(pipeline.dyeWidth() == 768U);
    BAFX_CHECK(pipeline.dyeHeight() == 1365U);

    // A viewport smaller than the dye resolution shrinks the dye targets to
    // the swap-chain size, matching the reference canvas clamp.
    pipeline.setViewportSize(320.0F, 240.0F);
    pipeline.step(0.016F);
    BAFX_CHECK(pipeline.simulationWidth() == 213U);
    BAFX_CHECK(pipeline.simulationHeight() == 160U);
    BAFX_CHECK(pipeline.dyeWidth() == 320U);
    BAFX_CHECK(pipeline.dyeHeight() == 240U);
    BAFX_CHECK(pipeline.bloomWidth(0U) == 160U);
    BAFX_CHECK(pipeline.bloomWidth(4U) == 10U);

    // A fresh pipeline with no content displays full transparency.
    const DisplayTarget target =
        createDisplayTarget(graphics.device.Get(), 320U, 240U);
    pipeline.display(target.view.Get());
    const FramePixel corner =
        readPixel(graphics.context.Get(), target.texture.Get(), 4U, 4U);
    BAFX_CHECK_NEAR(corner.alpha, 0.0F, 0.001F);
    BAFX_CHECK_NEAR(corner.red, 0.0F, 0.001F);
}

BAFX_TEST(glow_pipeline_advect_moves_dye_with_velocity)
{
    const WarpDevice graphics = createWarpDevice();
    GlowFluidPipeline pipeline(graphics.device.Get(), graphics.context.Get());
    pipeline.setQuality(GlowSimulationQuality::Low);
    pipeline.setViewportSize(256.0F, 256.0F);
    // Long linger keeps the decay from eating the dye inside one step.
    pipeline.setLinger(10.0F);

    // A near-uniform velocity field: the blob radius covers the viewport, so
    // the pressure projection has almost nothing to correct.
    pipeline.splatVelocity(
        128.0F,
        128.0F,
        128.0F,
        128.0F,
        400.0F,
        GlowSplatValue{500.0F, 0.0F, 0.0F},
        GlowSplatMode::PointMix,
        1.0F);
    pipeline.splatDye(
        128.0F,
        128.0F,
        128.0F,
        128.0F,
        8.0F,
        8.0F,
        GlowSplatValue{20.0F, 20.0F, 0.5F},
        GlowSplatMode::PointAdditive,
        1.0F);
    pipeline.step(0.0F);

    const Centroid before =
        energyCentroid(readbackDye(graphics.context.Get(), pipeline.dyeReadTexture()));
    BAFX_CHECK(before.weight > 1.0F);

    pipeline.step(0.1F);
    const Centroid after =
        energyCentroid(readbackDye(graphics.context.Get(), pipeline.dyeReadTexture()));

    // Expected shift: 0.1s at 500 px/s over a 96-cell simulation grid, minus
    // the velocity damping and projection applied during the step.
    const float expectedShift = 0.1F * (500.0F * 96.0F / 256.0F) / 96.0F * 256.0F;
    BAFX_CHECK_NEAR(after.x - before.x, expectedShift * 0.9F, 8.0F);
    // No vertical drift for a horizontal field.
    BAFX_CHECK_NEAR(after.y, before.y, 4.0F);
    BAFX_CHECK(after.weight > 0.5F);
}

BAFX_TEST(glow_pipeline_pressure_projection_reduces_divergence)
{
    const WarpDevice graphics = createWarpDevice();
    GlowFluidPipeline pipeline(graphics.device.Get(), graphics.context.Get());
    pipeline.setQuality(GlowSimulationQuality::Low);
    pipeline.setViewportSize(256.0F, 256.0F);
    pipeline.setLinger(10.0F);

    // A lattice of small point splats with alternating directions creates a
    // cell-scale (high frequency) divergent field. Twenty Jacobi iterations
    // cannot remove smooth low-frequency divergence, but they must crush
    // this high-frequency content. The 16px spacing puts the dominant
    // divergence mode inside the range 20 iterations actually converge on.
    constexpr std::uint32_t lattice = 16U;
    constexpr float spacing = 256.0F / static_cast<float>(lattice);
    for (std::uint32_t row = 0U; row < lattice; ++row)
    {
        for (std::uint32_t column = 0U; column < lattice; ++column)
        {
            const bool pushRight = (row + column) % 2U == 0U;
            pipeline.splatVelocity(
                spacing * (static_cast<float>(column) + 0.5F),
                spacing * (static_cast<float>(row) + 0.5F),
                spacing * (static_cast<float>(column) + 0.5F),
                spacing * (static_cast<float>(row) + 0.5F),
                3.0F,
                GlowSplatValue{pushRight ? 700.0F : -700.0F, 0.0F, 0.0F},
                GlowSplatMode::PointMix,
                1.0F);
        }
    }

    // Measure the raw splatted field before any step projects it.
    const VectorField before = readbackVectorField(
        graphics.device.Get(),
        graphics.context.Get(),
        pipeline.velocityReadTexture());
    const float divergenceBefore = meanAbsoluteDivergence(before);
    const float speedBefore = meanSpeed(before);
    // The lattice covers a small fraction of the field, so the mean speed is
    // dominated by the empty cells; the thresholds match that coverage.
    BAFX_CHECK(divergenceBefore > 1.0F);
    BAFX_CHECK(speedBefore > 2.0F);

    // The projection converges gradually: one step runs the reference's 20
    // Jacobi iterations, which cut this field's divergence by a steady
    // factor rather than eliminating it. Assert the trend the ticket asks
    // for: monotone decrease across steps and a substantial total drop,
    // without the field being damped away.
    VectorField field = before;
    float previous = divergenceBefore;
    for (int round = 0; round < 3; ++round)
    {
        pipeline.step(0.0F);
        field = readbackVectorField(
            graphics.device.Get(),
            graphics.context.Get(),
            pipeline.velocityReadTexture());
        const float current = meanAbsoluteDivergence(field);
        BAFX_CHECK(current < previous);
        previous = current;
    }
    BAFX_CHECK(previous < divergenceBefore * 0.5F);
    BAFX_CHECK(meanSpeed(field) > speedBefore * 0.3F);
}

BAFX_TEST(glow_pipeline_maccormack_clamp_prevents_overshoot)
{
    const WarpDevice graphics = createWarpDevice();
    GlowFluidPipeline pipeline(graphics.device.Get(), graphics.context.Get());
    pipeline.setQuality(GlowSimulationQuality::Low);
    pipeline.setViewportSize(256.0F, 256.0F);
    pipeline.setLinger(10.0F);

    // A sharp dye blob inside a strongly non-uniform velocity field: the
    // classic MacCormack undershoot scenario.
    pipeline.splatVelocity(
        64.0F,
        128.0F,
        64.0F,
        128.0F,
        60.0F,
        GlowSplatValue{-600.0F, 0.0F, 0.0F},
        GlowSplatMode::PointMix,
        1.0F);
    pipeline.splatVelocity(
        192.0F,
        128.0F,
        192.0F,
        128.0F,
        60.0F,
        GlowSplatValue{600.0F, 0.0F, 0.0F},
        GlowSplatMode::PointMix,
        1.0F);
    pipeline.splatDye(
        128.0F,
        128.0F,
        128.0F,
        128.0F,
        3.0F,
        3.0F,
        GlowSplatValue{30.0F, 30.0F, 0.5F},
        GlowSplatMode::PointAdditive,
        1.0F);
    pipeline.step(0.0F);
    const float peakBefore =
        maximumEnergy(readbackDye(graphics.context.Get(), pipeline.dyeReadTexture()));
    BAFX_CHECK(peakBefore > 20.0F);

    // Large dt relative to the blob width maximizes correction error.
    pipeline.step(0.08F);
    const DyeImage dye =
        readbackDye(graphics.context.Get(), pipeline.dyeReadTexture());
    // The neighborhood clamp keeps every channel inside the source range, so
    // energy can never undershoot below zero or overshoot the injected peak.
    BAFX_CHECK_NEAR(minimumEnergy(dye), 0.0F, 0.01F);
    BAFX_CHECK(maximumEnergy(dye) <= peakBefore + 0.5F);
}

BAFX_TEST(glow_pipeline_quality_switching_reallocates_without_crash)
{
    const WarpDevice graphics = createWarpDevice();
    const std::array<GlowSimulationQuality, 3> qualities{
        GlowSimulationQuality::Low,
        GlowSimulationQuality::Medium,
        GlowSimulationQuality::High};
    const std::array<std::uint32_t, 3> simulationSides{96U, 128U, 160U};

    for (std::uint32_t round = 0U; round < 3U; ++round)
    {
        auto pipeline = std::make_unique<GlowFluidPipeline>(
            graphics.device.Get(), graphics.context.Get());
        BAFX_CHECK(pipeline->healthy());
        pipeline->setViewportSize(256.0F, 256.0F);
        for (std::size_t index = 0U; index < qualities.size(); ++index)
        {
            pipeline->setQuality(qualities[index]);
            pipeline->setLinger(0.3F);
            pipeline->splatVelocity(
                128.0F,
                128.0F,
                140.0F,
                128.0F,
                40.0F,
                GlowSplatValue{300.0F, 0.0F, 0.0F},
                GlowSplatMode::SegmentMix,
                0.45F);
            pipeline->splatDye(
                128.0F,
                128.0F,
                140.0F,
                128.0F,
                3.0F,
                18.0F,
                GlowSplatValue{5.6F, 5.6F, 0.7F},
                GlowSplatMode::SegmentAdditive,
                1.0F);
            pipeline->step(0.016F);
            pipeline->reset();
            BAFX_CHECK(pipeline->simulationWidth() == simulationSides[index]);
            BAFX_CHECK(pipeline->simulationHeight() == simulationSides[index]);
            BAFX_CHECK(pipeline->healthy());
        }
        const DisplayTarget target =
            createDisplayTarget(graphics.device.Get(), 256U, 256U);
        pipeline->display(target.view.Get());
        pipeline.reset();
        BAFX_CHECK(pipeline == nullptr);
    }
}

BAFX_TEST(glow_pipeline_display_lits_dye_with_tint_and_bloom)
{
    const WarpDevice graphics = createWarpDevice();
    GlowFluidPipeline pipeline(graphics.device.Get(), graphics.context.Get());
    pipeline.setQuality(GlowSimulationQuality::Low);
    pipeline.setViewportSize(256.0F, 256.0F);
    pipeline.setLinger(0.3F);
    pipeline.setAppearance(GlowFluidAppearance{});

    pipeline.splatDye(
        128.0F,
        128.0F,
        128.0F,
        128.0F,
        6.0F,
        6.0F,
        GlowSplatValue{25.0F, 25.0F, 1.0F},
        GlowSplatMode::PointAdditive,
        1.0F);
    pipeline.step(0.016F);

    const DisplayTarget target =
        createDisplayTarget(graphics.device.Get(), 256U, 256U);
    GlowFluidAppearance appearance{};
    appearance.glow = 0.0F;
    pipeline.setAppearance(appearance);
    pipeline.display(target.view.Get());
    const FramePixel plain =
        readPixel(graphics.context.Get(), target.texture.Get(), 128U, 128U);

    appearance.glow = 0.5F;
    pipeline.setAppearance(appearance);
    pipeline.display(target.view.Get());
    const FramePixel glowing =
        readPixel(graphics.context.Get(), target.texture.Get(), 128U, 128U);
    const FramePixel corner =
        readPixel(graphics.context.Get(), target.texture.Get(), 2U, 2U);

    // Default appearance is the 余烬 preset purple (#7c3aed / #a855f7):
    // blue leads, green trails.
    BAFX_CHECK(glowing.blue > glowing.red);
    BAFX_CHECK(glowing.red > glowing.green);
    BAFX_CHECK(glowing.alpha > 0.05F);
    // Bloom adds light on top of the shaded dye.
    BAFX_CHECK(glowing.alpha > plain.alpha);
    // Far from the dye there is no light to composite.
    BAFX_CHECK(corner.alpha < glowing.alpha * 0.1F);
}

BAFX_TEST(glow_pipeline_debug_layer_reports_no_resource_hazards)
{
    constexpr std::array featureLevels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const HRESULT created = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG,
        featureLevels.data(),
        static_cast<UINT>(featureLevels.size()),
        D3D11_SDK_VERSION,
        &device,
        nullptr,
        &context);
    if (FAILED(created))
    {
        // No Graphics Tools on this machine; the shared WARP device in the
        // other tests already covers behavior without the debug layer.
        return;
    }

    GlowFluidPipeline pipeline(device.Get(), context.Get());
    pipeline.setQuality(GlowSimulationQuality::Low);
    pipeline.setViewportSize(256.0F, 256.0F);
    pipeline.setLinger(0.3F);
    const DisplayTarget target = createDisplayTarget(device.Get(), 256U, 256U);
    exercisePipelineForDebugMessages(
        device.Get(), pipeline, target.view.Get());

    Microsoft::WRL::ComPtr<ID3D11InfoQueue> infoQueue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))))
    {
        return;
    }
    const unsigned long long severe =
        infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    std::string report;
    unsigned long long errors = 0ULL;
    for (unsigned long long index = 0ULL; index < severe; ++index)
    {
        SIZE_T length = 0U;
        infoQueue->GetMessage(index, nullptr, &length);
        if (length < sizeof(D3D11_MESSAGE))
        {
            continue;
        }
        std::vector<char> buffer(length);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(buffer.data());
        if (FAILED(infoQueue->GetMessage(index, message, &length)))
        {
            continue;
        }
        if (message->Severity == D3D11_MESSAGE_SEVERITY_ERROR
            || message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION)
        {
            ++errors;
            report += " [";
            report += std::to_string(message->ID);
            report += "] ";
            report += message->pDescription != nullptr
                ? std::string{message->pDescription}
                : std::string{"<no description>"};
        }
    }
    if (errors != 0ULL)
    {
        throw std::runtime_error(
            "D3D11 debug layer stored " + std::to_string(errors)
            + " error message(s):" + report);
    }
}

BAFX_TEST(glow_pipeline_clear_simulation_returns_to_transparent)
{
    const WarpDevice graphics = createWarpDevice();
    GlowFluidPipeline pipeline(graphics.device.Get(), graphics.context.Get());
    pipeline.setQuality(GlowSimulationQuality::Low);
    pipeline.setViewportSize(256.0F, 256.0F);
    pipeline.setLinger(0.3F);

    pipeline.splatDye(
        128.0F,
        128.0F,
        128.0F,
        128.0F,
        6.0F,
        6.0F,
        GlowSplatValue{25.0F, 25.0F, 1.0F},
        GlowSplatMode::PointAdditive,
        1.0F);
    pipeline.step(0.016F);
    const DisplayTarget target =
        createDisplayTarget(graphics.device.Get(), 256U, 256U);
    pipeline.display(target.view.Get());
    const FramePixel lit =
        readPixel(graphics.context.Get(), target.texture.Get(), 128U, 128U);
    BAFX_CHECK(lit.alpha > 0.05F);

    // The idle path clears the simulation; the next frame is transparent.
    pipeline.clearSimulation();
    pipeline.display(target.view.Get());
    const FramePixel cleared =
        readPixel(graphics.context.Get(), target.texture.Get(), 128U, 128U);
    BAFX_CHECK_NEAR(cleared.alpha, 0.0F, 0.001F);
    BAFX_CHECK_NEAR(cleared.red, 0.0F, 0.001F);
}
