#include "pch.h"
#include "DlssNr_PeripheralWarp.h"

#include <Config.h>
#include <Util.h>

#include "peripheral_warp/adapters/d3d12/d3d12_adapter.h"

#include <fstream>
#include <utility>
#include <vector>
#include <wrl/client.h>

namespace DlssNr::PeripheralWarp
{

using Microsoft::WRL::ComPtr;

namespace
{

struct State
{
    bool initialized = false;
    bool initFailed = false;
    pw::D3D12Adapter adapter;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    // RTV-only staging texture Unpack writes into; created directly in RENDER_TARGET state, so
    // the very first Unpack() call has nothing to transition it from.
    ComPtr<ID3D12Resource> unpackTarget;
    bool unpackTargetNeedsTransitionIn = false;
    // The adapter creates its packed color/depth/motion textures in D3D12_RESOURCE_STATE_COMMON
    // (confirmed from the SDK's own Initialize()); Pack() itself always leaves them in
    // NON_PIXEL_SHADER_RESOURCE afterward. This tracks which "before" state is true for the
    // next Pack()'s entry barrier -- false only until the very first successful Pack().
    bool packedNeedsTransitionIn = false;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    unsigned int nativeWidth = 0;
    unsigned int nativeHeight = 0;
    unsigned int workWidth = 0;
    unsigned int workHeight = 0;
    std::vector<char> vsBytes;
    std::vector<char> packBytes;
    std::vector<char> unpackBytes;

    // This frame's packed depth/motion guides from the last successful Pack(), consumed by the
    // matching Unpack() -- adapter-owned pointers, not our own refs; valid only within the same
    // frame's command list between those two calls. Null whenever no Pack() has succeeded since
    // the last Unpack() (or since init/shutdown), which is Unpack()'s guard against running with
    // stale or absent guides.
    ID3D12Resource* lastPackedDepth = nullptr;
    ID3D12Resource* lastPackedMotion = nullptr;
    pw::DepthConvention lastDepthConvention = pw::DepthConvention::Normal;

    // The CPU writes source descriptors and input constants at record time but the GPU reads them
    // when the list executes, possibly several frames later. Each frame therefore gets its own
    // pack set and its own unpack set out of a ring, never touching one still in flight.
    // Sets [0, kRing) pack, [kRing, 2*kRing) unpack.
    unsigned int frameCounter = 0;
    unsigned int lastRing = 0;
};

constexpr unsigned int kRing = 8;

State g_state;

bool LoadFile(const std::filesystem::path& path, std::vector<char>& out)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);

    if (!f)
        return false;

    const auto size = (size_t) f.tellg();

    if (size == 0)
        return false;

    out.resize(size);
    f.seekg(0);
    f.read(out.data(), (std::streamsize) size);
    return (bool) f;
}

void ShutdownInternal()
{
    if (!g_state.initialized)
        return;

    g_state.adapter.Shutdown();
    g_state.rtvHeap.Reset();
    g_state.unpackTarget.Reset();
    g_state.unpackTargetNeedsTransitionIn = false;
    g_state.packedNeedsTransitionIn = false;
    g_state.lastPackedDepth = nullptr;
    g_state.lastPackedMotion = nullptr;
    g_state.frameCounter = 0;
    g_state.initialized = false;
    g_state.nativeWidth = 0;
    g_state.nativeHeight = 0;
}

