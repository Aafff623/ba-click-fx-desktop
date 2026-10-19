#include "test_support.hpp"

#include "glow_fluid_shaders.hpp"

#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using Microsoft::WRL::ComPtr;
using namespace bafx::windows;

namespace
{

struct ShaderEntry
{
    std::string_view source{};
    const char* entryPoint{nullptr};
    const char* profile{nullptr};
};

[[nodiscard]] ComPtr<ID3DBlob> compileShader(const ShaderEntry& entry)
{
    ComPtr<ID3DBlob> byteCode;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        entry.source.data(),
        entry.source.size(),
        "glow_fluid_shaders.hpp",
        nullptr,
        nullptr,
        entry.entryPoint,
        entry.profile,
        D3DCOMPILE_ENABLE_STRICTNESS
            | D3DCOMPILE_WARNINGS_ARE_ERRORS
            | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &byteCode,
        &errors);
    if (FAILED(result))
    {
        std::string message = "D3DCompile failed for ";
        message += entry.entryPoint;
        if (errors != nullptr && errors->GetBufferPointer() != nullptr)
        {
            message += ": ";
            message.append(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        throw std::runtime_error(message);
    }
    return byteCode;
}

[[nodiscard]] ComPtr<ID3D11ShaderReflection> reflectShader(ID3DBlob* byteCode)
{
    ComPtr<ID3D11ShaderReflection> reflection;
    const HRESULT result = D3DReflect(
        byteCode->GetBufferPointer(),
        byteCode->GetBufferSize(),
        __uuidof(ID3D11ShaderReflection),
        reinterpret_cast<void**>(reflection.GetAddressOf()));
    if (FAILED(result))
    {
        throw std::runtime_error("D3DReflect failed");
    }
    return reflection;
}

[[nodiscard]] bool hasBinding(
    ID3D11ShaderReflection* reflection,
    const char* name,
    const D3D_SHADER_INPUT_TYPE type,
    const UINT bindPoint)
{
    D3D11_SHADER_INPUT_BIND_DESC description{};
    return SUCCEEDED(reflection->GetResourceBindingDescByName(name, &description))
        && description.Type == type
        && description.BindPoint == bindPoint
        && description.BindCount == 1U;
}

// Reads a uniform's byte offset inside its constant buffer so the CPU-side
// mirror structs in glow_fluid_pipeline.cpp stay pinned to the HLSL layout.
[[nodiscard]] UINT variableOffset(
    ID3D11ShaderReflection* reflection,
    const char* bufferName,
    const char* variableName)
{
    ID3D11ShaderReflectionConstantBuffer* buffer =
        reflection->GetConstantBufferByName(bufferName);
    if (buffer == nullptr)
    {
        throw std::runtime_error(
            std::string("missing constant buffer ") + bufferName);
    }
    ID3D11ShaderReflectionVariable* variable =
        buffer->GetVariableByName(variableName);
    if (variable == nullptr)
    {
        throw std::runtime_error(
            std::string("missing uniform ") + bufferName + "." + variableName);
    }
    D3D11_SHADER_VARIABLE_DESC description{};
    if (FAILED(variable->GetDesc(&description)))
    {
        throw std::runtime_error("GetDesc(variable) failed");
    }
    return description.StartOffset;
}

[[nodiscard]] bool hasSignatureParameter(
    ID3D11ShaderReflection* reflection,
    const bool output,
    const std::string_view semantic,
    const UINT semanticIndex)
{
    D3D11_SHADER_DESC shaderDescription{};
    if (FAILED(reflection->GetDesc(&shaderDescription)))
    {
        return false;
    }
    const UINT count = output
        ? shaderDescription.OutputParameters
        : shaderDescription.InputParameters;
    for (UINT index = 0U; index < count; ++index)
    {
        D3D11_SIGNATURE_PARAMETER_DESC parameter{};
        const HRESULT result = output
            ? reflection->GetOutputParameterDesc(index, &parameter)
            : reflection->GetInputParameterDesc(index, &parameter);
        if (SUCCEEDED(result)
            && parameter.SemanticName != nullptr
            && std::string_view(parameter.SemanticName) == semantic
            && parameter.SemanticIndex == semanticIndex)
        {
            return true;
        }
    }
    return false;
}

// Compiles every pass with warnings-as-errors and asserts the texture,
// sampler and constant-buffer contract the pipeline binds against.
void verifyPasses()
{
    struct PassContract
    {
        std::string source{};
        const char* name{nullptr};
        std::array<std::pair<const char*, UINT>, 4> textures{};
        // Sampler registers the pass declares; the pipeline binds one of its
        // two sampler states to each slot before drawing.
        bool linearAtZero{false};
        bool pointAtZero{false};
        bool linearAtOne{false};
    };
    const std::string header{kGlowFluidPixelHeader};
    const std::string shade{kGlowFluidShadeChunk};
    // Every pass is compiled with the shared PSInput header; the two shading
    // passes additionally carry the shared shade chunk.
    const auto passSource = [&header, &shade](
                                std::string_view source,
                                const bool shaded)
    {
        return header + (shaded ? shade : std::string{})
            + std::string{source};
    };
    const std::array<PassContract, 11> passes{{
        PassContract{
            passSource(kGlowFluidSplatShader, false),
            "splat",
            {{{"uTarget", 0}, {nullptr, 0}, {nullptr, 0}, {nullptr, 0}}},
            true,
            false,
            false},
        PassContract{
            passSource(kGlowFluidAdvectShader, false),
            "advect",
            {{{"uVelocity", 0}, {"uSource", 1}, {nullptr, 0}, {nullptr, 0}}},
            true,
            false,
            false},
        PassContract{
            passSource(kGlowFluidMaccormackShader, false),
            "maccormack",
            {{{"uVelocity", 0},
              {"uSource", 1},
              {"uForward", 2},
              {"uBack", 3}}},
            true,
            false,
            false},
        PassContract{
            passSource(kGlowFluidDivergenceShader, false),
            "divergence",
            {{{"uVelocity", 0}, {nullptr, 0}, {nullptr, 0}, {nullptr, 0}}},
            true,
            false,
            false},
        PassContract{
            passSource(kGlowFluidScaleShader, false),
            "scale",
            {{{"uSource", 0}, {nullptr, 0}, {nullptr, 0}, {nullptr, 0}}},
            false,
            true,
            false},
        PassContract{
            passSource(kGlowFluidPressureShader, false),
            "pressure",
            {{{"uPressure", 0}, {"uDivergence", 1}, {nullptr, 0}, {nullptr, 0}}},
            false,
            true,
            false},
        PassContract{
            passSource(kGlowFluidGradientShader, false),
            "gradient",
            {{{"uPressure", 0}, {"uVelocity", 1}, {nullptr, 0}, {nullptr, 0}}},
            false,
            true,
            true},
        PassContract{
            passSource(kGlowFluidPrefilterShader, true),
            "prefilter",
            {{{"uDye", 0}, {nullptr, 0}, {nullptr, 0}, {nullptr, 0}}},
            true,
            false,
            false},
        PassContract{
            passSource(kGlowFluidDownShader, false),
            "down",
            {{{"uSource", 0}, {nullptr, 0}, {nullptr, 0}, {nullptr, 0}}},
            true,
            false,
            false},
        PassContract{
            passSource(kGlowFluidUpShader, false),
            "up",
            {{{"uSource", 0}, {nullptr, 0}, {nullptr, 0}, {nullptr, 0}}},
            true,
            false,
            false},
        PassContract{
            passSource(kGlowFluidDisplayShader, true),
            "display",
            {{{"uDye", 0}, {"uBloom", 1}, {nullptr, 0}, {nullptr, 0}}},
            true,
            false,
            false},
    }};

    for (const PassContract& pass : passes)
    {
        const ComPtr<ID3D11ShaderReflection> reflection =
            reflectShader(
                compileShader(ShaderEntry{pass.source, "main", "ps_5_0"})
                    .Get());
        for (const auto& texture : pass.textures)
        {
            if (texture.first == nullptr)
            {
                continue;
            }
            BAFX_CHECK(hasBinding(
                reflection.Get(),
                texture.first,
                D3D_SIT_TEXTURE,
                texture.second));
        }
        if (pass.linearAtZero)
        {
            BAFX_CHECK(hasBinding(
                reflection.Get(),
                "linearSampler",
                D3D_SIT_SAMPLER,
                0U));
        }
        if (pass.pointAtZero)
        {
            BAFX_CHECK(hasBinding(
                reflection.Get(),
                "pointSampler",
                D3D_SIT_SAMPLER,
                0U));
        }
        if (pass.linearAtOne)
        {
            BAFX_CHECK(hasBinding(
                reflection.Get(),
                "linearSampler",
                D3D_SIT_SAMPLER,
                1U));
        }
    }
}

// Compiles one pass (with the shared pixel header, plus the shade chunk when
// shaded) and reflects it.
[[nodiscard]] ComPtr<ID3D11ShaderReflection> compilePass(
    const std::string_view source,
    const bool shaded = false)
{
    const std::string combined{
        std::string{kGlowFluidPixelHeader}
        + (shaded ? std::string{kGlowFluidShadeChunk} : std::string{})
        + std::string{source}};
    return reflectShader(compileShader(ShaderEntry{combined, "main", "ps_5_0"}).Get());
}

} // namespace

BAFX_TEST(glow_fluid_vertex_shader_emits_fullscreen_triangle_inputs)
{
    const ComPtr<ID3DBlob> byteCode = compileShader(ShaderEntry{
        kGlowFluidVertexShader, "main", "vs_5_0"});
    const ComPtr<ID3D11ShaderReflection> reflection =
        reflectShader(byteCode.Get());

    // The stage consumes exactly one float2 POSITION per vertex (the shared
    // fullscreen triangle); no other attributes are bound.
    BAFX_CHECK(hasSignatureParameter(reflection.Get(), false, "POSITION", 0U));
    BAFX_CHECK(hasSignatureParameter(reflection.Get(), true, "SV_POSITION", 0U));
    BAFX_CHECK(hasSignatureParameter(reflection.Get(), true, "TEXCOORD", 0U));
    BAFX_CHECK(hasSignatureParameter(reflection.Get(), true, "TEXCOORD", 1U));
    BAFX_CHECK(hasSignatureParameter(reflection.Get(), true, "TEXCOORD", 2U));
    BAFX_CHECK(hasSignatureParameter(reflection.Get(), true, "TEXCOORD", 3U));
    BAFX_CHECK(hasSignatureParameter(reflection.Get(), true, "TEXCOORD", 4U));
    BAFX_CHECK(hasBinding(
        reflection.Get(), "Transform", D3D_SIT_CBUFFER, 0U));
}

BAFX_TEST(glow_fluid_all_passes_compile_and_expose_expected_bindings)
{
    verifyPasses();
}

BAFX_TEST(glow_fluid_splat_constants_match_cpu_mirror_layout)
{
    const ComPtr<ID3D11ShaderReflection> reflection =
        compilePass(kGlowFluidSplatShader);
    // HLSL packing: values pack tightly at 4-byte alignment and only move to
    // the next 16-byte row when they would cross one, so float3 uValue skips
    // from 28 to 32.
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(reflection.Get(), "SplatConstants", "uAspect")),
        0.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(reflection.Get(), "SplatConstants", "uA")),
        4.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(reflection.Get(), "SplatConstants", "uB")),
        16.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(reflection.Get(), "SplatConstants", "uRadius")),
        24.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(reflection.Get(), "SplatConstants", "uWide")),
        28.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(reflection.Get(), "SplatConstants", "uValue")),
        32.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(reflection.Get(), "SplatConstants", "uMode")),
        44.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(reflection.Get(), "SplatConstants", "uStrength")),
        48.0F, 0.5F);
}

