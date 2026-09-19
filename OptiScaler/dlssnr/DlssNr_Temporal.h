#pragma once

// Temporal mode for DLSS-NR: the model runs on every Nth frame and the frames in between reuse the
// last pass's edit (final frame minus the untouched frame), reprojected along the game's own motion
// vectors. The reprojection machine is vendored from optimizer-fps-dlss5 (MIT); this file owns its
// lifetime and decides which frame is which. Synchronous only: no background queue.

#include <d3d12.h>

namespace DlssNr::Temporal
{

// The game's guides for this frame, in their own texture spaces. `base` is the untouched frame
// (native size, the target's format) in NON_PIXEL_SHADER_RESOURCE; depth and motion are
// NON_PIXEL_SHADER_RESOURCE too.
struct FrameArgs
{
    ID3D12Resource* base = nullptr;
    ID3D12Resource* motion = nullptr;
    DXGI_FORMAT motionView = DXGI_FORMAT_UNKNOWN;
    unsigned int motionX = 0, motionY = 0, motionW = 0, motionH = 0;
    ID3D12Resource* depth = nullptr;
    DXGI_FORMAT depthView = DXGI_FORMAT_UNKNOWN;
    unsigned int depthX = 0, depthY = 0, depthW = 0, depthH = 0;
    float mvScaleX = 1.0f, mvScaleY = 1.0f; // raw motion -> full-frame pixels
    bool depthInverted = false;
    unsigned int frameW = 0, frameH = 0;
};

bool Enabled();

// Decides this frame. False = temporal is unavailable right now (run the model normally).
// On true, *interpolate says whether this frame skips the model.
bool Plan(ID3D12Device* device, unsigned int frameW, unsigned int frameH, DXGI_FORMAT format,
          ID3D12Resource* motion, ID3D12Resource* depth, bool reset, bool* interpolate);

// Once per frame, before anything reads Acc().
void Accumulate(ID3D12GraphicsCommandList* cmd, const FrameArgs& args);

// Interpolated frame: writes base + reprojected edit into target (which must be in targetState).
void Reproject(ID3D12GraphicsCommandList* cmd, const FrameArgs& args, ID3D12Resource* target,
               D3D12_RESOURCE_STATES targetState);

// Full frame, after the resolve: stores (target - base) as the edit to reproject.
void Residual(ID3D12GraphicsCommandList* cmd, const FrameArgs& args, ID3D12Resource* target,
              D3D12_RESOURCE_STATES targetState);

// Motion accumulated since the last full frame, in full-frame pixels, R16G16_FLOAT, left in
// ALL_SHADER_RESOURCE. Null until a full frame has been followed by an accumulate.
ID3D12Resource* Acc();
bool AccValid();

void FrameDone(bool full);
void Shutdown();

} // namespace DlssNr::Temporal
