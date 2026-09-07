// Included in namespace DlssNr by DlssNr_Dx12.cpp, after the private NR helpers.
// Calls below are serialized by g_nrMutex. Nothing writes the game's NGX parameter block.
namespace DeferredSr
{
constexpr unsigned MarkerCount = 16;
struct Generation
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr; // identity/reference only; no private submissions
    unsigned w = 0, h = 0, outW = 0, outH = 0, flags = 0;
    DXGI_FORMAT inputFormat {}, outputFormat {};
    ID3D12Resource *edited = nullptr, *residualInput = nullptr, *residualOutput = nullptr, *clean = nullptr,
                   *composed = nullptr, *exposure = nullptr, *readback = nullptr;
    ID3D12QueryHeap* queries = nullptr;
    volatile UINT64* completed = nullptr;
    bool occupied[MarkerCount] {};
    unsigned nextMarker = 0, lastMarker = 0;
    bool everRecorded = false, smallReadable = false, reset = true, failed = false;
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    unsigned long long createEpoch = 0;
    unsigned long long lastBeginEpoch = 0;
    bool began = false;
    std::unique_ptr<DlssNr_Dx12> codec;

    bool Idle() const { return !everRecorded || completed[lastMarker] != 0; }
    ~Generation()
    {
        if (feature && NVNGXProxy::D3D12_ReleaseFeature())
            NVNGXProxy::D3D12_ReleaseFeature()(feature);
        if (parameters && NVNGXProxy::D3D12_DestroyParameters())
            NVNGXProxy::D3D12_DestroyParameters()(parameters);
        if (readback && completed) readback->Unmap(0, nullptr);
        for (auto* r : { edited, residualInput, residualOutput, clean, composed, exposure, readback })
            if (r) r->Release();
        if (queries) queries->Release();
        if (queue) queue->Release();
        if (device) device->Release();
    }
};

// Record an actual GPU completion marker after EACH seam. A later CPU frame/Present count alone
// does not prove a resource is no longer in flight. Slots aren't reused until the GPU wrote them.
struct Use
{
    Generation& g;
    ID3D12GraphicsCommandList* cmd;
    unsigned slot;
    bool valid;
    Use(Generation& gen, ID3D12GraphicsCommandList* commands) : g(gen), cmd(commands), slot(g.nextMarker)
    {
        valid = !g.occupied[slot] || g.completed[slot] != 0;
        if (!valid) return;
        g.completed[slot] = 0;
        g.occupied[slot] = true;
        g.lastMarker = slot;
        g.everRecorded = true;
        g.nextMarker = (slot + 1) % MarkerCount;
    }
    ~Use()
    {
        if (!valid) return;
        cmd->EndQuery(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, slot);
        cmd->ResolveQueryData(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, slot, 1,
                              g.readback, slot * sizeof(UINT64));
    }
};

std::unique_ptr<Generation> current;
std::vector<std::unique_ptr<Generation>> retired;
std::string status = "not started";
struct Pending
{
    ID3D12GraphicsCommandList* cmd = nullptr;
    NVSDK_NGX_Parameter* caller = nullptr;
    ID3D12Resource* output = nullptr;
    unsigned long long epoch = 0;
    float scale = 1;
} pending;

void Say(const std::string& text)
{
    if (status == text) return;
    status = text;
    LOG_INFO("DLSS-NR deferred DLSS: {}", text);
}
void Cancel()
{
    pending = {};
    if (current) current->reset = true;
}
void Collect()
{
    std::erase_if(retired, [](const auto& g) { return g->Idle(); });
}

unsigned UInt(NVSDK_NGX_Parameter* p, const char* key, unsigned fallback = 0)
{
    unsigned value = fallback;
    p->Get(key, &value);
    return value;
}
float Float(NVSDK_NGX_Parameter* p, const char* key, float fallback)
{
    float value = fallback;
    p->Get(key, &value);
    return std::isfinite(value) ? value : fallback;
}

bool Allocate(Generation& g)
{
    g.edited = CreateScratch(g.device, g.inputFormat, g.w, g.h);
    g.residualInput = CreateScratch(g.device, DXGI_FORMAT_R16G16B16A16_FLOAT, g.w, g.h);
    g.residualOutput = CreateScratch(g.device, DXGI_FORMAT_R16G16B16A16_FLOAT, g.outW, g.outH);
    g.clean = CreateScratch(g.device, g.outputFormat, g.outW, g.outH);
    g.composed = CreateScratch(g.device, g.outputFormat, g.outW, g.outH);
    g.exposure = CreateScratch(g.device, DXGI_FORMAT_R32_FLOAT, 1, 1);
    if (!g.edited || !g.residualInput || !g.residualOutput || !g.clean || !g.composed || !g.exposure) return false;
    g.codec = std::make_unique<DlssNr_Dx12>("Deferred NR contribution", g.device);
    if (!g.codec->IsInit()) return false;
    D3D12_QUERY_HEAP_DESC query {};
    query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query.Count = MarkerCount;
    if (FAILED(g.device->CreateQueryHeap(&query, IID_PPV_ARGS(&g.queries)))) return false;
    auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(MarkerCount * sizeof(UINT64));
    if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.readback)))) return false;
    void* mapped = nullptr;
    if (FAILED(g.readback->Map(0, nullptr, &mapped))) return false;
    g.completed = static_cast<volatile UINT64*>(mapped);
    for (unsigned i = 0; i < MarkerCount; ++i) g.completed[i] = 0;
    return true;
}

