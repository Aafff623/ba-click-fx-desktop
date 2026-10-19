#include "glow_test_support.hpp"

#include "bafx/windows/glow_fluid_renderer.hpp"

#include <algorithm>

using namespace bafx::windows;

namespace
{

constexpr double frameStep = 1.0 / 60.0;

} // namespace

BAFX_TEST(glow_renderer_movement_emits_trail_and_click_bursts)
{
    const bafx::test::WarpDevice graphics = bafx::test::createWarpDevice();
    GlowFluidRenderer renderer(
        graphics.device.Get(), graphics.context.Get());
    renderer.setSettings(GlowRenderSettings{});
    renderer.setViewportSize(256.0F, 256.0F);
    const bafx::test::DisplayTarget target =
        bafx::test::createDisplayTarget(graphics.device.Get(), 256U, 256U);

    double now = 1.0;
    // Drag across the middle: every frame the emitter moves, so dye lands
    // along the path and the center pixel lights up.
    for (int step = 0; step < 24; ++step)
    {
        renderer.submitPointerSample(
            40.0F + static_cast<float>(step) * 4.0F, 128.0F, now);
        renderer.render(target.view.Get(), now);
        now += frameStep;
    }
    BAFX_CHECK(renderer.needsFrame());
    const bafx::test::FramePixel moved = bafx::test::readPixel(
        graphics.context.Get(), target.texture.Get(), 128U, 128U);
    BAFX_CHECK(moved.alpha > 0.02F);
    // 余烬 preset purple: blue leads, green trails.
    BAFX_CHECK(moved.blue > moved.green);

    // A click injects the reference burst jets at the click position.
    renderer.submitPointerClick(200.0F, 60.0F);
    renderer.render(target.view.Get(), now);
    now += frameStep;
    const bafx::test::FramePixel burst = bafx::test::readPixel(
        graphics.context.Get(), target.texture.Get(), 200U, 60U);
    BAFX_CHECK(burst.alpha > 0.02F);
    BAFX_CHECK(burst.alpha > moved.alpha * 0.2F);
}

BAFX_TEST(glow_renderer_settles_then_sleeps_without_phantom_trail)
{
    const bafx::test::WarpDevice graphics = bafx::test::createWarpDevice();
    GlowFluidRenderer renderer(
        graphics.device.Get(), graphics.context.Get());
    renderer.setSettings(GlowRenderSettings{});
    renderer.setViewportSize(256.0F, 256.0F);
    const bafx::test::DisplayTarget target =
        bafx::test::createDisplayTarget(graphics.device.Get(), 256U, 256U);

    double now = 1.0;
    for (int step = 0; step < 24; ++step)
    {
        renderer.submitPointerSample(
            40.0F + static_cast<float>(step) * 4.0F, 128.0F, now);
        renderer.render(target.view.Get(), now);
        now += frameStep;
    }
    BAFX_CHECK(renderer.needsFrame());

    // Stop moving: the emitter rests, the quiet window (linger 0.3 -> 1.26s)
    // elapses, one transparent frame is presented and the style sleeps.
    int frames = 0;
    while (renderer.needsFrame() && frames < 400)
    {
        renderer.render(target.view.Get(), now);
        now += frameStep;
        ++frames;
    }
    BAFX_CHECK(!renderer.needsFrame());
    BAFX_CHECK(frames > 60);

    // The last presented frame is fully transparent: no faded ghost of the
    // old trail survives the idle.
    const bafx::test::FramePixel settled = bafx::test::readPixel(
        graphics.context.Get(), target.texture.Get(), 128U, 128U);
    BAFX_CHECK_NEAR(settled.alpha, 0.0F, 0.002F);
    BAFX_CHECK_NEAR(settled.blue, 0.0F, 0.002F);

    // Waking with movement far from the old trail lights only the new path.
    renderer.submitPointerSample(20.0F, 20.0F, now);
    renderer.submitPointerSample(28.0F, 20.0F, now);
    renderer.render(target.view.Get(), now);
    const bafx::test::FramePixel fresh = bafx::test::readPixel(
        graphics.context.Get(), target.texture.Get(), 24U, 20U);
    const bafx::test::FramePixel stale = bafx::test::readPixel(
        graphics.context.Get(), target.texture.Get(), 128U, 128U);
    BAFX_CHECK(fresh.alpha > 0.01F);
    BAFX_CHECK(stale.alpha < fresh.alpha * 0.2F);
}

BAFX_TEST(glow_renderer_follow_speed_spring_lags_behind_pointer)
{
    const bafx::test::WarpDevice graphics = bafx::test::createWarpDevice();
    GlowFluidRenderer renderer(
        graphics.device.Get(), graphics.context.Get());
    GlowRenderSettings settings{};
    // Below 1 the emitter is a damped spring, so dye lands behind the raw
    // pointer position for a while after the jump.
    settings.followSpeed = 0.2F;
    renderer.setSettings(settings);
    renderer.setViewportSize(256.0F, 256.0F);
    const bafx::test::DisplayTarget target =
        bafx::test::createDisplayTarget(graphics.device.Get(), 256U, 256U);

    double now = 1.0;
    renderer.submitPointerSample(20.0F, 128.0F, now);
    renderer.render(target.view.Get(), now);
    now += frameStep;
    // Jump the pointer to the right edge; the spring emitter trails it.
    renderer.submitPointerSample(220.0F, 128.0F, now);
    for (int frame = 0; frame < 6; ++frame)
    {
        renderer.render(target.view.Get(), now);
        now += frameStep;
    }
    const bafx::test::FramePixel trailing = bafx::test::readPixel(
        graphics.context.Get(), target.texture.Get(), 90U, 128U);
    const bafx::test::FramePixel pointerSpot = bafx::test::readPixel(
        graphics.context.Get(), target.texture.Get(), 215U, 128U);
    BAFX_CHECK(trailing.alpha > 0.01F);
    BAFX_CHECK(trailing.alpha > pointerSpot.alpha);
}
