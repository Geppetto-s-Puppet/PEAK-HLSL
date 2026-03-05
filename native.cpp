// native.cpp
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <cstdio>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

#define EXPORT extern "C" __declspec(dllexport)

static void DbgLog(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsprintf_s(buf, fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

#define CHK(hr, label) \
    if (FAILED(hr)) { DbgLog("[PEAK_SSGI] FAILED %s hr=0x%08X", label, (unsigned)hr); return false; }

// ============================================================
// シェーダー
// ============================================================

// ★ UV の Y を反転しているのは Unity URP (DX12) のバックバッファが
//   DirectX 標準（左上原点）に対して上下逆で返ってくるため。
static const char* VS = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
VSOut main(uint id : SV_VertexID)
{
    // フルスクリーントライアングル（NDC空間）
    float2 pos[3] = { float2(-1, 1), float2(3, 1), float2(-1,-3) };
    // UV の Y を反転 (Unity バックバッファが上下逆)
    float2 uv[3]  = { float2(0.0, 1.0), float2(2.0, 1.0), float2(0.0,-1.0) };
    VSOut o;
    o.pos = float4(pos[id], 0, 1);
    o.uv  = uv[id];
    return o;
}
)";

// mode:
//   1 = パススルー (ゲーム画面そのまま)
//   2 = 深度バッファ可視化
//   3 = 法線バッファ可視化
//   その他 = 黒 (非表示時は呼ばれないが念のため)
static const char* PS = R"(
Texture2D<float4> tColor  : register(t0);
Texture2D<float>  tDepth  : register(t1);
Texture2D<float4> tNormal : register(t2);
SamplerState      sLinear : register(s0);

cbuffer CB : register(b0) { int mode; int pad0; int pad1; int pad2; }

float4 main(float4 svpos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    if (mode == 1) // パススルー
        return tColor.Sample(sLinear, uv);

    if (mode == 2) { // 深度可視化 (リバースZなので近=1、遠=0)
        float d = tDepth.Sample(sLinear, uv).r;
        return float4(d, d, d, 1.0);
    }

    if (mode == 3) { // 法線可視化 ([-1,1] -> [0,1])
        float3 n = tNormal.Sample(sLinear, uv).xyz * 0.5 + 0.5;
        return float4(n, 1.0);
    }

    return float4(0.0, 0.0, 0.0, 1.0);
}
)";

// ============================================================
// 定数 / グローバル
// ============================================================

static const UINT FRAMES = 2;
static const UINT SRV_COUNT = 3;

static HWND   g_hwnd = nullptr;
static int    g_width = 0;
static int    g_height = 0;

static ID3D12Device* g_device = nullptr;
static ID3D12CommandQueue* g_queue = nullptr;
static IDXGISwapChain3* g_swapChain = nullptr;
static ID3D12DescriptorHeap* g_rtvHeap = nullptr;
static ID3D12Resource* g_rtvRes[FRAMES] = {};
static ID3D12CommandAllocator* g_alloc[FRAMES] = {};   // フレーム別アロケーター
static ID3D12GraphicsCommandList* g_cmdList = nullptr;
static ID3D12RootSignature* g_rootSig = nullptr;
static ID3D12PipelineState* g_pso = nullptr;
static ID3D12Fence* g_fence = nullptr;
static UINT64                    g_fenceVal = 0;
static UINT64                    g_frameFenceValues[FRAMES] = {}; // フレーム別フェンス値
static HANDLE                    g_fenceEvent = nullptr;
static UINT                      g_rtvStep = 0;
static UINT                      g_frameIdx = 0;

static ID3D12DescriptorHeap* g_srvHeap = nullptr;
static UINT                      g_srvStep = 0;
static bool                      g_srvReady = false;

static ID3D12Resource* g_cbRes = nullptr;
static int* g_cbPtr = nullptr;

static CRITICAL_SECTION          g_cs;
static bool                      g_pendingUpdate = false;
static ID3D12Resource* g_pendingColor = nullptr;
static ID3D12Resource* g_pendingDepth = nullptr;
static ID3D12Resource* g_pendingNormal = nullptr;
// ▼ 削除
// static ID3D12Resource* g_srvColor  = nullptr;
// static ID3D12Resource* g_srvDepth  = nullptr;
// static ID3D12Resource* g_srvNormal = nullptr;

