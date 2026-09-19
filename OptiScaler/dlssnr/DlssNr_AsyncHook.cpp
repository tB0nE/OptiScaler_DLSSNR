#include "pch.h"
#include "DlssNr_AsyncHook.h"

#include <Util.h>
#include <detours/detours.h>

#include <atomic>
#include <mutex>

namespace DlssNr::AsyncHook
{

namespace
{

typedef void(STDMETHODCALLTYPE* PFN_ExecuteCommandLists)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

PFN_ExecuteCommandLists o_ExecuteCommandLists = nullptr;

// {5E0C7A11-2B4F-4D6A-8E31-DA55BACC0001}
const GUID kTag = { 0x5e0c7a11, 0x2b4f, 0x4d6a, { 0x8e, 0x31, 0xda, 0x55, 0xba, 0xcc, 0x00, 0x01 } };

std::mutex g_mutex;
std::atomic<bool> g_armed { false };
std::atomic<bool> g_fired { false };
ID3D12CommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
unsigned long long g_value = 0;

void STDMETHODCALLTYPE hkExecuteCommandLists(ID3D12CommandQueue* This, UINT count, ID3D12CommandList* const* lists)
{
    bool match = false;

    if (g_armed.load(std::memory_order_acquire) && lists != nullptr)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (g_armed.load(std::memory_order_relaxed))
        {
            for (UINT i = 0; i < count && !match; ++i)
            {
                if (lists[i] == nullptr)
                    continue;

                if (lists[i] == g_list)
                {
                    match = true;
                    break;
                }

                unsigned long long tag = 0;
                UINT size = sizeof(tag);

                if (SUCCEEDED(lists[i]->GetPrivateData(kTag, &size, &tag)) && size == sizeof(tag) && tag == g_value)
                    match = true;
            }
        }
    }

    o_ExecuteCommandLists(This, count, lists);

    if (!match)
        return;

    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_armed.load(std::memory_order_relaxed) && g_fence != nullptr)
    {
        This->Signal(g_fence, g_value);
        g_armed.store(false, std::memory_order_release);
        g_fired.store(true, std::memory_order_release);
    }
}

} // namespace

bool Install(ID3D12Device* device)
{
    static bool tried = false;

    if (tried)
        return o_ExecuteCommandLists != nullptr;

    tried = true;

    if (device == nullptr)
        return false;

    D3D12_COMMAND_QUEUE_DESC desc {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ID3D12CommandQueue* queue = nullptr;

    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))) || queue == nullptr)
        return false;

    ID3D12CommandQueue* realQueue = nullptr;

    if (!Util::CheckForRealObject(__FUNCTION__, queue, (IUnknown**) &realQueue))
        realQueue = queue;

    PVOID* vtable = *(PVOID**) realQueue;
    o_ExecuteCommandLists = (PFN_ExecuteCommandLists) vtable[10];

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) o_ExecuteCommandLists, hkExecuteCommandLists);
    const auto result = DetourTransactionCommit();

    queue->Release();

    if (result != NO_ERROR)
    {
        LOG_ERROR("DLSS-NR background: could not hook ExecuteCommandLists ({:X})", (unsigned long) result);
        o_ExecuteCommandLists = nullptr;
        return false;
    }

    LOG_INFO("DLSS-NR background: ExecuteCommandLists hooked");
    return true;
}

void Watch(ID3D12GraphicsCommandList* list, ID3D12Fence* fence, unsigned long long value)
{
    list->SetPrivateData(kTag, sizeof(value), &value);

    std::lock_guard<std::mutex> lock(g_mutex);
    g_list = list;
    g_fence = fence;
    g_value = value;
    g_fired.store(false, std::memory_order_relaxed);
    g_armed.store(true, std::memory_order_release);
}

bool Fired() { return g_fired.load(std::memory_order_acquire); }

void Cancel()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_armed.store(false, std::memory_order_release);
    g_fence = nullptr;
    g_list = nullptr;
}

} // namespace DlssNr::AsyncHook
