#pragma once

// Native integration of optimizer-fps-dlss5's PeripheralWarp SDK (MIT-licensed,
// https://github.com/BeliyG3/optimizer-fps-dlss5) into this fork's own DX12 NR call path,
// instead of running it as a separate standalone ReShade add-on.
//
// Why native rather than the standalone add-on: running the add-on alongside this fork and
// ReShade means three independent hook layers rewriting the same D3D12 command list state.
// That combination produced a confirmed GPU fault (NVRM Xid 32, invalid/corrupted push buffer
// stream) in testing. Doing the Pack/Unpack inside our own evaluate call removes that failure
// mode: only this fork's own, already-stable D3D12 code touches the command list.
//
// Scope of this first integration: the primary pass only (pass 0). Multipass + warp together
// would need per-pass frame-slot tracking; left out for now to keep the surface small while this
// is unproven in this environment (RTX 30-series via vkd3d-proton -- the SDK was developed and
// tested on Windows, RTX 4080 SUPER).

#include <d3d12.h>

namespace DlssNr::PeripheralWarp
{

// Config-gated; false unless [DlssNr] PeripheralWarpEnabled=true.
bool Enabled();

// The layout the adapter is currently built for. False until the first successful Pack().
bool GetInfo(unsigned int* nativeWidth, unsigned int* nativeHeight, unsigned int* workWidth,
             unsigned int* workHeight);

// True for the two-channel motion formats the SDK reads directly. Anything else (ray tracing
// makes Cyberpunk write a four-channel motion texture) must be converted to R16G16_FLOAT first.
bool MotionFormatSupported(DXGI_FORMAT format);

// Releases the adapter and its resources. Call on device loss / shutdown / resolution change,
// the same way the rest of NrState's D3D12 resources are torn down.
void Shutdown();

// Packs color/depth/motion into a smaller work-extent proxy before the model runs.
//
// On success, *outColor/*outDepth/*outMotion point at adapter-owned, SRV-readable resources at
// *outWorkWidth x *outWorkHeight (already in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
// call the model's evaluate with these instead of the original color/depth/motion, and with
// the returned work extent instead of the caller's own workWidth/workHeight.
//
// On failure (returns false), touches nothing: the caller should evaluate on the original,
// unwarped inputs exactly as it would with this feature off. This is the SDK's own documented
// rule -- skip Warp for an unsupported signature, never guess a view or present a packed image.
//
// On success, this call also remembers this frame's packed depth/motion guides internally, for
// the matching Unpack() call to reuse -- see Unpack()'s doc comment.
bool Pack(
    ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* color, DXGI_FORMAT colorFormat, unsigned int colorWidth, unsigned int colorHeight,
    ID3D12Resource* depth, DXGI_FORMAT depthFormat,
    unsigned int depthBaseX, unsigned int depthBaseY, unsigned int depthWidth, unsigned int depthHeight,
    ID3D12Resource* motion, DXGI_FORMAT motionFormat,
    unsigned int motionBaseX, unsigned int motionBaseY, unsigned int motionWidth, unsigned int motionHeight,
    float motionScaleX, float motionScaleY, bool depthInverted,
    ID3D12Resource** outColor, ID3D12Resource** outDepth, ID3D12Resource** outMotion,
    unsigned int* outWorkWidth, unsigned int* outWorkHeight) noexcept;

// Reconstructs the model's work-extent answer back to nativeWidth x nativeHeight, into
// nativeTarget -- one of this fork's own UAV scratch buffers (g_nr.output / g_nr.passScratch),
// unmodified from how it is created elsewhere in this file.
//
// modelOutput is the model's own work-extent answer (what it wrote into the buffer passed as
// Pack's outColor/outDepth/outMotion triple stood in for -- the *color* slot specifically) and
// must be in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE on entry, the same state the caller's
// MakeModelReadable() lambda already leaves it in, and is left in that same state on return.
//
// nativeTarget must be in D3D12_RESOURCE_STATE_UNORDERED_ACCESS on entry (the state the caller's
// MakeModelWritable() lambda already leaves it in) and is left in that same state on return --
// this function owns every transition in between internally (on modelOutput, nativeTarget, and
// this frame's packed depth/motion guides left over from Pack()), so the caller's existing
// barrier lambdas (MakeModelReadable/MakeModelWritable) keep working completely unchanged on
// both the warped and unwarped paths.
//
// Only valid to call once, immediately after a Pack() that returned true on the same frame --
// internally it reuses that Pack() call's packed depth/motion guides (there is no separate
// "unpack guides" concept in this SDK) and consumes them; a second call without an intervening
// Pack() fails immediately.
bool Unpack(
    ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* modelOutput, DXGI_FORMAT modelOutputFormat,
    ID3D12Resource* nativeTarget, unsigned int nativeWidth, unsigned int nativeHeight,
    DXGI_FORMAT nativeFormat) noexcept;

} // namespace DlssNr::PeripheralWarp