// ▼ 追加 (aliased な Unity リソースの自前コピー)
static ID3D12Resource* g_ownColor = nullptr;
static ID3D12Resource* g_ownDepth = nullptr;
static ID3D12Resource* g_ownNormal = nullptr;
static D3D12_RESOURCE_STATES g_ownColorState = D3D12_RESOURCE_STATE_COPY_DEST;
static D3D12_RESOURCE_STATES g_ownDepthState = D3D12_RESOURCE_STATE_COPY_DEST;
static D3D12_RESOURCE_STATES g_ownNormalState = D3D12_RESOURCE_STATE_COPY_DEST;

// シェーダーモード (C# から NativeOverlay_SetMode で書き込む)
static volatile int  g_shaderMode = 0;
// 新規テクスチャが届いたらレンダリングをトリガー
static volatile bool g_needsRender = false;

// ステータス (C# 側 GetStatus から読む)
static volatile bool g_statusSrvReady = false;
static volatile bool g_statusUsingOwn = false;
static volatile bool g_statusInitDone = false;
static volatile int  g_statusInitResult = 0; // 0=未実行 1=成功 -1=失敗

// ★ Unity フレーム同期用イベント
static HANDLE g_updateEvent = nullptr;

// ============================================================
// ユーティリティ
// ============================================================

template<typename T>
static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static DXGI_FORMAT ToSRVFormat(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:          return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:          return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:     return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:                                 return fmt;
    }
}

// GPU 完全待機（解放・フラッシュ時専用、通常描画には使わない）
static void WaitGpu()
{
    if (!g_queue || !g_fence) return;
    g_queue->Signal(g_fence, ++g_fenceVal);
    if (g_fence->GetCompletedValue() < g_fenceVal)
    {
        g_fence->SetEventOnCompletion(g_fenceVal, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
}

// ============================================================
// SRV 登録
// ============================================================
static void RegisterSRV(ID3D12Resource* res, UINT slot)
{
    if (!res || !g_device || !g_srvHeap) return;

    DXGI_FORMAT fmt = ToSRVFormat(res->GetDesc().Format);
    DbgLog("[PEAK_SSGI] RegisterSRV slot=%u fmt=%u res=%p", slot, (unsigned)fmt, res);

    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Format = fmt;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE handle = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)slot * g_srvStep;
    g_device->CreateShaderResourceView(res, &desc, handle);
}

// ============================================================
// コピー先フォーマット変換
// Depth フォーマットを typeless に変換することで
//   ① COPY_DEST として作成可能
//   ② SRV (R32_FLOAT 等) として作成可能
// を両立する
// ============================================================
static DXGI_FORMAT ToCopyDestFormat(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_D32_FLOAT:            return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:            return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
    default:                               return fmt;
    }
}

// src に合わせた自前 committed resource を作成 / 再利用
// サイズ変化時のみ WaitGpu + 再作成
static bool EnsureOwnResource(ID3D12Resource*& own,
    D3D12_RESOURCE_STATES& ownState,
    ID3D12Resource* src)
{
    if (!src) { SafeRelease(own); return false; }
    if (!g_device) return false;

    D3D12_RESOURCE_DESC sd = src->GetDesc();
    if (own)
    {
        D3D12_RESOURCE_DESC od = own->GetDesc();
        if (od.Width == sd.Width && od.Height == sd.Height)
            return true;           // サイズ不変 → 再利用
        WaitGpu();                 // サイズ変化 → 安全に解放
        SafeRelease(own);
    }

    D3D12_RESOURCE_DESC d = sd;
    d.Format = ToCopyDestFormat(sd.Format);
    d.Flags = D3D12_RESOURCE_FLAG_NONE; // DENY_SHADER_RESOURCE / ALLOW_DEPTH_STENCIL を除去

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = g_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&own));
    if (FAILED(hr))
    {
        DbgLog("[PEAK_SSGI] EnsureOwnResource FAILED hr=0x%08X fmt=%u->%u",
            (unsigned)hr, (unsigned)sd.Format, (unsigned)d.Format);
        return false;
    }
    ownState = D3D12_RESOURCE_STATE_COPY_DEST;
    DbgLog("[PEAK_SSGI] EnsureOwnResource OK %p  %ux%u fmt=%u",
        (void*)own, (unsigned)d.Width, d.Height, (unsigned)d.Format);
    return true;
}

