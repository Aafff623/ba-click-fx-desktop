#pragma once

// HLSL port of the GlowCursor WebGL2 reference implementation (see
// temp/glow-handoff/BAFX-GlowCursor-Handoff/source/GlowCursor.jsx, kept out
// of version control). The pass names, uniform names and math mirror the
// GLSL sources one-to-one so the port can be diffed against the reference.
//
// Texture coordinate convention: unlike WebGL, D3D11 texture space has its
// origin at the top-left, so uv.y grows downward on screen. Pointer samples
// therefore map to uv without the reference's 1.0 - y flip, and every pass
// shader below is orientation-agnostic (the boundary conditions in the
// divergence pass stay symmetric under this convention).
//
// Constant buffer layouts follow HLSL packing: values pack tightly at
// 4-byte alignment and only move to the next 16-byte row when they would
// otherwise cross one (a float3 therefore skips from 28 to 32).
// glow_fluid_shader_tests.cpp pins every uniform offset through shader
// reflection; the CPU-side structs in glow_fluid_pipeline.cpp must match
// those offsets exactly.

#include <string_view>

namespace bafx::windows
{

// Fullscreen triangle emitted from a three-vertex buffer; a POSITION input
// keeps the input-assembly contract explicit (a null layout with an
// SV_VertexID-only signature is silently dropped by the retail runtime).
// Carries uv plus the four neighbor uvs the stencil passes (divergence /
// pressure / gradient) sample at one texel offsets.
inline constexpr char kGlowFluidVertexShader[] = R"hlsl(
cbuffer Transform : register(b0)
{
    float2 uTexel;
    float2 uPadding;
}

struct VSInput
{
    float2 position : POSITION;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 vUv : TEXCOORD0;
    float2 vL : TEXCOORD1;
    float2 vR : TEXCOORD2;
    float2 vT : TEXCOORD3;
    float2 vB : TEXCOORD4;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.vUv = input.position * 0.5 + 0.5;
    output.vL = output.vUv - float2(uTexel.x, 0.0);
    output.vR = output.vUv + float2(uTexel.x, 0.0);
    output.vT = output.vUv + float2(0.0, uTexel.y);
    output.vB = output.vUv - float2(0.0, uTexel.y);
    return output;
}
)hlsl";

// Shared pixel-stage input contract.
inline constexpr char kGlowFluidPixelHeader[] = R"hlsl(
struct PSInput
{
    float4 position : SV_POSITION;
    float2 vUv : TEXCOORD0;
    float2 vL : TEXCOORD1;
    float2 vR : TEXCOORD2;
    float2 vT : TEXCOORD3;
    float2 vB : TEXCOORD4;
};
)hlsl";

// Shared dye -> light shading, concatenated into the prefilter and display
// passes exactly like the reference's ${HEADER}${SHADE} composition.
inline constexpr char kGlowFluidShadeChunk[] = R"hlsl(
float3 shadeDye(float3 dye, float3 tintColor, float3 tintSecondary, float intensity, float hotspot)
{
    float energy = max(dye.x, 0.0);
    float heat = clamp(dye.y, 0.0, energy);
    float gas = max(dye.z, 0.0);
    float fresh = heat / max(energy, 0.0001);
    float3 tint = lerp(tintSecondary, tintColor, fresh);
    float spark = heat * heat * 0.3 * hotspot;
    return (tint * energy + tintSecondary * gas + float3(spark, spark, spark)) * intensity;
}
)hlsl";

// Splats a moving segment (uMode >= 2) or a point (uMode < 2) into a
// velocity/dye pair. Additive modes grow the field; mix modes blend toward
// the value. The reference emits velocity with a mix segment and dye with an
// additive segment.
inline constexpr char kGlowFluidSplatShader[] = R"hlsl(
cbuffer SplatConstants : register(b0)
{
    float uAspect;
    float2 uA;
    float2 uB;
    float uRadius;
    float uWide;
    float3 uValue;
    float uMode;
    float uStrength;
}

Texture2D uTarget : register(t0);
SamplerState linearSampler : register(s0);

float erf(float x)
{
    float x2 = x * x;
    float a = 0.147;
    return sign(x) * sqrt(1.0 - exp(-x2 * (1.2732395 + a * x2) / (1.0 + a * x2)));
}

