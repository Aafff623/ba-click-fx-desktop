#include "bafx/windows/glow_trail_state.hpp"

#include <algorithm>

namespace bafx::windows
{

namespace
{

constexpr float minimumLinger = 0.1F;
constexpr float maximumLinger = 10.0F;
constexpr float minimumRadius = 0.5F;
constexpr float maximumRadius = 40.0F;

[[nodiscard]] float clampValue(float value, float low, float high) noexcept
{
    return std::min(std::max(value, low), high);
}

}

void GlowTrailState::setLinger(const float seconds) noexcept
{
    linger_ = clampValue(seconds, minimumLinger, maximumLinger);
}

void GlowTrailState::setRadius(const float pixels) noexcept
{
    radius_ = clampValue(pixels, minimumRadius, maximumRadius);
}

void GlowTrailState::addSample(
    const float x,
    const float y,
    const double nowSeconds) noexcept
{
    if (samples_.size() >= maximumGlowTrailSamples)
    {
        // Input can outrun the render slot; drop the oldest sample so the
        // buffer and the GPU vertex budget stay bounded.
        samples_.erase(samples_.begin());
    }
    samples_.push_back(GlowTrailSample{x, y, nowSeconds});
}

void GlowTrailState::advance(
    const double nowSeconds,
    const bool emitterResting) noexcept
{
    const bool hadSamples = !samples_.empty();
    if (emitterResting)
    {
        // The reference keeps rendering while the emitter is still catching
        // up with a stopped pointer, so expiry only runs at rest.
        const float window = quietWindow();
        const auto freshEnd = std::remove_if(
            samples_.begin(),
            samples_.end(),
            [nowSeconds, window](const GlowTrailSample& sample)
            {
                return nowSeconds - sample.atSeconds
                    > static_cast<double>(window);
            });
        samples_.erase(freshEnd, samples_.end());
    }
    if (hadSamples && samples_.empty())
    {
        // The trail just emptied: owe exactly one transparent frame so the
        // overlay does not keep the faded trail on screen after idle.
        pendingTransparentFrame_ = true;
    }
}

void GlowTrailState::clearPendingTransparentFrame() noexcept
{
    pendingTransparentFrame_ = false;
}

void GlowTrailState::reset() noexcept
{
    samples_.clear();
    pendingTransparentFrame_ = false;
}

const std::vector<GlowTrailSample>& GlowTrailState::samples() const noexcept
{
    return samples_;
}

bool GlowTrailState::needsFrame() const noexcept
{
    return !samples_.empty() || pendingTransparentFrame_;
}

bool GlowTrailState::pendingTransparentFrame() const noexcept
{
    return pendingTransparentFrame_;
}

float GlowTrailState::linger() const noexcept
{
    return linger_;
}

float GlowTrailState::radius() const noexcept
{
    return radius_;
}

float GlowTrailState::quietWindow() const noexcept
{
    // Reference: linger * 1200 + 900 milliseconds after the last activity.
    return linger_ * 1.2F + 0.9F;
}

float GlowTrailState::fadeWeight(
    const GlowTrailSample& sample,
    const double nowSeconds,
    const float linger) noexcept
{
    const double age = nowSeconds - sample.atSeconds;
    if (age < 0.0 || linger <= 0.0F)
    {
        // A future timestamp is treated as fresh rather than as an overflow.
        return age < 0.0 ? 1.0F : 0.0F;
    }
    const double normalized = age / static_cast<double>(linger);
    if (normalized >= 1.0)
    {
        return 0.0F;
    }
    return static_cast<float>(1.0 - normalized);
}

}
