#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace bafx::windows
{

// Parses a #rrggbb color into [0, 1] channels. Invalid input keeps the
// caller's fallback so a malformed config can never blank the effect.
inline void parseHexColorRgb(
    const std::string_view value,
    const std::array<float, 3>& fallback,
    std::array<float, 3>& out) noexcept
{
    out = fallback;
    if (value.size() != 7U || value.front() != '#')
    {
        return;
    }
    const auto digit = [](const char character) noexcept
    {
        if (character >= '0' && character <= '9')
        {
            return static_cast<int>(character - '0');
        }
        if (character >= 'a' && character <= 'f')
        {
            return static_cast<int>(character - 'a') + 10;
        }
        if (character >= 'A' && character <= 'F')
        {
            return static_cast<int>(character - 'A') + 10;
        }
        return -1;
    };
    const auto channel = [&value, &digit](const std::size_t index) noexcept
    {
        const int high = digit(value[index]);
        const int low = digit(value[index + 1U]);
        if (high < 0 || low < 0)
        {
            return -1.0F;
        }
        return static_cast<float>((high << 4) | low) / 255.0F;
    };
    const float red = channel(1);
    const float green = channel(3);
    const float blue = channel(5);
    if (red < 0.0F || green < 0.0F || blue < 0.0F)
    {
        return;
    }
    out = std::array<float, 3>{red, green, blue};
}

// RGB to HSV, matching the reference implementation's channel math.
inline std::array<float, 3> toHsv(const std::array<float, 3>& rgb) noexcept
{
    const float maximum = std::max({rgb[0], rgb[1], rgb[2]});
    const float minimum = std::min({rgb[0], rgb[1], rgb[2]});
    const float delta = maximum - minimum;
    float hue = 0.0F;
    if (delta > 0.0F)
    {
        if (maximum == rgb[0])
        {
            hue = std::fmod((rgb[1] - rgb[2]) / delta, 6.0F);
        }
        else if (maximum == rgb[1])
        {
            hue = (rgb[2] - rgb[0]) / delta + 2.0F;
        }
        else
        {
            hue = (rgb[0] - rgb[1]) / delta + 4.0F;
        }
    }
    hue = std::fmod(hue * 60.0F + 360.0F, 360.0F);
    const float saturation = maximum > 0.0F ? delta / maximum : 0.0F;
    return std::array<float, 3>{hue, saturation, maximum};
}

// HSV to RGB, matching the reference implementation's sector selection.
inline std::array<float, 3> fromHsv(
    const float hue,
    const float saturation,
    const float value) noexcept
{
    const float h = std::fmod(std::fmod(hue, 360.0F) + 360.0F, 360.0F) / 60.0F;
    const float chroma = value * saturation;
    const float x = chroma * (1.0F - std::fabs(std::fmod(h, 2.0F) - 1.0F));
    const float m = value - chroma;
    const std::array<std::array<float, 3>, 6> sectors{{
        {{chroma, x, 0.0F}},
        {{x, chroma, 0.0F}},
        {{0.0F, chroma, x}},
        {{0.0F, x, chroma}},
        {{x, 0.0F, chroma}},
        {{chroma, 0.0F, x}}}};
    const auto sector = sectors[static_cast<std::size_t>(h) % 6U];
    return std::array<float, 3>{
        sector[0] + m,
        sector[1] + m,
        sector[2] + m};
}

// Derives the cooling color from the primary by a small hue rotation with a
// saturation lift, exactly like the reference when secondaryColor is empty.
inline std::array<float, 3> deriveSecondaryByHueRotation(
    const std::array<float, 3>& color) noexcept
{
    const std::array<float, 3> hsv = toHsv(color);
    return fromHsv(
        hsv[0] + 8.0F,
        std::min(1.0F, hsv[1] * 1.1F + 0.05F),
        hsv[2]);
}

} // namespace bafx::windows