float sweep(float along, float length, float across, float radius)
{
    return exp(-across * across / (radius * radius)) * 0.5 * (erf(along / radius) - erf((along - length) / radius));
}

float4 main(PSInput input) : SV_TARGET
{
    float2 p = float2(input.vUv.x * uAspect, input.vUv.y);
    float2 a = float2(uA.x * uAspect, uA.y);
    float2 ab = float2(uB.x * uAspect, uB.y) - a;
    float len = length(ab);
    float2 dir = len > 1e-6 ? ab / len : float2(1.0, 0.0);
    float2 ap = p - a;
    float3 base = uTarget.Sample(linearSampler, input.vUv).xyz;
    if (uMode > 1.5)
    {
        float along = dot(ap, dir);
        float across = dot(ap, float2(-dir.y, dir.x));
        if (uMode > 2.5)
        {
            return float4(lerp(base, uValue, sweep(along, len, across, uRadius) * uStrength), 1.0);
        }
        float3 add = float3(uValue.xy * sweep(along, len, across, uRadius), uValue.z * sweep(along, len, across, uWide));
        return float4(base + add, 1.0);
    }
    float h = clamp(dot(ap, ab) / max(dot(ab, ab), 1e-9), 0.0, 1.0);
    float2 d = ap - ab * h;
    float r2 = dot(d, d);
    float falloff = exp(-r2 / (uRadius * uRadius));
    float wide = exp(-r2 / (uWide * uWide));
    float3 result = uMode > 0.5 ? lerp(base, uValue, falloff * uStrength) : base + float3(uValue.xy * falloff, uValue.z * wide);
    return float4(result, 1.0);
}
)hlsl";

// Semi-Lagrangian advection with per-channel decay.
inline constexpr char kGlowFluidAdvectShader[] = R"hlsl(
cbuffer AdvectConstants : register(b0)
{
    float2 uVelocityTexel;
    float uDt;
    float4 uDecay;
}

Texture2D uVelocity : register(t0);
Texture2D uSource : register(t1);
SamplerState linearSampler : register(s0);

float4 main(PSInput input) : SV_TARGET
{
    float2 coord = input.vUv - uDt * uVelocity.Sample(linearSampler, input.vUv).xy * uVelocityTexel;
    return uSource.Sample(linearSampler, coord) * uDecay;
}
)hlsl";

// MacCormack advection with the neighborhood clamp that keeps the corrected
// value inside the forward/back sampled range.
inline constexpr char kGlowFluidMaccormackShader[] = R"hlsl(
cbuffer MaccormackConstants : register(b0)
{
    float2 uVelocityTexel;
    float2 uSourceTexel;
    float uDt;
    float4 uDecay;
}

Texture2D uVelocity : register(t0);
Texture2D uSource : register(t1);
Texture2D uForward : register(t2);
Texture2D uBack : register(t3);
SamplerState linearSampler : register(s0);

float4 main(PSInput input) : SV_TARGET
{
    float2 coord = input.vUv - uDt * uVelocity.Sample(linearSampler, input.vUv).xy * uVelocityTexel;
    float4 forward = uForward.Sample(linearSampler, input.vUv);
    float4 corrected = forward + 0.5 * (uSource.Sample(linearSampler, input.vUv) - uBack.Sample(linearSampler, input.vUv));
    float2 base = (floor(coord / uSourceTexel - 0.5) + 0.5) * uSourceTexel;
    float4 a = uSource.Sample(linearSampler, base);
    float4 b = uSource.Sample(linearSampler, base + float2(uSourceTexel.x, 0.0));
    float4 c = uSource.Sample(linearSampler, base + float2(0.0, uSourceTexel.y));
    float4 d = uSource.Sample(linearSampler, base + uSourceTexel);
    float4 low = min(min(a, b), min(c, d));
    float4 high = max(max(a, b), max(c, d));
    return clamp(corrected, low, high) * uDecay;
}
)hlsl";

// Velocity divergence with free-slip (no-through) boundaries.
inline constexpr char kGlowFluidDivergenceShader[] = R"hlsl(
Texture2D uVelocity : register(t0);
SamplerState linearSampler : register(s0);

