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

BAFX_TEST(glow_trail_state_expires_on_reference_quiet_window_at_rest)
{
    GlowTrailState state;
    state.setLinger(0.3F);
    // Reference quiet window: linger * 1200ms + 900ms = 1260ms.
    BAFX_CHECK_NEAR(state.quietWindow(), 1.26F, 0.00001F);

    BAFX_CHECK(!state.needsFrame());
    state.addSample(1.0F, 2.0F, 1.0);
    state.addSample(3.0F, 4.0F, 1.1);
    BAFX_CHECK(state.needsFrame());
    BAFX_CHECK(state.samples().size() == 2U);

    // Inside the quiet window both samples stay live even though the
    // placeholder fade already reached zero.
    state.advance(2.0, true);
    BAFX_CHECK(state.samples().size() == 2U);
    BAFX_CHECK(!state.pendingTransparentFrame());

    // The first sample expires at 2.26, the second at 2.36.
    state.advance(2.3, true);
    BAFX_CHECK(state.samples().size() == 1U);
    state.advance(2.4, true);
    BAFX_CHECK(state.samples().empty());
    BAFX_CHECK(state.pendingTransparentFrame());
    // The owed transparent frame keeps needsFrame true for exactly one more
    // frame; the Host presents transparency, then the overlay goes idle.
    BAFX_CHECK(state.needsFrame());
    state.clearPendingTransparentFrame();
    BAFX_CHECK(!state.needsFrame());
    BAFX_CHECK(!state.pendingTransparentFrame());
}

BAFX_TEST(glow_trail_state_holds_samples_while_emitter_catches_up)
{
    GlowTrailState state;
    state.setLinger(0.3F);
    state.addSample(1.0F, 2.0F, 1.0);

    // A moving emitter means the pointer just stopped but the trail source
    // is still travelling: nothing expires and the render loop stays alive.
    state.advance(5.0, false);
    BAFX_CHECK(state.samples().size() == 1U);
    BAFX_CHECK(!state.pendingTransparentFrame());
    BAFX_CHECK(state.needsFrame());

    // Once the emitter rests, the same timestamp expires immediately.
    state.advance(5.0, true);
    BAFX_CHECK(state.samples().empty());
    BAFX_CHECK(state.pendingTransparentFrame());
}

BAFX_TEST(glow_trail_state_full_cycle_active_fade_idle_wake)
{
    GlowTrailState state;
    state.setLinger(0.3F);

    // Active: movement keeps arriving.
    state.addSample(10.0F, 10.0F, 1.0);
    state.advance(1.5, true);
    BAFX_CHECK(state.needsFrame());
    state.addSample(20.0F, 12.0F, 1.5);
    // The first sample survives until its own quiet window ends at 2.26.
    state.advance(2.0, true);
    BAFX_CHECK(state.needsFrame());
    BAFX_CHECK(state.samples().size() == 2U);
    state.advance(2.3, true);
    BAFX_CHECK(state.samples().size() == 1U);

    // Fading: past the last sample but still inside the quiet window.
    state.advance(2.7, true);
    BAFX_CHECK(state.needsFrame());
    BAFX_CHECK(!state.pendingTransparentFrame());

    // Idle: quiet window elapsed at rest, one transparent frame owed.
    state.advance(2.8, true);
    BAFX_CHECK(state.samples().empty());
    BAFX_CHECK(state.pendingTransparentFrame());
    BAFX_CHECK(state.needsFrame());
    state.clearPendingTransparentFrame();
    BAFX_CHECK(!state.needsFrame());

    // Wake: a fresh sample revives the trail without any phantom sample
    // surviving from before the idle.
    state.addSample(30.0F, 30.0F, 5.0);
    BAFX_CHECK(state.needsFrame());
    BAFX_CHECK(state.samples().size() == 1U);
    BAFX_CHECK(state.samples().front().x == 30.0F);
    state.advance(5.5, true);
    BAFX_CHECK(state.needsFrame());
}

BAFX_TEST(glow_trail_state_reset_clears_everything)
{
    GlowTrailState state;
    state.addSample(1.0F, 1.0F, 1.0);
    state.advance(5.0, true);
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
