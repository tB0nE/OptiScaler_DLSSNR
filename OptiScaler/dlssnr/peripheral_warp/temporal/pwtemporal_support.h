#pragma once

// The handful of D3D12 helpers the vendored temporal machine (ngx_temporal.*, from
// BeliyG3/optimizer-fps-dlss5, MIT) takes from that project's NGX layer, reimplemented here so the
// machine can be used without the rest of it. Bodies match upstream's ngx_common.cpp.

#include <d3d12.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace pwngx {

struct Disposable {
    virtual ~Disposable() = default;
};

struct Shaders {
    std::vector<char> vertex;
    std::vector<char> temporalResidual, temporalAccumulate, temporalReproject;
    std::vector<char> temporalDownsample; // optional: without it the hole fill is off
    std::vector<char> temporalCompose;    // optional: without it the guided smoothing is off
    bool TemporalLoaded() const { return !temporalResidual.empty() && !temporalAccumulate.empty() && !temporalReproject.empty(); }
};

inline void Barrier(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res, D3D12_RESOURCE_STATES &tracked, D3D12_RESOURCE_STATES to)
{
    if (res == nullptr || tracked == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = tracked;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
    tracked = to;
}

inline void BarrierExternal(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to,
                            UINT subresource)
{
    if (res == nullptr || from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = subresource;
    cmd->ResourceBarrier(1, &b);
}

inline void BarrierExternal(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    BarrierExternal(cmd, res, from, to, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
}

inline bool CreateTexture(ID3D12Device *device, UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, ID3D12Resource **out)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(out)));
}

} // namespace pwngx