void Before(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* source,
            unsigned long long epoch, ID3D12CommandQueue* queue)
{
    if (pending.cmd && current) current->reset = true; // abandoned/failed main SR call
    pending = {};
    struct ResetOnGap
    {
        ~ResetOnGap() { if (!pending.cmd && current) current->reset = true; }
    } resetOnGap;
    Collect();
    const auto& cfg = *Config::Instance();
    if (cfg.DlssNrUseProxy.value_or_default() || cfg.DlssNrHoldFrame.value_or_default() ||
        cfg.DlssNrDebugView.value_or_default() != 0 || cfg.DlssNrCompare.value_or_default() != 0 ||
        cfg.DlssNrShowSkinMask.value_or_default() ||
        !cfg.DlssNrApplyModel.value_or_default())
    {
        Say("inactive: disable proxy backend, frame hold/debug/compare, and enable Apply model");
        return;
    }
    if (cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        ((cfg.RestoreComputeSignature.value_or_default() || cfg.RestoreGraphicSignature.value_or_default()) &&
         !D3D12Hooks::CanRestoreRootSignature(cmd)))
    {
        Say("inactive: requires a direct command list with restorable game state");
        return;
    }
    auto* color = GetResource(source, NVSDK_NGX_Parameter_Color, "DLSSD.Color");
    auto* output = GetResource(source, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    auto* depth = GetResource(source, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    auto* motion = GetResource(source, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");
    if (!color || !output || !depth || !motion || color == output)
    {
        Say("inactive: distinct Color/Output, depth and motion are required");
        return;
    }
    for (const char* key : { NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
         NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
         NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
         NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,
         NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y })
        if (UInt(source, key) != 0) { Say("inactive: non-zero colour/guide/output offsets"); return; }
    const auto inDesc = color->GetDesc(), outDesc = output->GetDesc();
    const auto active = PreSrColorExtent(inDesc,
        UInt(source, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width),
        UInt(source, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height));
    if (!active || !PreSrColorExtent(outDesc, 0, 0) || inDesc.MipLevels != 1 || outDesc.MipLevels != 1 ||
        active->width > outDesc.Width || active->height > outDesc.Height)
    {
        Say("inactive: unsupported active input/output dimensions");
        return;
    }
    ID3D12Device* device = nullptr;
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device)))) return;
    auto* ownerQueue = queue ? queue : (ID3D12CommandQueue*)State::Instance().currentCommandQueue;
    ID3D12Device* queueDevice = nullptr;
    if (!ownerQueue || ownerQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        FAILED(ownerQueue->GetDevice(IID_PPV_ARGS(&queueDevice))) || queueDevice != device)
    {
        if (queueDevice) queueDevice->Release();
        device->Release();
        Say("waiting for a same-device direct queue identity");
        return;
    }
    queueDevice->Release();
    const unsigned flags = UInt(source, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags) &
        (NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
         NVSDK_NGX_DLSS_Feature_Flags_MVJittered);
    if (current && (current->device != device || current->queue != ownerQueue || current->w != active->width ||
        current->h != active->height || current->outW != outDesc.Width || current->outH != outDesc.Height ||
        current->inputFormat != inDesc.Format || current->outputFormat != outDesc.Format || current->flags != flags))
        retired.push_back(std::move(current));
    if (!current)
    {
        if (retired.size() >= 4) { device->Release(); Say("waiting for retired GPU work; clean SR frame retained"); return; }
        current = std::make_unique<Generation>();
        current->device = device; // take the GetDevice reference
        current->queue = ownerQueue;
        ownerQueue->AddRef();
        current->w = active->width; current->h = active->height;
        current->outW = (unsigned)outDesc.Width; current->outH = outDesc.Height;
        current->inputFormat = inDesc.Format; current->outputFormat = outDesc.Format; current->flags = flags;
        if (!Allocate(*current)) { current->failed = true; Say("allocation failed; clean SR frame retained"); return; }
    }
    else device->Release();
    auto& g = *current;
    if (g.failed) return;
    if (g.began && g.lastBeginEpoch == epoch)
    { g.reset = true; Say("inactive: more than one upscale in a submission epoch"); return; }
    g.began = true;
    g.lastBeginEpoch = epoch;
    Use use(g, cmd);
    if (!use.valid) { Say("waiting for GPU completion slots; clean SR frame retained"); return; }
    if (!g.feature)
    {
        ScopedNrStateEnvelope envelope(cmd);
        if (!NVNGXProxy::InitDx12(g.device) || !NVNGXProxy::D3D12_AllocateParameters() ||
            !NVNGXProxy::D3D12_DestroyParameters() || !NVNGXProxy::D3D12_CreateFeature() ||
            !NVNGXProxy::D3D12_EvaluateFeature() || !NVNGXProxy::D3D12_ReleaseFeature() ||
            NVNGXProxy::D3D12_AllocateParameters()(&g.parameters) != NVSDK_NGX_Result_Success || !g.parameters)
        { g.failed = true; Say("NVIDIA DLSS SR runtime unavailable; no alternative upscaler used"); return; }
        auto* p = g.parameters;
        p->Set(NVSDK_NGX_Parameter_Width, g.w); p->Set(NVSDK_NGX_Parameter_Height, g.h);
        p->Set(NVSDK_NGX_Parameter_OutWidth, g.outW); p->Set(NVSDK_NGX_Parameter_OutHeight, g.outH);
        p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u); p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        p->Set(NVSDK_NGX_Parameter_PerfQualityValue, (int)UInt(source, NVSDK_NGX_Parameter_PerfQualityValue,
                                                           NVSDK_NGX_PerfQuality_Value_MaxPerf));
        // LDR biased carrier, constant unit exposure, no auto-exposure/sharpening. No main-game presets
        // or feature handle are overwritten. NGX is called directly, bypassing OptiScaler's NR hooks.
        p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, g.flags);
        const auto result = NVNGXProxy::D3D12_CreateFeature()(cmd, NVSDK_NGX_Feature_SuperSampling, p, &g.feature);
        if (result != NVSDK_NGX_Result_Success || !g.feature)
        { g.failed = true; Say("private DLSS creation failed: " + std::to_string((unsigned)result)); return; }
        DlssNrConstants unit {}; unit.Mode = DlssNrMode_UnitExposure; unit.Width = unit.Height = 1;
        if (!g.codec->DispatchPass(cmd, unit, g.edited, nullptr, nullptr, nullptr, nullptr, g.exposure, nullptr))
        { g.failed = true; Say("private exposure initialization failed"); return; }
        Barrier(cmd, g.exposure, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g.createEpoch = epoch;
        Say("private DLSS created; waiting for a later submission epoch");
        return;
    }
    if (epoch == g.createEpoch) return;

    const auto arrival = cfg.ColorResourceBarrier.has_value() ?
        (D3D12_RESOURCE_STATES)cfg.ColorResourceBarrier.value() : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Barrier(cmd, color, arrival, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, g.edited, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    CopyActiveColor(cmd, g.edited, color, *active);
    Barrier(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, arrival);
    Barrier(cmd, g.edited, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    DlssNrFrameInfo frame {};
    frame.BeforeUpscale = frame.PrivateColorCopy = true;
    frame.SubmissionEpoch = epoch;
    frame.RenderSubrectWidth = g.w; frame.RenderSubrectHeight = g.h;
    frame.DepthInverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    frame.ColourIsLinearHdr = (UInt(source, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags) &
        NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0 && FormatCanHoldLinearHdr(outDesc.Format);
    frame.Reset = UInt(source, NVSDK_NGX_Parameter_Reset) != 0 || g.reset;
    frame.MvScaleX = Float(source, NVSDK_NGX_Parameter_MV_Scale_X, 1);
    frame.MvScaleY = Float(source, NVSDK_NGX_Parameter_MV_Scale_Y, 1);
    frame.PreExposure = std::max(Float(source, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1), 1e-4f);
    frame.ExposureTexture = GetResource(source, NVSDK_NGX_Parameter_ExposureTexture, "ExposureTexture");
    g_nr.exposureOfferedNow = frame.ExposureTexture != nullptr;
    g_nr.exposureEverOffered = g_nr.exposureEverOffered || g_nr.exposureOfferedNow;
    ++g_nr.exposureFrames;
    if (!g_compose) g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", g.device);
    const auto before = g_nr.successfulDispatches;
    g_compose->Dispatch(cmd, g.edited, depth, motion, g.edited, frame, queue);
    const bool evaluated = g_nr.successfulDispatches != before;
    if (evaluated)
    {
        ScopedNrStateEnvelope envelope(cmd);
        if (g.smallReadable)
            Barrier(cmd, g.residualInput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cmd, color, arrival, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        DlssNrConstants encode {}; encode.Mode = DlssNrMode_EncodeResidual;
        encode.Width = g.w; encode.Height = g.h; encode.ExposurePreMul = frame.PreExposure;
        const bool ok = g.codec->DispatchPass(cmd, encode, color, g.edited, nullptr, nullptr, nullptr, g.residualInput, nullptr);
        Barrier(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, arrival);
        Barrier(cmd, g.residualInput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g.smallReadable = true;
        if (ok)
        {
            auto* p = g.parameters;
            p->Set(NVSDK_NGX_Parameter_Color, g.residualInput); p->Set(NVSDK_NGX_Parameter_Output, g.residualOutput);
            p->Set(NVSDK_NGX_Parameter_Depth, depth); p->Set(NVSDK_NGX_Parameter_MotionVectors, motion);
            p->Set(NVSDK_NGX_Parameter_ExposureTexture, g.exposure);
            p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, g.w);
            p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, g.h);
            p->Set(NVSDK_NGX_Parameter_Reset, (unsigned)(frame.Reset || g.reset));
            p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, Float(source, NVSDK_NGX_Parameter_Jitter_Offset_X, 0));
            p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, Float(source, NVSDK_NGX_Parameter_Jitter_Offset_Y, 0));
            p->Set(NVSDK_NGX_Parameter_MV_Scale_X, frame.MvScaleX);
            p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, frame.MvScaleY);
            p->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, Float(source, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 16.67f));
            p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
            p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
            p->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
            pending = { cmd, source, output, epoch, frame.PreExposure };
        }
    }
    else { g.reset = true; Say("waiting for NR evaluation; clean SR frame retained"); }
    Barrier(cmd, g.edited, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void After(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* source, unsigned long long epoch)
{
    const auto pair = pending;
    pending = {}; // Consume once, only for the immediately matching successful upscale.
    if (!current || current->failed || pair.cmd != cmd || pair.caller != source || pair.epoch != epoch ||
        pair.output != GetResource(source, NVSDK_NGX_Parameter_Output, "DLSSD.Output"))
    { if (current) current->reset = true; return; }
    auto& g = *current;
    const auto& cfg = *Config::Instance();
    if ((cfg.RestoreComputeSignature.value_or_default() || cfg.RestoreGraphicSignature.value_or_default()) &&
        !D3D12Hooks::CanRestoreRootSignature(cmd))
    { g.reset = true; Say("inactive: game state cannot be restored after SR"); return; }
    Use use(g, cmd);
    if (!use.valid) { g.reset = true; Say("waiting for GPU completion slots; clean SR frame retained"); return; }
    ScopedNrStateEnvelope envelope(cmd);
    const auto result = NVNGXProxy::D3D12_EvaluateFeature()(cmd, g.feature, g.parameters, nullptr);
    if (result != NVSDK_NGX_Result_Success)
    { g.failed = true; Say("private DLSS evaluation failed: " + std::to_string((unsigned)result)); return; }
    g.reset = false;
    Barrier(cmd, g.residualOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const auto arrival = cfg.OutputResourceBarrier.has_value() ?
        (D3D12_RESOURCE_STATES)cfg.OutputResourceBarrier.value() : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Barrier(cmd, pair.output, arrival, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, g.clean, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(g.clean, pair.output);
    Barrier(cmd, g.clean, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DlssNrConstants apply {}; apply.Mode = DlssNrMode_ApplyResidual;
    apply.Width = g.outW; apply.Height = g.outH; apply.ExposurePreMul = pair.scale;
    const bool ok = g.codec->DispatchPass(cmd, apply, g.clean, g.residualOutput, nullptr, nullptr, nullptr, g.composed, nullptr);
    if (ok)
    {
        Barrier(cmd, g.composed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmd, pair.output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(pair.output, g.composed);
        Barrier(cmd, pair.output, D3D12_RESOURCE_STATE_COPY_DEST, arrival);
        Barrier(cmd, g.composed, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Say("running: " + std::to_string(g.w) + "x" + std::to_string(g.h) + " contribution -> private DLSS -> " +
            std::to_string(g.outW) + "x" + std::to_string(g.outH) + "; applied after SR");
    }
    else { Barrier(cmd, pair.output, D3D12_RESOURCE_STATE_COPY_SOURCE, arrival); Say("composition failed; clean frame retained"); }
    Barrier(cmd, g.clean, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cmd, g.residualOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void Shutdown()
{
    Cancel();
    if (current) retired.push_back(std::move(current));
    Collect();
    // Never free feature histories, descriptors or surfaces referenced by an unsubmitted/in-flight
    // list. At shutdown only, retain uncompleted generations for process teardown rather than UAF.
    for (auto& g : retired) (void)g.release();
    retired.clear();
}
} // namespace DeferredSr

std::string DeferredDlssStatus()
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    return DeferredSr::status;
}