bool EnsureInitialized(ID3D12Device* device, DXGI_FORMAT colorFormat, unsigned int nativeWidth,
                       unsigned int nativeHeight)
{
    if (g_state.initialized && g_state.nativeWidth == nativeWidth && g_state.nativeHeight == nativeHeight &&
        g_state.format == colorFormat)
        return true;

    if (g_state.initialized)
        ShutdownInternal();

    if (g_state.initFailed)
        return false;

    auto& cfg = *Config::Instance();
    const auto dir = Util::DllPath().remove_filename() / "peripheral_warp";

    if (g_state.vsBytes.empty() &&
        (!LoadFile(dir / "fullscreen_vs.dxbc", g_state.vsBytes) ||
         !LoadFile(dir / "pack_ps.dxbc", g_state.packBytes) ||
         !LoadFile(dir / "unpack_ps.dxbc", g_state.unpackBytes)))
    {
        LOG_ERROR("DLSS-NR PeripheralWarp: could not load shader bytecode from {}", dir.string());
        g_state.initFailed = true;
        return false;
    }

    pw::ConfigV1 pwConfig = pw::DefaultConfigV1();
    pwConfig.mode = pw::WarpMode::Peripheral;
    pwConfig.xAxis.centerPercent = cfg.DlssNrPeripheralWarpCenterX.value_or_default();
    pwConfig.xAxis.workPercent = cfg.DlssNrPeripheralWarpWorkX.value_or_default();
    pwConfig.yAxis.centerPercent = cfg.DlssNrPeripheralWarpCenterY.value_or_default();
    pwConfig.yAxis.workPercent = cfg.DlssNrPeripheralWarpWorkY.value_or_default();

    if (pw::ValidateConfig(pwConfig) != pw::Status::Ok)
    {
        LOG_ERROR("DLSS-NR PeripheralWarp: invalid Center/Work configuration, leaving the model at "
                  "native resolution");
        g_state.initFailed = true;
        return false;
    }

    pw::LayoutV1 layout {};

    if (pw::BuildLayout(pwConfig, nativeWidth, nativeHeight, &layout) != pw::Status::Ok)
    {
        LOG_ERROR("DLSS-NR PeripheralWarp: could not build a layout for {}x{}", nativeWidth, nativeHeight);
        g_state.initFailed = true;
        return false;
    }

    // Depth/motion packed formats are declared even though only RecordUnpackColor() (colour-only)
    // is ever called: Initialize() takes one full format set regardless of which Record* variant
    // the caller later uses, so these mirror Pack's own formats rather than leaving them unknown.
    const pw::D3D12TargetFormats packFormats {
        colorFormat, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16_FLOAT };
    const pw::D3D12TargetFormats unpackFormats = packFormats;

    pw::ShaderSet shaders {};
    shaders.fullscreenVertex = { g_state.vsBytes.data(), g_state.vsBytes.size() };
    shaders.packPixel = { g_state.packBytes.data(), g_state.packBytes.size() };
    shaders.unpackPixel = { g_state.unpackBytes.data(), g_state.unpackBytes.size() };

    const auto initStatus = g_state.adapter.Initialize(device, layout, packFormats, unpackFormats, shaders,
                                                        /* framesInFlight */ 1, /* sourceSets */ 2 * kRing);

    if (initStatus != pw::AdapterStatus::Ok)
    {
        LOG_ERROR("DLSS-NR PeripheralWarp: adapter init failed ({})", pw::AdapterStatusString(initStatus));
        g_state.initFailed = true;
        return false;
    }

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = nativeWidth;
    desc.Height = nativeHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = colorFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                               IID_PPV_ARGS(&g_state.unpackTarget))))
    {
        LOG_ERROR("DLSS-NR PeripheralWarp: could not allocate the Unpack staging target");
        g_state.adapter.Shutdown();
        g_state.initFailed = true;
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;

    if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_state.rtvHeap))))
    {
        LOG_ERROR("DLSS-NR PeripheralWarp: could not allocate the RTV descriptor heap");
        g_state.unpackTarget.Reset();
        g_state.adapter.Shutdown();
        g_state.initFailed = true;
        return false;
    }

    device->CreateRenderTargetView(g_state.unpackTarget.Get(), nullptr,
                                   g_state.rtvHeap->GetCPUDescriptorHandleForHeapStart());

    g_state.nativeWidth = nativeWidth;
    g_state.nativeHeight = nativeHeight;
    g_state.workWidth = layout.workWidth;
    g_state.workHeight = layout.workHeight;
    g_state.format = colorFormat;
    g_state.unpackTargetNeedsTransitionIn = false;
    g_state.initialized = true;
    g_state.initFailed = false;

    LOG_INFO("DLSS-NR PeripheralWarp: {}x{} native -> {}x{} work ({:.1f}% of the pixels)", nativeWidth,
             nativeHeight, layout.workWidth, layout.workHeight,
             100.0f * (float) (layout.workWidth * layout.workHeight) / (float) (nativeWidth * nativeHeight));

    return true;
}

} // namespace

