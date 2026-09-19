#include "pch.h"
#include "DlssNr_PeripheralWarp.h"

#include <Config.h>
#include <Util.h>

#include "peripheral_warp/adapters/d3d12/d3d12_adapter.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
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

    // The Center/Work percentages the current adapter was built with (after clamping).
    bool haveCfg = false;
    float cfgCenterX = 0.0f;
    float cfgCenterY = 0.0f;
    float cfgWorkX = 0.0f;
    float cfgWorkY = 0.0f;
};

// A replaced adapter is kept alive for a while rather than destroyed: the GPU can run several
// frames behind the CPU on the game's own queue, and freeing resources under in-flight work is
// the device hang this codebase already documents for the model's own scratch surfaces.
struct Retired
{
    pw::D3D12Adapter adapter;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12Resource> unpackTarget;
    int framesLeft = 64;
};

std::vector<Retired> g_retired;

struct Wanted
{
    float centerX, centerY, workX, workY;
};

// Any ini or slider value becomes a layout the SDK accepts: 0 < Center <= Work <= 100 and
// Work >= (100 + Center) / 2 per axis.
Wanted ReadWanted()
{
    const auto& cfg = *Config::Instance();
    const auto axis = [](float center, float work, float& c, float& w)
    {
        c = std::clamp(std::isfinite(center) ? center : 80.0f, 10.0f, 100.0f);
        w = std::clamp(std::isfinite(work) ? work : 90.0f, 10.0f, 100.0f);
        w = std::min(100.0f, std::max(w, (100.0f + c) * 0.5f + 0.01f));
    };

    Wanted r {};
    axis(cfg.DlssNrPeripheralWarpCenterX.value_or_default(), cfg.DlssNrPeripheralWarpWorkX.value_or_default(),
         r.centerX, r.workX);
    axis(cfg.DlssNrPeripheralWarpCenterY.value_or_default(), cfg.DlssNrPeripheralWarpWorkY.value_or_default(),
         r.centerY, r.workY);
    return r;
}

constexpr unsigned int kRing = 8;

// Every fallback to the unwarped model is otherwise silent, so log each distinct reason once.
void LogFallback(const char* stage, const char* detail, unsigned int colorFormat, unsigned int depthFormat,
                 unsigned int motionFormat, unsigned int colorW, unsigned int colorH, unsigned int depthW,
                 unsigned int depthH, unsigned int motionW, unsigned int motionH)
{
    static std::string last;
    const std::string now = std::string(stage) + "|" + detail + "|" + std::to_string(colorFormat) + "|" +
                            std::to_string(depthFormat) + "|" + std::to_string(motionFormat) + "|" +
                            std::to_string(colorW) + "x" + std::to_string(colorH) + "|" +
                            std::to_string(depthW) + "x" + std::to_string(depthH) + "|" +
                            std::to_string(motionW) + "x" + std::to_string(motionH);

    if (now == last)
        return;

    last = now;
    LOG_WARN("DLSS-NR PeripheralWarp: falling back to the unwarped model -- {} ({}); colour fmt {} {}x{}, "
             "depth fmt {} {}x{}, motion fmt {} {}x{}",
             stage, detail, colorFormat, colorW, colorH, depthFormat, depthW, depthH, motionFormat, motionW,
             motionH);
}

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

