#pragma once

// A hook on ID3D12CommandQueue::ExecuteCommandLists for the background temporal mode: when the game
// submits the list that carries a pass's input copies, the queue that ran it signals the fence the
// background queue is waiting on, straight after the submit. That is the moment the copies are
// guaranteed to be ahead of everything the background pass reads.

#include <d3d12.h>

namespace DlssNr::AsyncHook
{

// Patches the (shared) queue vtable once. False if it could not.
bool Install(ID3D12Device* device);

// Arms the hook: the queue that executes `list` (matched by pointer, or by a tag stored on it, since
// a proxy may sit in front) signals `fence` to `value` right after the submit.
void Watch(ID3D12GraphicsCommandList* list, ID3D12Fence* fence, unsigned long long value);

// True once the armed submission was seen and signalled.
bool Fired();

// Disarms. Call before the fence is released.
void Cancel();

} // namespace DlssNr::AsyncHook