BAFX_TEST(glow_fluid_advect_and_maccormack_constants_match_cpu_mirror_layout)
{
    const ComPtr<ID3D11ShaderReflection> advectReflection =
        compilePass(kGlowFluidAdvectShader);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(advectReflection.Get(), "AdvectConstants", "uVelocityTexel")),
        0.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(advectReflection.Get(), "AdvectConstants", "uDt")),
        8.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(advectReflection.Get(), "AdvectConstants", "uDecay")),
        16.0F, 0.5F);

    const ComPtr<ID3D11ShaderReflection> maccormackReflection =
        compilePass(kGlowFluidMaccormackShader);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(maccormackReflection.Get(), "MaccormackConstants", "uVelocityTexel")),
        0.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(maccormackReflection.Get(), "MaccormackConstants", "uSourceTexel")),
        8.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(maccormackReflection.Get(), "MaccormackConstants", "uDt")),
        16.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(maccormackReflection.Get(), "MaccormackConstants", "uDecay")),
        32.0F, 0.5F);
}

BAFX_TEST(glow_fluid_display_and_bloom_constants_match_cpu_mirror_layout)
{
    const ComPtr<ID3D11ShaderReflection> prefilterReflection =
        compilePass(kGlowFluidPrefilterShader, true);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(prefilterReflection.Get(), "PrefilterConstants", "uDyeTexel")),
        0.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(prefilterReflection.Get(), "PrefilterConstants", "uColor")),
        16.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(prefilterReflection.Get(), "PrefilterConstants", "uSecondary")),
        32.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(prefilterReflection.Get(), "PrefilterConstants", "uIntensity")),
        44.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(prefilterReflection.Get(), "PrefilterConstants", "uHotspot")),
        48.0F, 0.5F);

    const ComPtr<ID3D11ShaderReflection> displayReflection =
        compilePass(kGlowFluidDisplayShader, true);
    // Scalars pack tightly after a float3 as long as they stay inside one
    // 16-byte row, so uIntensity follows uSecondary at 28 rather than 32.
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(displayReflection.Get(), "DisplayConstants", "uColor")),
        0.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(displayReflection.Get(), "DisplayConstants", "uSecondary")),
        16.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(displayReflection.Get(), "DisplayConstants", "uIntensity")),
        28.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(displayReflection.Get(), "DisplayConstants", "uHotspot")),
        32.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(displayReflection.Get(), "DisplayConstants", "uGlow")),
        36.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(displayReflection.Get(), "DisplayConstants", "uGrain")),
        40.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(displayReflection.Get(), "DisplayConstants", "uTime")),
        44.0F, 0.5F);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(displayReflection.Get(), "DisplayConstants", "uLight")),
        48.0F, 0.5F);
    // The physical-pixel edge fade width replaced the reference's fixed 2%
    // container fade.
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(displayReflection.Get(), "DisplayConstants", "uEdgeWidth")),
        52.0F, 0.5F);

    const ComPtr<ID3D11ShaderReflection> downReflection =
        compilePass(kGlowFluidDownShader);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(downReflection.Get(), "DownConstants", "uSourceTexel")),
        0.0F, 0.5F);

    const ComPtr<ID3D11ShaderReflection> upReflection =
        compilePass(kGlowFluidUpShader);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(upReflection.Get(), "UpConstants", "uSourceTexel")),
        0.0F, 0.5F);

    const ComPtr<ID3D11ShaderReflection> scaleReflection =
        compilePass(kGlowFluidScaleShader);
    BAFX_CHECK_NEAR(
        static_cast<float>(variableOffset(scaleReflection.Get(), "ScaleConstants", "uValue")),
        0.0F, 0.5F);
}