bool Enabled() { return Config::Instance()->DlssNrPeripheralWarpEnabled.value_or_default(); }

void Shutdown() { ShutdownInternal(); }

bool Pack(
    ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* color, DXGI_FORMAT colorFormat, unsigned int colorWidth, unsigned int colorHeight,
    ID3D12Resource* depth, DXGI_FORMAT depthFormat,
    unsigned int depthBaseX, unsigned int depthBaseY, unsigned int depthWidth, unsigned int depthHeight,
    ID3D12Resource* motion, DXGI_FORMAT motionFormat,
    unsigned int motionBaseX, unsigned int motionBaseY, unsigned int motionWidth, unsigned int motionHeight,
    float motionScaleX, float motionScaleY, bool depthInverted,
    ID3D12Resource** outColor, ID3D12Resource** outDepth, ID3D12Resource** outMotion,
    unsigned int* outWorkWidth, unsigned int* outWorkHeight) noexcept
{
    // A fresh Pack() invalidates any leftover guides from a previous frame immediately: if this
    // call fails or is never reached again this frame, Unpack() must refuse to run rather than
    // silently reuse stale data.
    g_state.lastPackedDepth = nullptr;
    g_state.lastPackedMotion = nullptr;

    if (device == nullptr || cmdList == nullptr || color == nullptr || depth == nullptr || motion == nullptr ||
        outColor == nullptr || outDepth == nullptr || outMotion == nullptr || outWorkWidth == nullptr ||
        outWorkHeight == nullptr)
        return false;

    if (!EnsureInitialized(device, colorFormat, colorWidth, colorHeight))
        return false;

    pw::InputDescriptionV2 description = pw::DefaultInputDescriptionV2(colorWidth, colorHeight);
    description.colorRect = { 0, 0, colorWidth, colorHeight };
    description.depthRect = { depthBaseX, depthBaseY, depthWidth, depthHeight };
    description.motionRect = { motionBaseX, motionBaseY, motionWidth, motionHeight };
    description.confidenceRect = { 0, 0, 0, 0 };
    description.motionScaleX = motionScaleX;
    description.motionScaleY = motionScaleY;
    description.motionDirection = pw::MotionDirection::CurrentToPrevious;
    description.depthConvention = depthInverted ? pw::DepthConvention::Reversed : pw::DepthConvention::Normal;
    description.colorEncoding = pw::ColorEncoding::LinearHdr;
    description.flags = pw::InputFlagNone;

    if (pw::ValidateInputDescriptionV2(description) != pw::Status::Ok)
        return false;

    pw::D3D12SourceResources sources {};
    sources.color = { color, colorFormat };
    sources.depth = { depth, depthFormat };
    sources.motion = { motion, motionFormat };
    sources.confidence = { nullptr, DXGI_FORMAT_UNKNOWN };

    // color/depth/motion arrive in NON_PIXEL_SHADER_RESOURCE (what this fork's compute path and
    // the model read them in). Pack's draw is a pixel shader, which may only read
    // PIXEL_SHADER_RESOURCE, so they are switched for the draw and restored right after.
    const unsigned int ring = g_state.frameCounter++ % kRing;

    if (g_state.adapter.WriteSourceSetV2(ring, sources, description) != pw::AdapterStatus::Ok)
        return false;

    const auto packed = g_state.adapter.PackedViews(0);

    if (packed.resources.color.resource == nullptr || packed.resources.depth.resource == nullptr ||
        packed.resources.motion.resource == nullptr)
        return false;

    // RecordPack() draws into these as render targets; it never transitions their resource state
    // itself (the same "caller owns every barrier" contract documented for every Record* call in
    // this SDK), so this function must bring them from wherever the *previous* call left them (or
    // from their as-created state, the first time) to RENDER_TARGET, then back out to
    // NON_PIXEL_SHADER_RESOURCE -- the state the model's own evaluate (and, for depth/motion, the
    // matching Unpack() call) expects to read them in.
    D3D12_RESOURCE_BARRIER toRt[3] {};
    for (auto& b : toRt)
    {
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = g_state.packedNeedsTransitionIn
                                        ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                        : D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    toRt[0].Transition.pResource = packed.resources.color.resource;
    toRt[1].Transition.pResource = packed.resources.depth.resource;
    toRt[2].Transition.pResource = packed.resources.motion.resource;
    cmdList->ResourceBarrier(3, toRt);

    D3D12_RESOURCE_BARRIER srcToPixel[3] {};
    for (auto& b : srcToPixel)
    {
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    srcToPixel[0].Transition.pResource = color;
    srcToPixel[1].Transition.pResource = depth;
    srcToPixel[2].Transition.pResource = motion;
    cmdList->ResourceBarrier(3, srcToPixel);

    const auto packStatus = g_state.adapter.RecordPackFromSet(cmdList, 0, ring);

    D3D12_RESOURCE_BARRIER srcBack[3] { srcToPixel[0], srcToPixel[1], srcToPixel[2] };
    for (auto& b : srcBack)
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    cmdList->ResourceBarrier(3, srcBack);

    D3D12_RESOURCE_BARRIER toSrv[3] { toRt[0], toRt[1], toRt[2] };
    for (auto& b : toSrv)
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    cmdList->ResourceBarrier(3, toSrv);
    g_state.packedNeedsTransitionIn = true;

    if (packStatus != pw::AdapterStatus::Ok)
        return false;

    g_state.lastPackedDepth = packed.resources.depth.resource;
    g_state.lastPackedMotion = packed.resources.motion.resource;
    g_state.lastDepthConvention = description.depthConvention;
    g_state.lastRing = ring;

    *outColor = packed.resources.color.resource;
    *outDepth = packed.resources.depth.resource;
    *outMotion = packed.resources.motion.resource;
    *outWorkWidth = g_state.workWidth;
    *outWorkHeight = g_state.workHeight;

    return true;
}

bool Unpack(
    ID3D12Device*, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* modelOutput,
    DXGI_FORMAT modelOutputFormat, ID3D12Resource* nativeTarget, unsigned int nativeWidth,
    unsigned int nativeHeight, DXGI_FORMAT nativeFormat) noexcept
{
    if (!g_state.initialized || cmdList == nullptr || modelOutput == nullptr || nativeTarget == nullptr ||
        g_state.lastPackedDepth == nullptr || g_state.lastPackedMotion == nullptr)
        return false;

    if (nativeWidth != g_state.nativeWidth || nativeHeight != g_state.nativeHeight ||
        nativeFormat != g_state.format)
        return false;

    ID3D12Resource* const packedDepth = g_state.lastPackedDepth;
    ID3D12Resource* const packedMotion = g_state.lastPackedMotion;
    // One Unpack() per Pack(): whatever happens below, this frame's guides are spent -- a second
    // call without an intervening Pack() must fail via the guard above, not reuse these.
    g_state.lastPackedDepth = nullptr;
    g_state.lastPackedMotion = nullptr;

    // The unpack draw reads the model's answer as though it were "color": the adapter has no
    // separate notion of a model-output slot, so this re-registers sourceSet 0 -- previously
    // holding Pack()'s native-extent inputs, now spent -- with the model's work-extent output
    // plus this frame's packed depth/motion guides. RecordUnpackColorFromSet() specifically
    // requires the registered extent to be Work, not Native, which is what makes this a distinct
    // registration from Pack()'s rather than something Pack() could have set up in advance.
    pw::InputDescriptionV2 unpackInput = pw::DefaultInputDescriptionV2(g_state.workWidth, g_state.workHeight);
    unpackInput.colorRect = { 0, 0, g_state.workWidth, g_state.workHeight };
    unpackInput.depthRect = { 0, 0, g_state.workWidth, g_state.workHeight };
    unpackInput.motionRect = { 0, 0, g_state.workWidth, g_state.workHeight };
    unpackInput.confidenceRect = { 0, 0, 0, 0 };
    unpackInput.motionScaleX = 1.0f;
    unpackInput.motionScaleY = 1.0f;
    unpackInput.motionDirection = pw::MotionDirection::CurrentToPrevious;
    unpackInput.depthConvention = g_state.lastDepthConvention;
    unpackInput.colorEncoding = pw::ColorEncoding::LinearHdr;
    unpackInput.flags = pw::InputFlagNone;

    if (pw::ValidateInputDescriptionV2(unpackInput) != pw::Status::Ok)
        return false;

    // modelOutput and this frame's packed depth/motion arrive SRV-readable for compute
    // (NON_PIXEL_SHADER_RESOURCE, matching the caller's MakeModelReadable()); the unpack draw is
    // a pixel shader, so bring all three to PIXEL_SHADER_RESOURCE for the duration of this call
    // and hand them back exactly as received, regardless of how this function returns.
    D3D12_RESOURCE_BARRIER toPixel[3] {};
    for (auto& b : toPixel)
    {
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    toPixel[0].Transition.pResource = modelOutput;
    toPixel[1].Transition.pResource = packedDepth;
    toPixel[2].Transition.pResource = packedMotion;
    cmdList->ResourceBarrier(3, toPixel);

    const auto restoreToNonPixel = [&]() {
        D3D12_RESOURCE_BARRIER back[3] { toPixel[0], toPixel[1], toPixel[2] };
        for (auto& b : back)
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cmdList->ResourceBarrier(3, back);
    };

    pw::D3D12SourceResources workSources {};
    workSources.color = { modelOutput, modelOutputFormat };
    workSources.depth = { packedDepth, DXGI_FORMAT_R32_FLOAT };
    workSources.motion = { packedMotion, DXGI_FORMAT_R16G16_FLOAT };
    workSources.confidence = { nullptr, DXGI_FORMAT_UNKNOWN };

    const unsigned int unpackSet = kRing + g_state.lastRing;

    if (g_state.adapter.WriteSourceSetV2(unpackSet, workSources, unpackInput) != pw::AdapterStatus::Ok)
    {
        restoreToNonPixel();
        return false;
    }

    if (g_state.unpackTargetNeedsTransitionIn)
    {
        D3D12_RESOURCE_BARRIER toRtv {};
        toRtv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRtv.Transition.pResource = g_state.unpackTarget.Get();
        toRtv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toRtv.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        toRtv.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmdList->ResourceBarrier(1, &toRtv);
    }

    const auto rtv = g_state.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const auto unpackStatus = g_state.adapter.RecordUnpackColorFromSet(cmdList, 0, unpackSet, rtv, pw::DiagnosticOutlineNone);

    restoreToNonPixel();

    if (unpackStatus != pw::AdapterStatus::Ok)
        return false;

    D3D12_RESOURCE_BARRIER toSrv {};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition.pResource = g_state.unpackTarget.Get();
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cmdList->ResourceBarrier(1, &toSrv);
    g_state.unpackTargetNeedsTransitionIn = true;

    // nativeTarget arrives, and must leave, in UNORDERED_ACCESS: the caller's own
    // MakeModelWritable() already put it there before calling here. Bracket the copy with its
    // own explicit transitions on both resources rather than assuming any state beyond what the
    // header documents.
    D3D12_RESOURCE_BARRIER pre[2] {};
    pre[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre[0].Transition.pResource = nativeTarget;
    pre[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    pre[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    pre[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    pre[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre[1].Transition.pResource = g_state.unpackTarget.Get();
    pre[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    pre[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    pre[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmdList->ResourceBarrier(2, pre);

    cmdList->CopyResource(nativeTarget, g_state.unpackTarget.Get());

    D3D12_RESOURCE_BARRIER post[2] { pre[0], pre[1] };
    std::swap(post[0].Transition.StateBefore, post[0].Transition.StateAfter);
    std::swap(post[1].Transition.StateBefore, post[1].Transition.StateAfter);
    cmdList->ResourceBarrier(2, post);

    return true;
}

} // namespace DlssNr::PeripheralWarp
