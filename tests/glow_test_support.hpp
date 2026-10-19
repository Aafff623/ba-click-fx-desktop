#pragma once

#include "test_support.hpp"

#include "bafx/windows/error.hpp"
#include "bafx/windows/gpu_texture_readback.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace bafx::test
{

struct WarpDevice
{
    ComPtr<ID3D11Device> device{};
    ComPtr<ID3D11DeviceContext> context{};
};

// Creates a WARP device, preferring the D3D11 Debug Layer (Graphics Tools)
// so SRV/RTV conflicts and resource hazards surface as test failures, and
// falling back when the feature is not installed.
[[nodiscard]] inline WarpDevice createWarpDevice()
{
    constexpr std::array featureLevels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    WarpDevice graphics{};
    const HRESULT debugResult = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG,
        featureLevels.data(),
        static_cast<UINT>(featureLevels.size()),
        D3D11_SDK_VERSION,
        &graphics.device,
        nullptr,
        &graphics.context);
    if (SUCCEEDED(debugResult))
    {
        return graphics;
    }
    bafx::windows::throwIfFailed(
        D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels.data(),
            static_cast<UINT>(featureLevels.size()),
            D3D11_SDK_VERSION,
            &graphics.device,
            nullptr,
            &graphics.context),
        "D3D11CreateDevice(WARP)");
    return graphics;
}

struct DisplayTarget
{
    ComPtr<ID3D11Texture2D> texture{};
    ComPtr<ID3D11RenderTargetView> view{};
};

// Matches the production FP16 swap chain so display output reads back the
// same way the compositor sees it.
[[nodiscard]] inline DisplayTarget createDisplayTarget(
    ID3D11Device* device,
    std::uint32_t width,
    std::uint32_t height)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1U;
    description.ArraySize = 1U;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.SampleDesc.Count = 1U;
    description.SampleDesc.Quality = 0U;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    DisplayTarget target{};
    bafx::windows::throwIfFailed(
        device->CreateTexture2D(&description, nullptr, &target.texture),
        "CreateTexture2D(glow display target)");
    bafx::windows::throwIfFailed(
        device->CreateRenderTargetView(
            target.texture.Get(), nullptr, &target.view),
        "CreateRenderTargetView(glow display target)");
    return target;
}

struct FramePixel
{
    float red{0.0F};
    float green{0.0F};
    float blue{0.0F};
    float alpha{0.0F};
};

[[nodiscard]] inline FramePixel readPixel(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture,
    std::uint32_t x,
    std::uint32_t y)
{
    const bafx::windows::Rgba16FloatImage raw =
        bafx::windows::readbackRgba16FloatTexture(
            context,
            texture,
            bafx::windows::TextureReadbackRegion{x, y, 1U, 1U});
    if (raw.pixels.empty())
    {
        throw std::runtime_error("pixel readback returned no data");
    }
    return FramePixel{
        bafx::windows::halfToFloat(raw.pixels.front().red),
        bafx::windows::halfToFloat(raw.pixels.front().green),
        bafx::windows::halfToFloat(raw.pixels.front().blue),
        bafx::windows::halfToFloat(raw.pixels.front().alpha)};
}

} // namespace bafx::test