float4 main(PSInput input) : SV_TARGET
{
    float left = uVelocity.Sample(linearSampler, input.vL).x;
    float right = uVelocity.Sample(linearSampler, input.vR).x;
    float top = uVelocity.Sample(linearSampler, input.vT).y;
    float bottom = uVelocity.Sample(linearSampler, input.vB).y;
    float2 center = uVelocity.Sample(linearSampler, input.vUv).xy;
    if (input.vL.x < 0.0) left = -center.x;
    if (input.vR.x > 1.0) right = -center.x;
    if (input.vT.y > 1.0) top = -center.y;
    if (input.vB.y < 0.0) bottom = -center.y;
    return float4(0.5 * (right - left + top - bottom), 0.0, 0.0, 1.0);
}
)hlsl";

inline constexpr char kGlowFluidScaleShader[] = R"hlsl(
cbuffer ScaleConstants : register(b0)
{
    float uValue;
}

Texture2D uSource : register(t0);
SamplerState pointSampler : register(s0);

float4 main(PSInput input) : SV_TARGET
{
    return uSource.Sample(pointSampler, input.vUv) * uValue;
}
)hlsl";

// One Jacobi pressure iteration.
inline constexpr char kGlowFluidPressureShader[] = R"hlsl(
Texture2D uPressure : register(t0);
Texture2D uDivergence : register(t1);
SamplerState pointSampler : register(s0);

float4 main(PSInput input) : SV_TARGET
{
    float left = uPressure.Sample(pointSampler, input.vL).x;
    float right = uPressure.Sample(pointSampler, input.vR).x;
    float top = uPressure.Sample(pointSampler, input.vT).x;
    float bottom = uPressure.Sample(pointSampler, input.vB).x;
    float divergence = uDivergence.Sample(pointSampler, input.vUv).x;
    return float4((left + right + bottom + top - divergence) * 0.25, 0.0, 0.0, 1.0);
}
)hlsl";

// Subtracts the pressure gradient, projecting velocity onto its
// divergence-free part.
inline constexpr char kGlowFluidGradientShader[] = R"hlsl(
Texture2D uPressure : register(t0);
Texture2D uVelocity : register(t1);
SamplerState pointSampler : register(s0);
SamplerState linearSampler : register(s1);

float4 main(PSInput input) : SV_TARGET
{
    float left = uPressure.Sample(pointSampler, input.vL).x;
    float right = uPressure.Sample(pointSampler, input.vR).x;
    float top = uPressure.Sample(pointSampler, input.vT).x;
    float bottom = uPressure.Sample(pointSampler, input.vB).x;
    float2 velocity = uVelocity.Sample(linearSampler, input.vUv).xy - float2(right - left, top - bottom);
    return float4(velocity, 0.0, 1.0);
}
)hlsl";

inline constexpr char kGlowFluidPrefilterShader[] = R"hlsl(
cbuffer PrefilterConstants : register(b0)
{
    float2 uDyeTexel;
    float3 uColor;
    float3 uSecondary;
    float uIntensity;
    float uHotspot;
}

Texture2D uDye : register(t0);
SamplerState linearSampler : register(s0);

float4 main(PSInput input) : SV_TARGET
{
    float2 o = uDyeTexel * 0.5;
    float3 light = shadeDye(uDye.Sample(linearSampler, input.vUv + float2(-o.x, -o.y)).xyz, uColor, uSecondary, uIntensity, uHotspot);
    light += shadeDye(uDye.Sample(linearSampler, input.vUv + float2(o.x, -o.y)).xyz, uColor, uSecondary, uIntensity, uHotspot);
    light += shadeDye(uDye.Sample(linearSampler, input.vUv + float2(-o.x, o.y)).xyz, uColor, uSecondary, uIntensity, uHotspot);
    light += shadeDye(uDye.Sample(linearSampler, input.vUv + float2(o.x, o.y)).xyz, uColor, uSecondary, uIntensity, uHotspot);
    return float4(light * 0.25, 1.0);
}
)hlsl";

inline constexpr char kGlowFluidDownShader[] = R"hlsl(
cbuffer DownConstants : register(b0)
{
    float2 uSourceTexel;
}

Texture2D uSource : register(t0);
SamplerState linearSampler : register(s0);

