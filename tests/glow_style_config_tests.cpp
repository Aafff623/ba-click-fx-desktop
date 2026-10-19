#include "test_support.hpp"

#include "bafx/config/config.hpp"

#include <string>
#include <string_view>

namespace
{

using namespace bafx::config;

// The reference visual preset is the screenshot-tuned "ember" configuration
// from the GlowCursor handoff. Native defaults deliberately adopt it so a
// fresh install shows the intended look without extra setup.
void checkReferenceDefaults(const GlowSettings& glow)
{
    BAFX_CHECK(glow.enabled);
    BAFX_CHECK(glow.color == "#7c3aed");
    BAFX_CHECK(glow.secondaryColor == "#a855f7");
    BAFX_CHECK_NEAR(glow.intensity, 0.3F, 0.00001F);
    BAFX_CHECK_NEAR(glow.trailWidthPx, 3.0F, 0.00001F);
    BAFX_CHECK_NEAR(glow.linger, 0.3F, 0.00001F);
    BAFX_CHECK_NEAR(glow.glow, 0.5F, 0.00001F);
    BAFX_CHECK_NEAR(glow.hotspot, 0.0F, 0.00001F);
    BAFX_CHECK_NEAR(glow.followSpeed, 1.0F, 0.00001F);
    BAFX_CHECK_NEAR(glow.grain, 0.15F, 0.00001F);
    BAFX_CHECK(glow.clickBurst);
    BAFX_CHECK(glow.quality == GlowQuality::High);
}

}

BAFX_TEST(glow_style_defaults_match_reference_preset)
{
    const Config defaults = defaultConfig();
    BAFX_CHECK(defaults.schemaVersion == currentSchemaVersion);
    BAFX_CHECK(defaults.effectStyle == EffectStyle::BA);
    checkReferenceDefaults(defaults.glow);
    BAFX_CHECK(toString(EffectStyle::BA) == "ba");
    BAFX_CHECK(toString(EffectStyle::GlowFluid) == "glowFluid");
    BAFX_CHECK(toString(GlowQuality::Low) == "low");
    BAFX_CHECK(toString(GlowQuality::Medium) == "medium");
    BAFX_CHECK(toString(GlowQuality::High) == "high");
}

BAFX_TEST(glow_style_schema20_migrates_to_21)
{
    // A schema 20 document keeps every authored value and gains the style
    // seam with the BA style selected, so an upgrade never changes rendering.
    Config legacy = defaultConfig();
    legacy.effects.themeColor = "#123456";
    legacy.effects.trailLifetimeMs = 420.0F;
    std::string document = toJson(legacy, false);
    const std::string currentVersionField = "\"schemaVersion\":"
        + std::to_string(currentSchemaVersion);
    const std::size_t versionPosition = document.find(currentVersionField);
    BAFX_CHECK(versionPosition != std::string::npos);
    document.replace(
        versionPosition,
        currentVersionField.size(),
        "\"schemaVersion\":20");
    // Drop the new sections to emulate a genuine pre-21 document.
    const std::size_t stylePosition = document.find("\"effectStyle\":");
    BAFX_CHECK(stylePosition != std::string::npos);
    document.erase(
        stylePosition,
        document.find(',', stylePosition) - stylePosition + 1);
    const std::size_t glowPosition = document.find("\"glow\":");
    BAFX_CHECK(glowPosition != std::string::npos);
    document.erase(
        glowPosition,
        document.find('}', glowPosition) - glowPosition + 2);

    const auto migrated = parseJson(document);
    bafx::test::check(migrated.succeeded(), migrated.message);
    BAFX_CHECK(migrated.config.schemaVersion == currentSchemaVersion);
    BAFX_CHECK(migrated.config.effectStyle == EffectStyle::BA);
    checkReferenceDefaults(migrated.config.glow);
    BAFX_CHECK(migrated.config.effects.themeColor == "#123456");
    BAFX_CHECK_NEAR(migrated.config.effects.trailLifetimeMs, 420.0F, 0.00001F);
}

