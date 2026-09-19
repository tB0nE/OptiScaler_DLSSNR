#include "pch.h"
#include "DlssNr_Temporal.h"

#include <Config.h>
#include <Util.h>

#include "peripheral_warp/temporal/ngx_temporal.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <utility>
#include <vector>

namespace DlssNr::Temporal
{

namespace
{

struct Retired
{
    std::unique_ptr<pwtemporal::Machine> machine;
    int framesLeft = 64;
};

std::unique_ptr<pwtemporal::Machine> g_machine;
std::vector<Retired> g_retired;
pwngx::Shaders g_shaders;
bool g_shadersTried = false;
bool g_failed = false;
unsigned int g_sinceFull = 0;
unsigned long long g_fullFrames = 0;
unsigned long long g_interpFrames = 0;

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

bool LoadShaders()
{
    if (g_shadersTried)
        return g_shaders.TemporalLoaded() && !g_shaders.vertex.empty();

    g_shadersTried = true;
    const auto dir = Util::DllPath().remove_filename() / "peripheral_warp";
    LoadFile(dir / "fullscreen_vs.dxbc", g_shaders.vertex);
    LoadFile(dir / "temporal_residual_ps.dxbc", g_shaders.temporalResidual);
    LoadFile(dir / "temporal_accumulate_ps.dxbc", g_shaders.temporalAccumulate);
    LoadFile(dir / "temporal_reproject_ps.dxbc", g_shaders.temporalReproject);
    LoadFile(dir / "temporal_downsample_ps.dxbc", g_shaders.temporalDownsample);
    LoadFile(dir / "temporal_compose_ps.dxbc", g_shaders.temporalCompose);

    if (!g_shaders.TemporalLoaded() || g_shaders.vertex.empty())
        LOG_ERROR("DLSS-NR temporal: shader files missing in {} (need fullscreen_vs and temporal_*_ps .dxbc)",
                  dir.string());

    return g_shaders.TemporalLoaded() && !g_shaders.vertex.empty();
}

pwtemporal::FrameInputs Make(const FrameArgs& a)
{
    pwtemporal::FrameInputs t;
    t.base = a.base;
    t.baseState = a.baseState;
    t.color = a.base;
    t.colorRect = { 0, 0, a.frameW, a.frameH };
    t.motion = a.motion;
    t.motionView = a.motionView;
    t.motionRect = { a.motionX, a.motionY, a.motionW, a.motionH };
    t.depth = a.depth;
    t.depthView = a.depthView;
    t.depthRect = { a.depthX, a.depthY, a.depthW, a.depthH };
    t.mvScaleX = a.mvScaleX;
    t.mvScaleY = a.mvScaleY;
    t.depthInverted = a.depthInverted;
    t.hostInputState = a.motionState;
    t.depthState = a.depthState;
    t.residualBlend = a.residualBlend;
    t.blendFromMotion = a.blendFromMotion;
    t.motionIsPassChain = a.motionIsChain;
    if (a.motionIsChain)
    {
        t.mvScaleX = 1.0f;
        t.mvScaleY = 1.0f;
        t.motionSign = 1.0f;
    }
    t.depthSubresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    const auto& cfg = *Config::Instance();
    t.residualCatmullRom = cfg.DlssNrTemporalCatmullRom.value_or_default();
    t.holeFill = cfg.DlssNrTemporalHoleFill.value_or_default();
    t.colorTolerance = cfg.DlssNrTemporalColorTolerance.value_or_default();
    t.depthThreshold = cfg.DlssNrTemporalDepthTolerance.value_or_default();
    t.smoothRadius = std::clamp(cfg.DlssNrTemporalSmoothRadius.value_or_default(), 0.0f, 128.0f);
    t.debugVis = (int) std::min(cfg.DlssNrTemporalDebugView.value_or_default(), 3u);
    return t;
}

} // namespace

bool Enabled() { return Config::Instance()->DlssNrTemporalEnabled.value_or_default(); }

bool EnsureMachine(ID3D12Device* device, unsigned int frameW, unsigned int frameH, DXGI_FORMAT format,
                   ID3D12Resource* motion, ID3D12Resource* depth, bool* created)
{
    if (created != nullptr)
        *created = false;

    for (auto& r : g_retired)
        --r.framesLeft;
    g_retired.erase(std::remove_if(g_retired.begin(), g_retired.end(),
                                   [](const Retired& r) { return r.framesLeft <= 0; }),
                    g_retired.end());

    if (g_failed || device == nullptr || motion == nullptr || depth == nullptr)
        return false;

    const D3D12_RESOURCE_DESC md = motion->GetDesc();
    const D3D12_RESOURCE_DESC dd = depth->GetDesc();

    if (!LoadShaders())
    {
        g_failed = true;
        return false;
    }

    if (g_machine && g_machine->Matches(frameW, frameH, format, (unsigned int) md.Width, md.Height, dd.Format,
                                        (unsigned int) dd.Width, dd.Height))
        return true;

    if (g_machine)
    {
        // The old machine's textures may still be in flight.
        Retired old;
        old.machine = std::move(g_machine);
        g_retired.push_back(std::move(old));
    }

    auto machine = std::make_unique<pwtemporal::Machine>();
    char error[256] = {};

    if (!machine->Initialize(device, g_shaders, frameW, frameH, format, format, (unsigned int) md.Width, md.Height,
                             dd.Format, (unsigned int) dd.Width, dd.Height, error, sizeof(error)))
    {
        LOG_ERROR("DLSS-NR temporal: could not start ({})", error);
        g_failed = true;
        return false;
    }

    g_machine = std::move(machine);
    g_sinceFull = 0;

    if (created != nullptr)
        *created = true;

    LOG_INFO("DLSS-NR temporal: machine ready (frame {}x{}, motion {}x{}, depth {}x{} fmt {})", frameW, frameH,
             (unsigned int) md.Width, md.Height, (unsigned int) dd.Width, dd.Height, (int) dd.Format);
    return true;
}

bool Plan(ID3D12Device* device, unsigned int frameW, unsigned int frameH, DXGI_FORMAT format,
          ID3D12Resource* motion, ID3D12Resource* depth, bool reset, bool* interpolate)
{
    *interpolate = false;

    if (!Enabled() || !EnsureMachine(device, frameW, frameH, format, motion, depth, nullptr))
        return false;

    if (reset)
    {
        g_machine->Invalidate();
        g_sinceFull = 0;
    }

    const unsigned int every = std::clamp(Config::Instance()->DlssNrTemporalEvery.value_or_default(), 2u, 8u);
    const bool full = !g_machine->HasResidual() || reset || g_sinceFull >= every - 1;
    *interpolate = !full;
    return true;
}

bool HasResidual() { return g_machine && g_machine->HasResidual(); }
void Invalidate()
{
    if (g_machine)
        g_machine->Invalidate();
}
bool PendingValid() { return g_machine && g_machine->PendingValid(); }
bool PendingMirrorsAcc() { return g_machine && g_machine->PendingMirrorsAcc(); }
void ResetPending()
{
    if (g_machine)
        g_machine->ResetPending();
}
void PromotePending()
{
    if (g_machine)
        g_machine->PromotePending();
}
void AccumulatePending(ID3D12GraphicsCommandList* cmd, const FrameArgs& args)
{
    if (g_machine)
        g_machine->RecordAccumulatePending(cmd, Make(args));
}
void CopyAcc(ID3D12GraphicsCommandList* cmd, bool pending, ID3D12Resource* dst, D3D12_RESOURCE_STATES dstState)
{
    if (g_machine)
        g_machine->RecordCopyAcc(cmd, pending, dst, dstState);
}

void Accumulate(ID3D12GraphicsCommandList* cmd, const FrameArgs& args)
{
    if (g_machine)
        g_machine->RecordAccumulate(cmd, Make(args));
}

void Reproject(ID3D12GraphicsCommandList* cmd, const FrameArgs& args, ID3D12Resource* target,
               D3D12_RESOURCE_STATES targetState)
{
    if (g_machine)
        g_machine->RecordReproject(cmd, Make(args), target, targetState, 0, 0);
}

void Residual(ID3D12GraphicsCommandList* cmd, const FrameArgs& args, ID3D12Resource* target,
              D3D12_RESOURCE_STATES targetState)
{
    if (g_machine)
        g_machine->RecordResidual(cmd, Make(args), target, targetState);
}

ID3D12Resource* Acc() { return g_machine ? g_machine->Acc() : nullptr; }

bool AccValid() { return g_machine && g_machine->AccValid(); }

void FrameDone(bool full)
{
    if (full)
    {
        g_sinceFull = 0;
        ++g_fullFrames;
    }
    else
    {
        ++g_sinceFull;
        ++g_interpFrames;
    }

    if (g_fullFrames + g_interpFrames == 2)
        LOG_INFO("DLSS-NR temporal: running (full model pass every {} frames)",
                 std::clamp(Config::Instance()->DlssNrTemporalEvery.value_or_default(), 2u, 8u));
}

void Shutdown()
{
    g_machine.reset();
    g_retired.clear();
    g_failed = false;
    g_sinceFull = 0;
}

} // namespace DlssNr::Temporal
