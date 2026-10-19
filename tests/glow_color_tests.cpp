#include "test_support.hpp"

#include "bafx/windows/glow_color.hpp"

#include <array>

namespace
{

using bafx::windows::deriveSecondaryByHueRotation;
using bafx::windows::fromHsv;
using bafx::windows::parseHexColorRgb;
using bafx::windows::toHsv;

}

BAFX_TEST(glow_color_parses_hex_and_keeps_fallback_on_garbage)
{
    std::array<float, 3> color{};
    parseHexColorRgb("#7c3aed", {{0.0F, 0.0F, 0.0F}}, color);
    BAFX_CHECK_NEAR(color[0], 0.486F, 0.002F);
    BAFX_CHECK_NEAR(color[1], 0.227F, 0.002F);
    BAFX_CHECK_NEAR(color[2], 0.929F, 0.002F);

    parseHexColorRgb("#A855F7", {{0.1F, 0.2F, 0.3F}}, color);
    BAFX_CHECK_NEAR(color[0], 0.659F, 0.002F);
    BAFX_CHECK_NEAR(color[1], 0.333F, 0.002F);
    BAFX_CHECK_NEAR(color[2], 0.969F, 0.002F);

    // Malformed input never blanks the effect: the fallback survives.
    const std::array<float, 3> fallback{0.25F, 0.5F, 0.75F};
    parseHexColorRgb("7c3aed", fallback, color);
    BAFX_CHECK_NEAR(color[0], 0.25F, 0.00001F);
    BAFX_CHECK_NEAR(color[1], 0.5F, 0.00001F);
    BAFX_CHECK_NEAR(color[2], 0.75F, 0.00001F);
    parseHexColorRgb("#7c3aZZ", fallback, color);
    BAFX_CHECK_NEAR(color[0], 0.25F, 0.00001F);
    parseHexColorRgb("", fallback, color);
    BAFX_CHECK_NEAR(color[1], 0.5F, 0.00001F);
}

BAFX_TEST(glow_color_hsv_round_trips)
{
    const std::array<float, 3> colors[]{
        {{0.486F, 0.227F, 0.929F}},
        {{0.659F, 0.333F, 0.969F}},
        {{0.1F, 0.8F, 0.3F}},
        {{0.0F, 0.0F, 0.0F}},
        {{1.0F, 1.0F, 1.0F}}};
    for (const auto& color : colors)
    {
        const std::array<float, 3> hsv = toHsv(color);
        const std::array<float, 3> back =
            fromHsv(hsv[0], hsv[1], hsv[2]);
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            BAFX_CHECK_NEAR(back[channel], color[channel], 0.002F);
        }
    }
}

BAFX_TEST(glow_color_derives_secondary_by_hue_rotation)
{
    const std::array<float, 3> primary{0.486F, 0.227F, 0.929F};
    const std::array<float, 3> secondary =
        deriveSecondaryByHueRotation(primary);
    // Hue +8 degrees with a saturation lift, value unchanged: the violet
    // stays a violet and stays inside [0, 1].
    BAFX_CHECK(secondary[2] > secondary[0]);
    BAFX_CHECK(secondary[0] > secondary[1]);
    BAFX_CHECK(secondary[2] > 0.85F);
    for (const float channel : secondary)
    {
        BAFX_CHECK(channel >= 0.0F);
        BAFX_CHECK(channel <= 1.0F);
    }
    // The derived color is a rotation of the primary, not a copy of it.
    BAFX_CHECK(secondary[0] > primary[0] + 0.01F);
    BAFX_CHECK(secondary[1] < primary[1]);
}