void ShutdownInternal(bool retire)
{
    if (!g_state.initialized)
        return;

    if (retire)
    {
        Retired old;
        old.adapter = std::move(g_state.adapter);
        old.rtvHeap = std::move(g_state.rtvHeap);
        old.unpackTarget = std::move(g_state.unpackTarget);
        g_retired.push_back(std::move(old));
    }
    else
    {
        g_state.adapter.Shutdown();
    }

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
    const Wanted wanted = ReadWanted();
    const bool cfgChanged = !g_state.haveCfg || wanted.centerX != g_state.cfgCenterX ||
                            wanted.centerY != g_state.cfgCenterY || wanted.workX != g_state.cfgWorkX ||
                            wanted.workY != g_state.cfgWorkY;

    if (g_state.initialized && !cfgChanged && g_state.nativeWidth == nativeWidth &&
        g_state.nativeHeight == nativeHeight && g_state.format == colorFormat)
        return true;

    if (g_state.initialized)
        ShutdownInternal(true);

    if (cfgChanged)
        g_state.initFailed = false;

    if (g_state.initFailed)
        return false;

    g_state.haveCfg = true;
    g_state.cfgCenterX = wanted.centerX;
    g_state.cfgCenterY = wanted.centerY;
    g_state.cfgWorkX = wanted.workX;
    g_state.cfgWorkY = wanted.workY;
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
    pwConfig.xAxis.centerPercent = wanted.centerX;
    pwConfig.xAxis.workPercent = wanted.workX;
    pwConfig.yAxis.centerPercent = wanted.centerY;
    pwConfig.yAxis.workPercent = wanted.workY;

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

void Shutdown()
{
    ShutdownInternal(false);
    g_retired.clear();
}

bool MotionFormatSupported(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R16G16_FLOAT || format == DXGI_FORMAT_R32G32_FLOAT ||
           format == DXGI_FORMAT_R16G16_SNORM || format == DXGI_FORMAT_R16G16_UNORM;
}

bool GetInfo(unsigned int* nativeWidth, unsigned int* nativeHeight, unsigned int* workWidth,
             unsigned int* workHeight)
{
    if (!g_state.initialized)
        return false;

    *nativeWidth = g_state.nativeWidth;
    *nativeHeight = g_state.nativeHeight;
    *workWidth = g_state.workWidth;
    *workHeight = g_state.workHeight;
    return true;
}

bool Pack(
    ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* color, DXGI_FORMAT colorFormat, unsigned int colorWidth, unsigned int colorHeight,
    ID3D12Resource* depth, DXGI_FORMAT depthFormat,
    unsigned int depthBaseX, unsigned int depthBaseY, unsigned int depthWidth, unsigned int depthHeight,
    ID3D12Resource* motion, DXGI_FORMAT motionFormat,
    unsigned int motionBaseX, unsigned int motionBaseY, unsigned int motionWidth, unsigned int motionHeight,
    float motionScaleX, float motionScaleY, bool depthInverted,
    ID3D12Resource** outColor, ID3D12Resource** outDepth, ID3D12Resource** outMotion,
    unsigned int* outWorkWidth, unsigned int* outWorkHeight, D3D12_RESOURCE_STATES motionState) noexcept
{
    // A fresh Pack() invalidates any leftover guides from a previous frame immediately: if this
    // call fails or is never reached again this frame, Unpack() must refuse to run rather than
    // silently reuse stale data.
    g_state.lastPackedDepth = nullptr;
    g_state.lastPackedMotion = nullptr;

    for (auto& r : g_retired)
        --r.framesLeft;
    g_retired.erase(std::remove_if(g_retired.begin(), g_retired.end(),
                                   [](const Retired& r) { return r.framesLeft <= 0; }),
                    g_retired.end());

    if (device == nullptr || cmdList == nullptr || color == nullptr || depth == nullptr || motion == nullptr ||
        outColor == nullptr || outDepth == nullptr || outMotion == nullptr || outWorkWidth == nullptr ||
        outWorkHeight == nullptr)
        return false;

    const auto fail = [&](const char* stage, const char* detail)
    {
        LogFallback(stage, detail, (unsigned int) colorFormat, (unsigned int) depthFormat,
                    (unsigned int) motionFormat, colorWidth, colorHeight, depthWidth, depthHeight,
                    motionWidth, motionHeight);
        return false;
    };

    if (!EnsureInitialized(device, colorFormat, colorWidth, colorHeight))
        return fail("init", g_state.initFailed ? "adapter setup failed earlier" : "adapter setup failed");

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
        return fail("input description", "rejected by the SDK (a rect or scale is out of range)");

    pw::D3D12SourceResources sources {};
    sources.color = { color, colorFormat };
    sources.depth = { depth, depthFormat };
    sources.motion = { motion, motionFormat };
    sources.confidence = { nullptr, DXGI_FORMAT_UNKNOWN };

    // color/depth/motion arrive in NON_PIXEL_SHADER_RESOURCE (what this fork's compute path and
    // the model read them in). Pack's draw is a pixel shader, which may only read
    // PIXEL_SHADER_RESOURCE, so they are switched for the draw and restored right after.
    const unsigned int ring = g_state.frameCounter++ % kRing;

    const auto writeStatus = g_state.adapter.WriteSourceSetV2(ring, sources, description);

    if (writeStatus != pw::AdapterStatus::Ok)
        return fail("source registration", pw::AdapterStatusString(writeStatus));

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
    UINT srcCount = 0;
    const auto addSource = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES before)
    {
        // Already readable by a pixel shader: nothing to change (a same-state barrier is illegal).
        if ((before & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0)
            return;

        auto& b = srcToPixel[srcCount++];
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    };
    addSource(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    addSource(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    addSource(motion, motionState);

    if (srcCount != 0)
        cmdList->ResourceBarrier(srcCount, srcToPixel);

    const auto packStatus = g_state.adapter.RecordPackFromSet(cmdList, 0, ring);

    if (srcCount != 0)
    {
        D3D12_RESOURCE_BARRIER srcBack[3] { srcToPixel[0], srcToPixel[1], srcToPixel[2] };
        for (UINT i = 0; i < srcCount; ++i)
            std::swap(srcBack[i].Transition.StateBefore, srcBack[i].Transition.StateAfter);
        cmdList->ResourceBarrier(srcCount, srcBack);
    }

    D3D12_RESOURCE_BARRIER toSrv[3] { toRt[0], toRt[1], toRt[2] };
    for (auto& b : toSrv)
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    cmdList->ResourceBarrier(3, toSrv);
    g_state.packedNeedsTransitionIn = true;

    if (packStatus != pw::AdapterStatus::Ok)
        return fail("pack draw", pw::AdapterStatusString(packStatus));

    g_state.lastPackedDepth = packed.resources.depth.resource;
    g_state.lastPackedMotion = packed.resources.motion.resource;
    g_state.lastDepthConvention = description.depthConvention;
    g_state.lastRing = ring;

    {
        static std::string lastOk;
        const std::string now = std::to_string((int) colorFormat) + "/" + std::to_string((int) depthFormat) + "/" +
                                std::to_string((int) motionFormat) + "/" + std::to_string(colorWidth) + "x" +
                                std::to_string(colorHeight) + "/" + std::to_string(depthWidth) + "x" +
                                std::to_string(depthHeight) + "/" + std::to_string(motionWidth) + "x" +
                                std::to_string(motionHeight);

        if (now != lastOk)
        {
            lastOk = now;
            LOG_INFO("DLSS-NR PeripheralWarp: packing colour fmt {} {}x{}, depth fmt {} {}x{}, motion fmt {} {}x{}",
                     (int) colorFormat, colorWidth, colorHeight, (int) depthFormat, depthWidth, depthHeight,
                     (int) motionFormat, motionWidth, motionHeight);
        }
    }

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
