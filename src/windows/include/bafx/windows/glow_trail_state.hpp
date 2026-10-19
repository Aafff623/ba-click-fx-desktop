#pragma once

#include <vector>

namespace bafx::windows
{

// Placeholder trail accumulation for the Glow style slice. The real fluid
// simulation replaces the visual side in the next ticket, but this state
// machine (accumulate, expire by linger, transparent frame, idle) is the
// contract the product depends on and is deliberately GPU-free so it can be
// tested without a device.
inline constexpr std::size_t maximumGlowTrailSamples = 1024U;

struct GlowTrailSample final
{
    float x{0.0F};
    float y{0.0F};
    double atSeconds{0.0};
};

class GlowTrailState final
{
public:
    void setLinger(float seconds) noexcept;
    void setRadius(float pixels) noexcept;

    void addSample(float x, float y, double nowSeconds) noexcept;
    // Expires samples older than the linger window. When the last live sample
    // expires, one transparent frame is owed so the overlay does not keep the
    // faded trail on screen after idle.
    void advance(double nowSeconds) noexcept;
    // Called after the owed transparent frame was presented.
    void clearPendingTransparentFrame() noexcept;
    void reset() noexcept;

    [[nodiscard]] const std::vector<GlowTrailSample>& samples() const noexcept;
    [[nodiscard]] bool needsFrame() const noexcept;
    [[nodiscard]] bool pendingTransparentFrame() const noexcept;
    [[nodiscard]] float linger() const noexcept;
    [[nodiscard]] float radius() const noexcept;

    // Per-sample fade weight in [0, 1]; 1 is fresh, 0 is fully expired.
    [[nodiscard]] static float fadeWeight(
        const GlowTrailSample& sample,
        double nowSeconds,
        float linger) noexcept;

private:
    std::vector<GlowTrailSample> samples_{};
    float linger_{0.3F};
    float radius_{3.0F};
    bool pendingTransparentFrame_{false};
};

}