// バリアヘルパー (StateBefore == StateAfter なら何もしない)
static void Barrier(ID3D12GraphicsCommandList* cmd,
    ID3D12Resource* res,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    if (!res || before == after) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
}


// ============================================================
// デバイスリソース解放
// ============================================================
static void ReleaseDeviceResources()
{
    WaitGpu();
    SafeRelease(g_cmdList);
    for (UINT i = 0; i < FRAMES; ++i) SafeRelease(g_alloc[i]);
    SafeRelease(g_pso);
    SafeRelease(g_rootSig);
    // ▼ 追加: 自前コピーリソース解放
    SafeRelease(g_ownColor);
    SafeRelease(g_ownDepth);
    SafeRelease(g_ownNormal);
    SafeRelease(g_srvHeap);
    SafeRelease(g_rtvHeap);
    for (UINT i = 0; i < FRAMES; ++i) SafeRelease(g_rtvRes[i]);
    SafeRelease(g_swapChain);
    SafeRelease(g_queue);
    SafeRelease(g_fence);
    if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    if (g_cbPtr && g_cbRes) { g_cbRes->Unmap(0, nullptr); g_cbPtr = nullptr; }
    SafeRelease(g_cbRes);

    g_srvReady = false;
    g_statusSrvReady = false;
    for (UINT i = 0; i < FRAMES; ++i) g_frameFenceValues[i] = 0;
    g_fenceVal = 0;
}

// ============================================================
// ルートシグネチャ & PSO
// ============================================================
static bool BuildRootSigAndPSO()
{
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = SRV_COUNT;
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].Descriptor.ShaderRegister = 0;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &sampler;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* sigBlob = nullptr;
    ID3DBlob* errBlob = nullptr;
    D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (errBlob) { DbgLog("[PEAK_SSGI] RootSig err: %s", (char*)errBlob->GetBufferPointer()); errBlob->Release(); }
    if (!sigBlob) return false;

    HRESULT hr = g_device->CreateRootSignature(
        0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_rootSig));
    sigBlob->Release();
    CHK(hr, "CreateRootSignature");

    ID3DBlob* vsBlob = nullptr, * vsErr = nullptr;
    ID3DBlob* psBlob = nullptr, * psErr = nullptr;
    D3DCompile(VS, strlen(VS), "VS", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &vsErr);
    D3DCompile(PS, strlen(PS), "PS", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &psErr);
    if (vsErr) { DbgLog("[PEAK_SSGI] VS err: %s", (char*)vsErr->GetBufferPointer()); vsErr->Release(); }
    if (psErr) { DbgLog("[PEAK_SSGI] PS err: %s", (char*)psErr->GetBufferPointer()); psErr->Release(); }
    if (!vsBlob || !psBlob) { SafeRelease(vsBlob); SafeRelease(psBlob); return false; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_rootSig;
    pd.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    pd.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    HRESULT hr2 = g_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_pso));
    vsBlob->Release();
    psBlob->Release();
    CHK(hr2, "CreateGraphicsPipelineState");

    return true;
}