float4 main(PSInput input) : SV_TARGET
{
    float2 o = uSourceTexel;
    float3 color = uSource.Sample(linearSampler, input.vUv).rgb * 4.0;
    color += uSource.Sample(linearSampler, input.vUv + float2(-o.x, -o.y)).rgb;
    color += uSource.Sample(linearSampler, input.vUv + float2(o.x, -o.y)).rgb;
    color += uSource.Sample(linearSampler, input.vUv + float2(-o.x, o.y)).rgb;
    color += uSource.Sample(linearSampler, input.vUv + float2(o.x, o.y)).rgb;
    return float4(color / 8.0, 1.0);
}
)hlsl";

inline constexpr char kGlowFluidUpShader[] = R"hlsl(
cbuffer UpConstants : register(b0)
{
    float2 uSourceTexel;
}

Texture2D uSource : register(t0);
SamplerState linearSampler : register(s0);

float4 main(PSInput input) : SV_TARGET
{
    float2 o = uSourceTexel;
    float3 color = uSource.Sample(linearSampler, input.vUv + float2(-o.x * 2.0, 0.0)).rgb;
    color += uSource.Sample(linearSampler, input.vUv + float2(o.x * 2.0, 0.0)).rgb;
    color += uSource.Sample(linearSampler, input.vUv + float2(0.0, -o.y * 2.0)).rgb;
    color += uSource.Sample(linearSampler, input.vUv + float2(0.0, o.y * 2.0)).rgb;
    color += uSource.Sample(linearSampler, input.vUv + float2(-o.x, -o.y)).rgb * 2.0;
    color += uSource.Sample(linearSampler, input.vUv + float2(o.x, -o.y)).rgb * 2.0;
    color += uSource.Sample(linearSampler, input.vUv + float2(-o.x, o.y)).rgb * 2.0;
    color += uSource.Sample(linearSampler, input.vUv + float2(o.x, o.y)).rgb * 2.0;
    return float4(color / 12.0, 1.0);
}
)hlsl";

// Final composite: shade dye, add bloom, apply grain and the reference's
// container edge fade, and write a premultiplied frame for the swap chain.
inline constexpr char kGlowFluidDisplayShader[] = R"hlsl(
cbuffer DisplayConstants : register(b0)
{
    float3 uColor;
    float3 uSecondary;
    float uIntensity;
    float uHotspot;
    float uGlow;
    float uGrain;
    float uTime;
    float uLight;
}

Texture2D uDye : register(t0);
Texture2D uBloom : register(t1);
SamplerState linearSampler : register(s0);

float hash(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float4 main(PSInput input) : SV_TARGET
{
    float3 dye = uDye.Sample(linearSampler, input.vUv).xyz;
    float3 light = shadeDye(dye, uColor, uSecondary, uIntensity, uHotspot);
    float3 bloom = uBloom.Sample(linearSampler, input.vUv).rgb * uGlow;
    float edge = smoothstep(0.0, 0.02, min(min(input.vUv.x, 1.0 - input.vUv.x), min(input.vUv.y, 1.0 - input.vUv.y)));
    float grain = 1.0 + (hash(input.position.xy + frac(uTime * 13.7) * 97.0) - 0.5) * uGrain * 2.0;
    if (uLight > 0.5)
    {
        float energy = max(dye.x, 0.0);
        float heat = clamp(dye.y, 0.0, energy);
        float gas = max(dye.z, 0.0);
        float fresh = heat / max(energy, 0.0001);
        float3 tint = lerp(uSecondary, uColor, fresh);
        float halo = dot(bloom, float3(0.2126, 0.7152, 0.0722));
        float density = 1.0 - exp(-((energy * 1.1 + gas * 0.8) * uIntensity + halo * 0.25));
        float tone = dot(tint, float3(0.2126, 0.7152, 0.0722));
        float3 ink = tint * min(1.0, 0.5 / max(tone, 0.001)) * lerp(0.95, 0.72, smoothstep(0.5, 3.0, energy));
        ink = lerp(ink, float3(1.0, 1.0, 1.0), clamp(heat * heat * 0.05 * uHotspot, 0.0, 0.45));
        density = clamp(density * grain, 0.0, 1.0) * edge;
        return float4(ink * density, density);
    }
    else
    {
        float3 color = 1.0 - exp(-(light + bloom) * grain);
        color *= edge;
        return float4(color, max(color.r, max(color.g, color.b)));
    }
}
)hlsl";

} // namespace bafx::windows