BAFX_TEST(glow_style_round_trips_through_versioned_json)
{
    Config config = defaultConfig();
    config.effectStyle = EffectStyle::GlowFluid;
    config.glow.color = "#112233";
    config.glow.secondaryColor = "";
    config.glow.intensity = 0.8F;
    config.glow.trailWidthPx = 6.5F;
    config.glow.linger = 1.2F;
    config.glow.glow = 0.9F;
    config.glow.hotspot = 0.4F;
    config.glow.followSpeed = 0.6F;
    config.glow.grain = 0.05F;
    config.glow.clickBurst = false;
    config.glow.quality = GlowQuality::Medium;
    config.glow.enabled = false;

    const auto parsed = parseJson(toJson(config));
    bafx::test::check(parsed.succeeded(), parsed.message);
    BAFX_CHECK(parsed.config.effectStyle == EffectStyle::GlowFluid);
    BAFX_CHECK(parsed.config.glow.color == "#112233");
    BAFX_CHECK(parsed.config.glow.secondaryColor.empty());
    BAFX_CHECK_NEAR(parsed.config.glow.intensity, 0.8F, 0.00001F);
    BAFX_CHECK_NEAR(parsed.config.glow.trailWidthPx, 6.5F, 0.00001F);
    BAFX_CHECK_NEAR(parsed.config.glow.linger, 1.2F, 0.00001F);
    BAFX_CHECK_NEAR(parsed.config.glow.glow, 0.9F, 0.00001F);
    BAFX_CHECK_NEAR(parsed.config.glow.hotspot, 0.4F, 0.00001F);
    BAFX_CHECK_NEAR(parsed.config.glow.followSpeed, 0.6F, 0.00001F);
    BAFX_CHECK_NEAR(parsed.config.glow.grain, 0.05F, 0.00001F);
    BAFX_CHECK(!parsed.config.glow.clickBurst);
    BAFX_CHECK(parsed.config.glow.quality == GlowQuality::Medium);
    BAFX_CHECK(!parsed.config.glow.enabled);

    EffectStyle style = EffectStyle::BA;
    BAFX_CHECK(parseEffectStyle("glowFluid", style) && style == EffectStyle::GlowFluid);
    BAFX_CHECK(parseEffectStyle("ba", style) && style == EffectStyle::BA);
    BAFX_CHECK(!parseEffectStyle("glow", style));
    BAFX_CHECK(!parseEffectStyle("", style));
    GlowQuality quality = GlowQuality::Low;
    BAFX_CHECK(parseGlowQuality("medium", quality) && quality == GlowQuality::Medium);
    BAFX_CHECK(!parseGlowQuality("ultra", quality));
}

BAFX_TEST(glow_parameters_clamp_and_validate)
{
    Config config = defaultConfig();
    config.glow.trailWidthPx = 500.0F;
    config.glow.linger = 99.0F;
    config.glow.followSpeed = 5.0F;
    config.glow.intensity = -1.0F;
    config.glow.grain = -0.5F;
    std::string error;
    BAFX_CHECK(!validateConfig(config, &error));
    BAFX_CHECK(!error.empty());

    // In-range values validate; an out-of-range or malformed color fails.
    Config valid = defaultConfig();
    valid.glow.color = "#ABCDEF";
    valid.glow.secondaryColor = "";
    BAFX_CHECK(validateConfig(valid, &error));
    valid.glow.color = "not-a-color";
    BAFX_CHECK(!validateConfig(valid, &error));
    valid.glow.color = "#7c3aed";
    valid.glow.secondaryColor = "#xyz";
    BAFX_CHECK(!validateConfig(valid, &error));
}

