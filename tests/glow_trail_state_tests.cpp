#include "test_support.hpp"

#include "bafx/windows/glow_trail_state.hpp"

#include <cmath>

namespace
{

using bafx::windows::GlowTrailSample;
using bafx::windows::GlowTrailState;

}

BAFX_TEST(glow_trail_state_clamps_settings_to_contract)
{
    GlowTrailState state;
    state.setLinger(99.0F);
    BAFX_CHECK_NEAR(state.linger(), 10.0F, 0.00001F);
    state.setLinger(0.001F);
    BAFX_CHECK_NEAR(state.linger(), 0.1F, 0.00001F);
    state.setRadius(500.0F);
    BAFX_CHECK_NEAR(state.radius(), 40.0F, 0.00001F);
    state.setRadius(0.01F);
    BAFX_CHECK_NEAR(state.radius(), 0.5F, 0.00001F);
    // The reference preset values survive clamping untouched.
    state.setLinger(0.3F);
    state.setRadius(3.0F);
    BAFX_CHECK_NEAR(state.linger(), 0.3F, 0.00001F);
    BAFX_CHECK_NEAR(state.radius(), 3.0F, 0.00001F);
}

BAFX_TEST(glow_trail_fade_weight_is_linear_and_bounded)
{
    const GlowTrailSample sample{10.0F, 20.0F, 5.0};
    BAFX_CHECK_NEAR(GlowTrailState::fadeWeight(sample, 5.0, 0.3F), 1.0F, 0.00001F);
    BAFX_CHECK_NEAR(GlowTrailState::fadeWeight(sample, 5.15, 0.3F), 0.5F, 0.00001F);
    BAFX_CHECK_NEAR(GlowTrailState::fadeWeight(sample, 5.3, 0.3F), 0.0F, 0.00001F);
    // Past the linger window stays fully expired, and a future timestamp is
    // treated as fresh instead of overflowing the fade math.
    BAFX_CHECK_NEAR(GlowTrailState::fadeWeight(sample, 9.0, 0.3F), 0.0F, 0.00001F);
    BAFX_CHECK_NEAR(GlowTrailState::fadeWeight(sample, 4.0, 0.3F), 1.0F, 0.00001F);
}

BAFX_TEST(glow_trail_state_accumulates_and_expires_by_linger)
{
    GlowTrailState state;
    state.setLinger(0.3F);
    BAFX_CHECK(!state.needsFrame());
    state.addSample(1.0F, 2.0F, 1.0);
    state.addSample(3.0F, 4.0F, 1.1);
    BAFX_CHECK(state.needsFrame());
    BAFX_CHECK(state.samples().size() == 2U);

    // Inside the window both samples stay live.
    state.advance(1.2);
    BAFX_CHECK(state.samples().size() == 2U);
    BAFX_CHECK(!state.pendingTransparentFrame());

    // The first sample expires at 1.3, the second at 1.4.
    state.advance(1.35);
    BAFX_CHECK(state.samples().size() == 1U);
    state.advance(1.45);
    BAFX_CHECK(state.samples().empty());
    BAFX_CHECK(state.pendingTransparentFrame());
    // The owed transparent frame keeps needsFrame true for exactly one more
    // frame; the Host presents transparency, then the overlay goes idle.
    BAFX_CHECK(state.needsFrame());
    state.clearPendingTransparentFrame();
    BAFX_CHECK(!state.needsFrame());
    BAFX_CHECK(!state.pendingTransparentFrame());
}

BAFX_TEST(glow_trail_state_reset_clears_everything)
{
    GlowTrailState state;
    state.addSample(1.0F, 1.0F, 1.0);
    state.advance(5.0);
    BAFX_CHECK(state.pendingTransparentFrame());
    state.reset();
    BAFX_CHECK(state.samples().empty());
    BAFX_CHECK(!state.pendingTransparentFrame());
    BAFX_CHECK(!state.needsFrame());
    // Settings survive a reset so a pause/resume cycle keeps the user's
    // chosen linger and width.
    state.setLinger(1.5F);
    state.reset();
    BAFX_CHECK_NEAR(state.linger(), 1.5F, 0.00001F);
}
