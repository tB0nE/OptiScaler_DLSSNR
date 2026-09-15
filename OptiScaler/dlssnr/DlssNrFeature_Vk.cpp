#include "pch.h"

#include "DlssNrFeature_Vk.h"

// Native-Vulkan DLSS-NR path: stubbed out.
//
// This fork targets games that present through DXGI/D3D12 (the DlssNr_Dx12.cpp path), not native
// Vulkan titles. Upstream's native Vulkan implementation depends on pre-SR/pass-profile scaffolding
// that was not carried into this fork, so rather than dragging that dependency across, this file
// stays a safe, always-inert stub: NR simply never comes up on the native Vulkan path here.

namespace DlssNr
{

void EvaluateAfterUpscaleVk(VkCommandBuffer, NVSDK_NGX_Parameter*, VkInstance, VkPhysicalDevice, VkDevice) {}

bool IsRunningVk() { return false; }

const char* FailureReasonVk() { return "Native Vulkan NR is not built in this fork."; }

unsigned long long FramesVk() { return 0; }

std::optional<double> LastGpuTimeVk() { return std::nullopt; }

bool ExposureOfferedVk() { return false; }

void ShutdownVk(bool) {}

} // namespace DlssNr