BAFX_TEST(glow_patch_paths_use_style_namespace)
{
    const Config base = defaultConfig();

    const auto stylePatch = applyPatchJson(
        base,
        R"({"path":"effectStyle","value":"glowFluid"})");
    bafx::test::check(stylePatch.succeeded(), stylePatch.message);
    BAFX_CHECK(stylePatch.config.effectStyle == EffectStyle::GlowFluid);

    const auto invalidStyle = applyPatchJson(
        base,
        R"({"path":"effectStyle","value":"glow"})");
    BAFX_CHECK(!invalidStyle.succeeded());
    BAFX_CHECK(invalidStyle.recognized);

    const auto widthPatch = applyPatchJson(
        base,
        R"({"path":"glow.trailWidthPx","value":8.5})");
    bafx::test::check(widthPatch.succeeded(), widthPatch.message);
    BAFX_CHECK_NEAR(widthPatch.config.glow.trailWidthPx, 8.5F, 0.00001F);

    const auto outOfRange = applyPatchJson(
        base,
        R"({"path":"glow.trailWidthPx","value":900})");
    BAFX_CHECK(!outOfRange.succeeded());

    const auto colorPatch = applyPatchJson(
        base,
        R"({"path":"glow.color","value":"#ff00ff"})");
    bafx::test::check(colorPatch.succeeded(), colorPatch.message);
    BAFX_CHECK(colorPatch.config.glow.color == "#ff00ff");

    const auto badColor = applyPatchJson(
        base,
        R"({"path":"glow.color","value":"red"})");
    BAFX_CHECK(!badColor.succeeded());

    const auto qualityPatch = applyPatchJson(
        base,
        R"({"path":"glow.quality","value":"low"})");
    bafx::test::check(qualityPatch.succeeded(), qualityPatch.message);
    BAFX_CHECK(qualityPatch.config.glow.quality == GlowQuality::Low);

    const auto enabledPatch = applyPatchJson(
        base,
        R"({"path":"glow.enabled","value":false})");
    bafx::test::check(enabledPatch.succeeded(), enabledPatch.message);
    BAFX_CHECK(!enabledPatch.config.glow.enabled);

    // The FX-only API keeps owning only the effects namespace.
    const auto fxLeak = applyFxPatchJson(
        base,
        R"({"path":"glow.color","value":"#ff00ff"})");
    BAFX_CHECK(!fxLeak.succeeded());

    // A batch applies atomically across the new namespace.
    const auto batch = applyPatchBatchJson(
        base,
        R"({"effectStyle":"glowFluid","glow.glow":1.5})");
    bafx::test::check(batch.succeeded(), batch.message);
    BAFX_CHECK(batch.config.effectStyle == EffectStyle::GlowFluid);
    BAFX_CHECK_NEAR(batch.config.glow.glow, 1.5F, 0.00001F);
    const auto badBatch = applyPatchBatchJson(
        base,
        R"({"effectStyle":"glowFluid","glow.linger":"wide"})");
    BAFX_CHECK(!badBatch.succeeded());
}

BAFX_TEST(glow_settings_survive_style_switch_and_fx_profile_load)
{
    Config config = defaultConfig();
    config.effectStyle = EffectStyle::GlowFluid;
    config.glow.color = "#123456";
    config.glow.intensity = 1.1F;
    config.glow.quality = GlowQuality::Low;
    const Config glowBaseline = config;

    // Switching the style axis must not rewrite the Glow namespace.
    const auto toBa = applyPatchJson(
        config, R"({"path":"effectStyle","value":"ba"})");
    bafx::test::check(toBa.succeeded(), toBa.message);
    BAFX_CHECK(toBa.config.effectStyle == EffectStyle::BA);
    BAFX_CHECK(toBa.config.glow.color == "#123456");
    BAFX_CHECK_NEAR(toBa.config.glow.intensity, 1.1F, 0.00001F);
    BAFX_CHECK(toBa.config.glow.quality == GlowQuality::Low);

    const auto backToGlow = applyPatchJson(
        toBa.config, R"({"path":"effectStyle","value":"glowFluid"})");
    bafx::test::check(backToGlow.succeeded(), backToGlow.message);
    BAFX_CHECK(backToGlow.config.effectStyle == EffectStyle::GlowFluid);
    BAFX_CHECK(backToGlow.config.glow.color == glowBaseline.glow.color);
    BAFX_CHECK_NEAR(
        backToGlow.config.glow.intensity,
        glowBaseline.glow.intensity,
        0.00001F);

    // An effects-only FX profile document carries no style axis: applying it
    // to a Glow config keeps the style and the Glow parameters intact.
    const auto fxOnly = applyFxPatchJson(
        config, R"({"path":"effects.trailWidth","value":2.5})");
    bafx::test::check(fxOnly.succeeded(), fxOnly.message);
    BAFX_CHECK(fxOnly.config.effectStyle == EffectStyle::GlowFluid);
    BAFX_CHECK_NEAR(fxOnly.config.effects.trailWidth, 2.5F, 0.00001F);
    BAFX_CHECK(fxOnly.config.glow.color == glowBaseline.glow.color);
    BAFX_CHECK_NEAR(
        fxOnly.config.glow.intensity,
        glowBaseline.glow.intensity,
        0.00001F);

    // The reference ember preset stays reproducible field by field, which is
    // what the Control Center reset button applies over the IPC.
    const auto preset = applyPatchBatchJson(
        defaultConfig(),
        R"({"glow.color":"#7c3aed",
            "glow.secondaryColor":"#a855f7",
            "glow.intensity":0.3,
            "glow.trailWidthPx":3,
            "glow.linger":0.3,
            "glow.glow":0.5,
            "glow.hotspot":0,
            "glow.followSpeed":1,
            "glow.grain":0.15,
            "glow.clickBurst":true,
            "glow.quality":"high"})");
    bafx::test::check(preset.succeeded(), preset.message);
    checkReferenceDefaults(preset.config.glow);
}
