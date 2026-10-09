#define CINTERFACE
#include <d3d12.h>
#include <dxgi1_6.h>
#include "core/graphics/installer.hpp"

namespace cmfm::graphics::detail {
namespace {
template<class T> constexpr std::uint32_t slots = sizeof(T) / sizeof(void*);
template<std::size_t N>
int version(const std::uint32_t (&counts)[N], std::uint32_t slot) noexcept {
    for (std::size_t i = 0; i < N; ++i) if (slot < counts[i]) return static_cast<int>(i);
    return -1;
}
}
int interface_version(std::uint32_t family, std::uint32_t slot) noexcept {
    if (slot < 3) return -1;
    static constexpr std::uint32_t factory[]{slots<IDXGIFactoryVtbl>, slots<IDXGIFactory1Vtbl>,
        slots<IDXGIFactory2Vtbl>, slots<IDXGIFactory3Vtbl>, slots<IDXGIFactory4Vtbl>,
        slots<IDXGIFactory5Vtbl>, slots<IDXGIFactory6Vtbl>, slots<IDXGIFactory7Vtbl>};
    static constexpr std::uint32_t swapchain[]{slots<IDXGISwapChainVtbl>, slots<IDXGISwapChain1Vtbl>,
        slots<IDXGISwapChain2Vtbl>, slots<IDXGISwapChain3Vtbl>, slots<IDXGISwapChain4Vtbl>};
    static constexpr std::uint32_t device[]{slots<ID3D12DeviceVtbl>, slots<ID3D12Device1Vtbl>,
        slots<ID3D12Device2Vtbl>, slots<ID3D12Device3Vtbl>, slots<ID3D12Device4Vtbl>,
        slots<ID3D12Device5Vtbl>, slots<ID3D12Device6Vtbl>, slots<ID3D12Device7Vtbl>,
        slots<ID3D12Device8Vtbl>, slots<ID3D12Device9Vtbl>, slots<ID3D12Device10Vtbl>,
        slots<ID3D12Device11Vtbl>, slots<ID3D12Device12Vtbl>, slots<ID3D12Device13Vtbl>,
        slots<ID3D12Device14Vtbl>};
    static constexpr std::uint32_t list[]{slots<ID3D12GraphicsCommandListVtbl>, slots<ID3D12GraphicsCommandList1Vtbl>,
        slots<ID3D12GraphicsCommandList2Vtbl>, slots<ID3D12GraphicsCommandList3Vtbl>,
        slots<ID3D12GraphicsCommandList4Vtbl>, slots<ID3D12GraphicsCommandList5Vtbl>,
        slots<ID3D12GraphicsCommandList6Vtbl>, slots<ID3D12GraphicsCommandList7Vtbl>,
        slots<ID3D12GraphicsCommandList8Vtbl>, slots<ID3D12GraphicsCommandList9Vtbl>,
        slots<ID3D12GraphicsCommandList10Vtbl>};
    static constexpr std::uint32_t queue[]{slots<ID3D12CommandQueueVtbl>};
    switch (family) {
    case CMFM_D3D12_FACTORY: return version(factory, slot);
    case CMFM_D3D12_SWAPCHAIN: return version(swapchain, slot);
    case CMFM_D3D12_DEVICE: return version(device, slot);
    case CMFM_D3D12_QUEUE: return version(queue, slot);
    case CMFM_D3D12_COMMAND_LIST: return version(list, slot);
    default: return -1;
    }
}
}