// ============================================================
// D3D12 初期化 (Unity デバイスを受け取る)
// ============================================================
static bool InitD3D12(ID3D12Device* deviceToUse)
{
    DbgLog("[PEAK_SSGI] InitD3D12 begin deviceToUse=%p", deviceToUse);

    if (!deviceToUse)
    {
        DbgLog("[PEAK_SSGI] InitD3D12: deviceToUse == nullptr");
        return false;
    }

    ReleaseDeviceResources();

    deviceToUse->AddRef();
    g_device = deviceToUse;
    DbgLog("[PEAK_SSGI] using Unity device %p", g_device);

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue));
    CHK(hr, "CreateCommandQueue");

    IDXGIFactory4* factory = nullptr;
    hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    CHK(hr, "CreateDXGIFactory1");

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.BufferCount = FRAMES;
    sd.Width = g_width;
    sd.Height = g_height;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;

    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hrSC = factory->CreateSwapChainForHwnd(g_queue, g_hwnd, &sd, nullptr, nullptr, &sc1);
    factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();
    CHK(hrSC, "CreateSwapChainForHwnd");
    sc1->QueryInterface(IID_PPV_ARGS(&g_swapChain));
    sc1->Release();

    g_frameIdx = g_swapChain->GetCurrentBackBufferIndex();

    // RTV ヒープ
    D3D12_DESCRIPTOR_HEAP_DESC rtvHd = {};
    rtvHd.NumDescriptors = FRAMES;
    rtvHd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = g_device->CreateDescriptorHeap(&rtvHd, IID_PPV_ARGS(&g_rtvHeap));
    CHK(hr, "CreateDescriptorHeap(RTV)");
    g_rtvStep = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FRAMES; i++)
    {
        hr = g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_rtvRes[i]));
        CHK(hr, "GetBuffer");
        g_device->CreateRenderTargetView(g_rtvRes[i], nullptr, rtvHandle);
        rtvHandle.ptr += g_rtvStep;
    }

    // フレーム別コマンドアロケーター（フリッカー対策）
    for (UINT i = 0; i < FRAMES; i++)
    {
        hr = g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i]));
        CHK(hr, "CreateCommandAllocator");
    }
    hr = g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS(&g_cmdList));
    CHK(hr, "CreateCommandList");
    g_cmdList->Close();

    // フェンス
    hr = g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    CHK(hr, "CreateFence");
    g_fenceVal = 0;
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    for (UINT i = 0; i < FRAMES; i++) g_frameFenceValues[i] = 0;

    // SRV ヒープ
    D3D12_DESCRIPTOR_HEAP_DESC srvHd = {};
    srvHd.NumDescriptors = SRV_COUNT;
    srvHd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = g_device->CreateDescriptorHeap(&srvHd, IID_PPV_ARGS(&g_srvHeap));
    CHK(hr, "CreateDescriptorHeap(SRV)");
    g_srvStep = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (!BuildRootSigAndPSO()) return false;

    // コンスタントバッファ（mode 格納用）
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = 256;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = g_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_cbRes));
    CHK(hr, "CreateCommittedResource(CB)");
    g_cbRes->Map(0, nullptr, (void**)&g_cbPtr);
    *g_cbPtr = 0;

    g_srvReady = true;
    g_statusSrvReady = true;
    g_statusUsingOwn = false;
    g_statusInitDone = true;
    g_statusInitResult = 1;

    DbgLog("[PEAK_SSGI] InitD3D12 done  device=%p  srvReady=true", (void*)g_device);
    return true;
}

// ============================================================
// Pending テクスチャを取り出す
// GPU コマンドの記録は Render() 内で行う
// ============================================================
static void FlushPendingTextures(ID3D12Resource*& outCol,
    ID3D12Resource*& outDep,
    ID3D12Resource*& outNor)
{
    outCol = outDep = outNor = nullptr;

    EnterCriticalSection(&g_cs);
    bool            doUpdate = g_pendingUpdate;
    ID3D12Resource* col = g_pendingColor;
    ID3D12Resource* dep = g_pendingDepth;
    ID3D12Resource* nor = g_pendingNormal;
    g_pendingColor = g_pendingDepth = g_pendingNormal = nullptr;
    g_pendingUpdate = false;
    LeaveCriticalSection(&g_cs);

    if (!doUpdate) return;
    if (g_cbPtr) *g_cbPtr = g_shaderMode;

    outCol = col;
    outDep = dep;
    outNor = nor;
}

// ============================================================
// レンダリング(フレーム別アロケーターで GPU 同期最小化)
// ============================================================
// レンダリング
//   ① Unity aliased リソース → 自前 committed リソースへ CopyResource
//   ② 自前リソースを SRV としてフルスクリーン描画
//   ※ コピーと描画を同一コマンドリストに記録することで
//      GPU 側での実行順が保証され、チラつきを防ぐ
// ============================================================
static void Render()
{
    // 1. Pending テクスチャ取り出し
    ID3D12Resource* newCol = nullptr;
    ID3D12Resource* newDep = nullptr;
    ID3D12Resource* newNor = nullptr;
    FlushPendingTextures(newCol, newDep, newNor);

    if (!g_cmdList || !g_pso)
    {
        SafeRelease(newCol); SafeRelease(newDep); SafeRelease(newNor);
        return;
    }

    // 2. 自前コピーリソースを確保 (サイズ変化時のみ再作成)
    bool doCopy = (newCol != nullptr);
    if (doCopy)
    {
        bool ok = EnsureOwnResource(g_ownColor, g_ownColorState, newCol);
        // depth / normal は nullptr でも可
        EnsureOwnResource(g_ownDepth, g_ownDepthState, newDep);
        EnsureOwnResource(g_ownNormal, g_ownNormalState, newNor);
        if (!ok) doCopy = false;
    }

    // 自前カラーリソースがなければ描画スキップ
    if (!g_ownColor)
    {
        SafeRelease(newCol); SafeRelease(newDep); SafeRelease(newNor);
        return;
    }

    // 3. このフレームの前回コマンドが GPU で完了するまで待機
    if (g_frameFenceValues[g_frameIdx] != 0 &&
        g_fence->GetCompletedValue() < g_frameFenceValues[g_frameIdx])
    {
        g_fence->SetEventOnCompletion(g_frameFenceValues[g_frameIdx], g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }

    g_alloc[g_frameIdx]->Reset();
    g_cmdList->Reset(g_alloc[g_frameIdx], g_pso);

    // 4. CopyResource: Unity aliased リソース → 自前リソース
    //    Unity の RenderGraph は cross-queue 共有リソースを COMMON 状態で渡す前提
    if (doCopy)
    {
        // Unity リソース: COMMON → COPY_SOURCE
        if (newCol) Barrier(g_cmdList, newCol, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (newDep) Barrier(g_cmdList, newDep, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (newNor) Barrier(g_cmdList, newNor, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);

        // 自前リソース: 現在状態 → COPY_DEST
        Barrier(g_cmdList, g_ownColor, g_ownColorState, D3D12_RESOURCE_STATE_COPY_DEST);
        g_ownColorState = D3D12_RESOURCE_STATE_COPY_DEST;
        if (g_ownDepth) { Barrier(g_cmdList, g_ownDepth, g_ownDepthState, D3D12_RESOURCE_STATE_COPY_DEST); g_ownDepthState = D3D12_RESOURCE_STATE_COPY_DEST; }
        if (g_ownNormal) { Barrier(g_cmdList, g_ownNormal, g_ownNormalState, D3D12_RESOURCE_STATE_COPY_DEST); g_ownNormalState = D3D12_RESOURCE_STATE_COPY_DEST; }

        // コピー実行
        if (newCol)              g_cmdList->CopyResource(g_ownColor, newCol);
        if (newDep && g_ownDepth)  g_cmdList->CopyResource(g_ownDepth, newDep);
        if (newNor && g_ownNormal) g_cmdList->CopyResource(g_ownNormal, newNor);

        // Unity リソース: COPY_SOURCE → COMMON に戻す
        if (newCol) Barrier(g_cmdList, newCol, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (newDep) Barrier(g_cmdList, newDep, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (newNor) Barrier(g_cmdList, newNor, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

        // 自前リソース: COPY_DEST → PIXEL_SHADER_RESOURCE
        Barrier(g_cmdList, g_ownColor, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_ownColorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (g_ownDepth) { Barrier(g_cmdList, g_ownDepth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); g_ownDepthState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; }
        if (g_ownNormal) { Barrier(g_cmdList, g_ownNormal, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); g_ownNormalState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; }

        // SRV を自前リソースに向けて更新
        RegisterSRV(g_ownColor, 0);
        if (g_ownDepth)  RegisterSRV(g_ownDepth, 1);
        if (g_ownNormal) RegisterSRV(g_ownNormal, 2);

        g_statusSrvReady = g_srvReady;
    }

    // Unity リソースは即座に解放 (もう参照しない)
    SafeRelease(newCol); SafeRelease(newDep); SafeRelease(newNor);

    // 5. フルスクリーン描画
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_rtvRes[g_frameIdx];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_cmdList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)g_frameIdx * g_rtvStep;

    float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    g_cmdList->ClearRenderTargetView(rtv, clear, 0, nullptr);
    g_cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT vp = { 0, 0, (float)g_width, (float)g_height, 0.0f, 1.0f };
    D3D12_RECT     scissor = { 0, 0, g_width, g_height };
    g_cmdList->RSSetViewports(1, &vp);
    g_cmdList->RSSetScissorRects(1, &scissor);
    g_cmdList->SetGraphicsRootSignature(g_rootSig);
    g_cmdList->SetPipelineState(g_pso);

    if (g_srvReady && g_ownColor && g_srvHeap)
    {
        ID3D12DescriptorHeap* heaps[] = { g_srvHeap };
        g_cmdList->SetDescriptorHeaps(1, heaps);
        g_cmdList->SetGraphicsRootDescriptorTable(
            0, g_srvHeap->GetGPUDescriptorHandleForHeapStart());
    }

    if (g_cbRes)
        g_cmdList->SetGraphicsRootConstantBufferView(1, g_cbRes->GetGPUVirtualAddress());

    g_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_cmdList->DrawInstanced(3, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_cmdList->ResourceBarrier(1, &barrier);
    g_cmdList->Close();

    ID3D12CommandList* lists[] = { g_cmdList };
    g_queue->ExecuteCommandLists(1, lists);

    g_swapChain->Present(0, 0);

    g_frameFenceValues[g_frameIdx] = ++g_fenceVal;
    g_queue->Signal(g_fence, g_fenceVal);
    g_frameIdx = g_swapChain->GetCurrentBackBufferIndex();
}

// ============================================================
// ウィンドウプロシージャ & スレッド
// ============================================================
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

static DWORD WINAPI Thread(LPVOID)
{
    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"PEAK_SSGI_Overlay";
    RegisterClassExW(&wc);

    g_width = GetSystemMetrics(SM_CXSCREEN);
    g_height = GetSystemMetrics(SM_CYSCREEN);

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"PEAK_SSGI_Overlay", L"PEAK SSGI",
        WS_POPUP,
        0, 0, g_width, g_height,
        nullptr, nullptr, inst, nullptr);

    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
    ShowWindow(g_hwnd, SW_HIDE);

    DbgLog("[PEAK_SSGI] Thread started, waiting for Unity device...");
    while (!g_statusInitDone) Sleep(10);

    if (g_statusInitResult != 1)
    {
        DbgLog("[PEAK_SSGI] AcquireUnityDevice failed, exiting thread");
        return 1;
    }

    DbgLog("[PEAK_SSGI] InitD3D12 succeeded, entering render loop");

    MSG msg;
    while (true)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            // 新規テクスチャが届いたときだけ描画（余計な再描画を避ける）
            if (IsWindowVisible(g_hwnd) && g_needsRender)
            {
                g_needsRender = false;
                Render();
            }
            else
            {
                // ★ 変更前: if (IsWindowVisible...) { Render(); } else { Sleep(1); }
                // ★ 変更後: Unityフレームのシグナルを待ってからRender
                DWORD wait = WaitForSingleObject(g_updateEvent, 16); // 最大16ms待機
                if (wait == WAIT_OBJECT_0 && IsWindowVisible(g_hwnd))
                {
                    Render();
                }
            }
        }
    }

    DbgLog("[PEAK_SSGI] Thread exiting");
    ReleaseDeviceResources();
    return 0;
}

// ============================================================
// エクスポート
// ============================================================

EXPORT void NativeOverlay_Start()
{
    InitializeCriticalSection(&g_cs);
    g_updateEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr); // ★ 追加
    CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr);
}

EXPORT void NativeOverlay_Show() { if (g_hwnd) ShowWindow(g_hwnd, SW_SHOWNA); }
EXPORT void NativeOverlay_Hide() { if (g_hwnd) ShowWindow(g_hwnd, SW_HIDE); }

/// <summary>
/// シェーダーモードを設定する。描画スレッドに非同期で伝達。
/// mode: 0=黒, 1=パススルー, 2=深度, 3=法線
/// </summary>
EXPORT void NativeOverlay_SetMode(int mode)
{
    g_shaderMode = mode;   // volatile: 描画スレッドが FlushPendingTextures で読む
    g_needsRender = true;   // モード変更時も再描画
    DbgLog("[PEAK_SSGI] SetMode %d", mode);
}

// ============================================================
// Unity デバイス取得（C# の AcquireDeviceAfterStart から呼ぶ）
// ============================================================
EXPORT int NativeOverlay_AcquireUnityDevice(void* sampleResourcePtr)
{
    if (!sampleResourcePtr)
    {
        DbgLog("[PEAK_SSGI] AcquireUnityDevice: sampleResourcePtr == nullptr");
        g_statusInitDone = true;
        g_statusInitResult = -1;
        return -1;
    }

    ID3D12Resource* res = static_cast<ID3D12Resource*>(sampleResourcePtr);

    ID3D12Device* device = nullptr;
    HRESULT hr = res->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device)
    {
        DbgLog("[PEAK_SSGI] GetDevice(ID3D12Device) failed hr=0x%08X, trying ID3D12Device5", (unsigned)hr);
        ID3D12Device5* dev5 = nullptr;
        hr = res->GetDevice(__uuidof(ID3D12Device5), (void**)&dev5);
        if (SUCCEEDED(hr) && dev5)
        {
            device = dev5;
            DbgLog("[PEAK_SSGI] got ID3D12Device5 %p", device);
        }
        else
        {
            DbgLog("[PEAK_SSGI] GetDevice(ID3D12Device5) failed hr=0x%08X", (unsigned)hr);
        }
    }
    else
    {
        DbgLog("[PEAK_SSGI] got ID3D12Device %p", device);
    }

    if (!device)
    {
        g_statusInitDone = true;
        g_statusInitResult = -1;
        return (int)hr;
    }

    bool ok = InitD3D12(device);
    device->Release(); // GetDevice で増えた参照を解放（InitD3D12 内で AddRef 済み）

    g_statusInitDone = true;
    g_statusInitResult = ok ? 1 : -1;

    DbgLog("[PEAK_SSGI] AcquireUnityDevice %s", ok ? "OK" : "FAILED");
    return ok ? 0 : -3;
}

// ============================================================
// ステータス取得
//  bit0 = initDone
//  bit1 = initSuccess
//  bit2 = usingOwnDevice (常に false)
//  bit3 = srvReady
//  bits4-7 = shaderMode
// ============================================================
EXPORT int NativeOverlay_GetStatus()
{
    int s = 0;
    if (g_statusInitDone)        s |= (1 << 0);
    if (g_statusInitResult == 1) s |= (1 << 1);
    if (g_statusUsingOwn)        s |= (1 << 2);
    if (g_statusSrvReady)        s |= (1 << 3);
    s |= ((g_shaderMode & 0xF) << 4);
    return s;
}

// ============================================================
// テクスチャ更新（C# の EndOfFrameCapture から呼ぶ）
//  colorPtr/depthPtr/normalPtr は ID3D12Resource*
// ============================================================
EXPORT void NativeOverlay_UpdateTextures(
    void* colorPtr,
    void* depthPtr,
    void* normalPtr,
    int   screenW,
    int   screenH)
{
    if (screenW > 0 && screenH > 0 && (screenW != g_width || screenH != g_height))
    {
        g_width = screenW;
        g_height = screenH;
    }

    EnterCriticalSection(&g_cs);

    ID3D12Resource* newCol = static_cast<ID3D12Resource*>(colorPtr);
    ID3D12Resource* newDep = static_cast<ID3D12Resource*>(depthPtr);
    ID3D12Resource* newNor = static_cast<ID3D12Resource*>(normalPtr);

    if (newCol) newCol->AddRef();
    if (newDep) newDep->AddRef();
    if (newNor) newNor->AddRef();

    SafeRelease(g_pendingColor);
    SafeRelease(g_pendingDepth);
    SafeRelease(g_pendingNormal);

    g_pendingColor = newCol;
    g_pendingDepth = newDep;
    g_pendingNormal = newNor;
    g_pendingUpdate = true;

    LeaveCriticalSection(&g_cs);

    // 新テクスチャが届いたら描画スレッドに通知

    // ★ 変更前: g_needsRender = true;
    // ★ 変更後:
    SetEvent(g_updateEvent);  // Unityフレームごとに1回だけRenderをトリガー
}