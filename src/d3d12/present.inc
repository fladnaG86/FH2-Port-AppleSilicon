/* ---------------- swap chain: spatial upscaler ---------------- */

/* Included by d3d12.c. azimuth.exe imports CreateDXGIFactory1/2 from dxgi.dll;
 * the import slots are pointed at our wrappers, which hook the factory's swap
 * chain creation and then the swap chain itself.
 *
 * Azimuth renders the game at its "resolution" setting and scales that frame
 * into the swap chain, which is as big as the window. With Retina mode on the
 * window has the display's real pixels (3456x2234 on a 16" MacBook Pro), so a
 * 1440p frame is stretched by Azimuth's own scaler. The upscaler hands Azimuth
 * back buffers of our own ("proxies") sized so the game frame lands in them
 * 1:1, and on Present scales the proxy into the real back buffer with two
 * full-screen passes modelled on AMD FidelityFX Super Resolution 1 (MIT):
 * EASU (edge-adaptive Lanczos-like upscale) and RCAS (contrast-adaptive
 * sharpening). Azimuth's first CreateSwapChainForHwnd asks for the render size
 * (2560x1440) before resizing to the window: that size is the proxy's content.
 *
 * FH2_UPSCALE=0 turns the upscaler off, FH2_SHARP=<stops> sets the sharpening
 * (0 = most, 2 = soft, default 0.25), FH2_PRESENT=0 leaves dxgi alone. */

#include <dxgi1_4.h>
#include <stdlib.h>
extern double __cdecl pow(double, double);

typedef HRESULT (WINAPI *create_factory1_fn)(REFIID, void **);
typedef HRESULT (WINAPI *create_factory2_fn)(UINT, REFIID, void **);

static create_factory1_fn real_create_factory1;
static create_factory2_fn real_create_factory2;

static HRESULT (STDMETHODCALLTYPE *real_fac_create_sc)(IDXGIFactory2 *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
static HRESULT (STDMETHODCALLTYPE *real_fac_create_hwnd)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                                         const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);

static HRESULT (STDMETHODCALLTYPE *real_sc_present)(IDXGISwapChain3 *, UINT, UINT);
static HRESULT (STDMETHODCALLTYPE *real_sc_get_buffer)(IDXGISwapChain3 *, UINT, REFIID, void **);
static HRESULT (STDMETHODCALLTYPE *real_sc_get_desc)(IDXGISwapChain3 *, DXGI_SWAP_CHAIN_DESC *);
static HRESULT (STDMETHODCALLTYPE *real_sc_resize)(IDXGISwapChain3 *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static HRESULT (STDMETHODCALLTYPE *real_sc_get_desc1)(IDXGISwapChain3 *, DXGI_SWAP_CHAIN_DESC1 *);
static HRESULT (STDMETHODCALLTYPE *real_sc_present1)(IDXGISwapChain3 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
static HRESULT (STDMETHODCALLTYPE *real_sc_resize1)(IDXGISwapChain3 *, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                                    const UINT *, IUnknown *const *);

static volatile LONG present_count, get_buffer_logs;

#define UP_MAX_BUFFERS 8
#define UP_ALLOCS 3

typedef struct {
    IDXGISwapChain3 *sc;            /* the game's swap chain (not referenced) */
    ID3D12CommandQueue *queue;      /* referenced */
    ID3D12Device *dev;              /* referenced */
    UINT rw, rh;                    /* render size from the first create */
    BOOL on;
    UINT n, pw, ph, dw, dh;         /* buffers, proxy size, real size */
    DXGI_FORMAT fmt;
    ID3D12Resource *proxy[UP_MAX_BUFFERS], *real[UP_MAX_BUFFERS], *mid;
    ID3D12DescriptorHeap *rtv, *srv;
    UINT rtv_inc, srv_inc;
    ID3D12CommandAllocator *alloc[UP_ALLOCS];
    UINT64 alloc_val[UP_ALLOCS];
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    UINT64 fence_val;
    HANDLE event;
    UINT frame;
} upscaler;

static upscaler up;
/* render target views of the proxies, and the factor from window to proxy pixels */
#define UP_RTVS 64
static volatile SIZE_T up_rtv[UP_RTVS];
static volatile float up_sx = 1, up_sy = 1;
static volatile LONG up_draw_logs, up_copy_logs;
#define UP_SMALL 16
static ID3D12Resource *volatile up_small[UP_SMALL];   /* Azimuth's own proxy-sized render targets */
static CRITICAL_SECTION up_cs;
static BOOL up_enabled = TRUE;
static float up_sharp_stops = 0.25f;
static ID3D12RootSignature *up_rs;
static ID3D12PipelineState *up_easu, *up_rcas;
static DXGI_FORMAT up_pso_fmt;

static const char up_hlsl[] =
"cbuffer C : register(b0) { float4 c0; float4 c1; };\n"
"Texture2D<float4> src : register(t0);\n"
"float4 vs(uint id : SV_VertexID) : SV_Position {\n"
"    float2 uv = float2((id << 1) & 2, id & 2);\n"
"    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"}\n"
"float3 tap(int2 p) { return src.Load(int3(clamp(p, int2(0, 0), int2(c1.xy) - 1), 0)).rgb; }\n"
"float luma(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }\n"
/* FsrEasuSet: direction and length from one bilinear quadrant (A above, B left, C centre, D right, E below) */
"void eset(inout float2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {\n"
"    float dc = lD - lC, cb = lC - lB;\n"
"    float lenX = max(abs(dc), abs(cb)); lenX = lenX > 0 ? 1.0 / lenX : 0;\n"
"    float dirX = lD - lB; dir.x += dirX * w;\n"
"    lenX = saturate(abs(dirX) * lenX); lenX *= lenX; len += lenX * w;\n"
"    float ec = lE - lC, ca = lC - lA;\n"
"    float lenY = max(abs(ec), abs(ca)); lenY = lenY > 0 ? 1.0 / lenY : 0;\n"
"    float dirY = lE - lA; dir.y += dirY * w;\n"
"    lenY = saturate(abs(dirY) * lenY); lenY *= lenY; len += lenY * w;\n"
"}\n"
/* FsrEasuTap: one tap of the approximated, rotated and stretched Lanczos-2 kernel */
"void etap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c) {\n"
"    float2 v = float2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len;\n"
"    float d2 = min(v.x * v.x + v.y * v.y, clp);\n"
"    float wB = 0.4 * d2 - 1.0, wA = lob * d2 - 1.0;\n"
"    wB *= wB; wA *= wA;\n"
"    wB = 1.5625 * wB - 0.5625;\n"
"    float w = wB * wA; aC += c * w; aW += w;\n"
"}\n"
"float4 easu(float4 pos : SV_Position) : SV_Target {\n"
"    float2 pp = floor(pos.xy) * c0.xy + c0.zw;\n"
"    float2 fp = floor(pp); pp -= fp;\n"
"    int2 f0 = int2(fp);\n"
"    float3 b = tap(f0 + int2(0, -1)), c = tap(f0 + int2(1, -1));\n"
"    float3 e = tap(f0 + int2(-1, 0)), f = tap(f0), g = tap(f0 + int2(1, 0)), h = tap(f0 + int2(2, 0));\n"
"    float3 i = tap(f0 + int2(-1, 1)), j = tap(f0 + int2(0, 1)), k = tap(f0 + int2(1, 1)), l = tap(f0 + int2(2, 1));\n"
"    float3 n = tap(f0 + int2(0, 2)), o = tap(f0 + int2(1, 2));\n"
"    float bL = luma(b), cL = luma(c), eL = luma(e), fL = luma(f), gL = luma(g), hL = luma(h);\n"
"    float iL = luma(i), jL = luma(j), kL = luma(k), lL = luma(l), nL = luma(n), oL = luma(o);\n"
"    float2 dir = 0; float len = 0;\n"
"    eset(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
"    eset(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
"    eset(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
"    eset(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
"    float dirR = dir.x * dir.x + dir.y * dir.y;\n"
"    bool zro = dirR < 1.0 / 32768.0;\n"
"    dirR = zro ? 1.0 : rsqrt(dirR);\n"
"    dir.x = zro ? 1.0 : dir.x;\n"
"    dir *= dirR;\n"
"    len = len * 0.5; len *= len;\n"
"    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));\n"
"    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
"    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;\n"
"    float clp = 1.0 / lob;\n"
"    float3 mn = min(min(f, g), min(j, k)), mx = max(max(f, g), max(j, k));\n"
"    float3 aC = 0; float aW = 0;\n"
"    etap(aC, aW, float2(0, -1) - pp, dir, len2, lob, clp, b);\n"
"    etap(aC, aW, float2(1, -1) - pp, dir, len2, lob, clp, c);\n"
"    etap(aC, aW, float2(-1, 1) - pp, dir, len2, lob, clp, i);\n"
"    etap(aC, aW, float2(0, 1) - pp, dir, len2, lob, clp, j);\n"
"    etap(aC, aW, float2(0, 0) - pp, dir, len2, lob, clp, f);\n"
"    etap(aC, aW, float2(-1, 0) - pp, dir, len2, lob, clp, e);\n"
"    etap(aC, aW, float2(1, 1) - pp, dir, len2, lob, clp, k);\n"
"    etap(aC, aW, float2(2, 1) - pp, dir, len2, lob, clp, l);\n"
"    etap(aC, aW, float2(2, 0) - pp, dir, len2, lob, clp, h);\n"
"    etap(aC, aW, float2(1, 0) - pp, dir, len2, lob, clp, g);\n"
"    etap(aC, aW, float2(1, 2) - pp, dir, len2, lob, clp, o);\n"
"    etap(aC, aW, float2(0, 2) - pp, dir, len2, lob, clp, n);\n"
"    return float4(min(mx, max(mn, aC / aW)), 1);\n"
"}\n"
/* FsrRcasF without the noise detection: sharpen as far as the 5 taps allow without clipping */
"float4 rcas(float4 pos : SV_Position) : SV_Target {\n"
"    int2 p = int2(pos.xy);\n"
"    float3 b = tap(p + int2(0, -1)), d = tap(p + int2(-1, 0)), e = tap(p), f = tap(p + int2(1, 0)), h = tap(p + int2(0, 1));\n"
"    float3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
"    float3 hitMin = min(mn4, e) / (4.0 * mx4 + 1e-5);\n"
"    float3 hitMax = (1.0 - max(mx4, e)) / (4.0 * mn4 - 4.0 - 1e-5);\n"
"    float3 lobeC = max(-hitMin, hitMax);\n"
"    float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeC.r, max(lobeC.g, lobeC.b)), 0.0)) * c1.z;\n"
"    return float4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 1);\n"
"}\n";

static D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu(ID3D12DescriptorHeap *h, UINT index, UINT inc)
{
    D3D12_CPU_DESCRIPTOR_HANDLE c;
    h->lpVtbl->GetCPUDescriptorHandleForHeapStart(h, &c);
    c.ptr += (SIZE_T)index * inc;
    return c;
}

static D3D12_GPU_DESCRIPTOR_HANDLE heap_gpu(ID3D12DescriptorHeap *h, UINT index, UINT inc)
{
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    h->lpVtbl->GetGPUDescriptorHandleForHeapStart(h, &g);
    g.ptr += (UINT64)index * inc;
    return g;
}

static void up_execute(ID3D12CommandList *l)
{
    if (queue_vt_orig) ((execute_fn)queue_vt_orig[10])(up.queue, 1, &l);
    else ID3D12CommandQueue_ExecuteCommandLists(up.queue, 1, &l);
}

static HRESULT up_signal(UINT64 v)
{
    if (queue_vt_orig) return ((signal_fn)queue_vt_orig[14])(up.queue, up.fence, v);
    return ID3D12CommandQueue_Signal(up.queue, up.fence, v);
}

static void up_wait(UINT64 v)
{
    if (!up.fence || ID3D12Fence_GetCompletedValue(up.fence) >= v) return;
    if (SUCCEEDED(ID3D12Fence_SetEventOnCompletion(up.fence, v, up.event))) WaitForSingleObject(up.event, 2000);
}

static void up_idle(void)
{
    if (!up.fence) return;
    if (SUCCEEDED(up_signal(++up.fence_val))) up_wait(up.fence_val);
}

#define UP_RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

/* drops everything sized by the swap chain; the real back buffers must go before ResizeBuffers */
static void up_release_buffers(void)
{
    up_idle();
    for (UINT i = 0; i < UP_MAX_BUFFERS; i++) { UP_RELEASE(up.proxy[i]); UP_RELEASE(up.real[i]); }
    UP_RELEASE(up.mid);
    UP_RELEASE(up.rtv);
    UP_RELEASE(up.srv);
    up.on = FALSE;
    for (UINT i = 0; i < UP_RTVS; i++) up_rtv[i] = 0;
    for (UINT i = 0; i < UP_SMALL; i++) up_small[i] = NULL;
    up_sx = up_sy = 1;
}

static void up_forget(void)
{
    up_release_buffers();
    for (UINT i = 0; i < UP_ALLOCS; i++) UP_RELEASE(up.alloc[i]);
    UP_RELEASE(up.list);
    UP_RELEASE(up.fence);
    UP_RELEASE(up.queue);
    UP_RELEASE(up.dev);
    if (up.event) CloseHandle(up.event);
    memset(&up, 0, sizeof(up));
}

static BOOL up_pipelines(DXGI_FORMAT fmt)
{
    HMODULE comp;
    d3dcompile_fn compile;
    ID3DBlob *vs = NULL, *ps1 = NULL, *ps2 = NULL, *err = NULL, *rsb = NULL;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd;
    HRESULT hr;
    if (up_rs && up_pso_fmt == fmt) return TRUE;
    UP_RELEASE(up_easu);
    UP_RELEASE(up_rcas);
    comp = LoadLibraryA("d3dcompiler_47.dll");
    compile = comp ? (d3dcompile_fn)GetProcAddress(comp, "D3DCompile") : NULL;
    if (!compile) { logmsg("upscale: no D3DCompile"); return FALSE; }
    hr = compile(up_hlsl, sizeof(up_hlsl) - 1, "upscale", NULL, NULL, "vs", "vs_5_0", 0, 0, &vs, &err);
    if (SUCCEEDED(hr)) hr = compile(up_hlsl, sizeof(up_hlsl) - 1, "upscale", NULL, NULL, "easu", "ps_5_0", 0, 0, &ps1, &err);
    if (SUCCEEDED(hr)) hr = compile(up_hlsl, sizeof(up_hlsl) - 1, "upscale", NULL, NULL, "rcas", "ps_5_0", 0, 0, &ps2, &err);
    if (FAILED(hr)) {
        logmsg("upscale: compile failed %#lx %s", hr, err ? (char *)ID3D10Blob_GetBufferPointer(err) : "");
        goto done;
    }
    if (!up_rs) {
        D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER params[2];
        D3D12_ROOT_SIGNATURE_DESC rd = {2, params, 0, NULL, 0};
        memset(params, 0, sizeof(params));
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 8;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        hr = real_serialize_rs(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err);
        if (SUCCEEDED(hr)) hr = ID3D12Device_CreateRootSignature(up.dev, 0, ID3D10Blob_GetBufferPointer(rsb),
                                                                 ID3D10Blob_GetBufferSize(rsb), &IID_ID3D12RootSignature, (void **)&up_rs);
        if (FAILED(hr)) { logmsg("upscale: root signature failed %#lx", hr); goto done; }
    }
    memset(&gd, 0, sizeof(gd));
    gd.pRootSignature = up_rs;
    gd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
    gd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gd.SampleMask = 0xffffffff;
    gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gd.RasterizerState.DepthClipEnable = TRUE;
    gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gd.NumRenderTargets = 1;
    gd.RTVFormats[0] = fmt;
    gd.SampleDesc.Count = 1;
    gd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps1);
    gd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps1);
    hr = ID3D12Device_CreateGraphicsPipelineState(up.dev, &gd, &IID_ID3D12PipelineState, (void **)&up_easu);
    gd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps2);
    gd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps2);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateGraphicsPipelineState(up.dev, &gd, &IID_ID3D12PipelineState, (void **)&up_rcas);
    if (FAILED(hr)) { logmsg("upscale: pipeline failed %#lx", hr); UP_RELEASE(up_easu); UP_RELEASE(up_rcas); goto done; }
    up_pso_fmt = fmt;
done:
    UP_RELEASE(vs); UP_RELEASE(ps1); UP_RELEASE(ps2); UP_RELEASE(err); UP_RELEASE(rsb);
    return up_easu && up_rcas;
}

static ID3D12Resource *up_texture(UINT w, UINT h, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, up.fmt, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ID3D12Resource *r = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(up.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL,
                                                      &IID_ID3D12Resource, (void **)&r);
    if (FAILED(hr)) logmsg("upscale: texture %ux%u failed %#lx", w, h, hr);
    return r;
}

/* sizes the proxies after a create or resize; leaves the upscaler off when there is nothing to enlarge */
static void up_setup(void)
{
    DXGI_SWAP_CHAIN_DESC1 d;
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    HRESULT hr;
    if (!up_enabled || !up.sc || !up.rw || !up.rh || FAILED(real_sc_get_desc1(up.sc, &d))) return;
    up.n = d.BufferCount;
    up.dw = d.Width;
    up.dh = d.Height;
    up.fmt = d.Format;
    /* the render size fits the proxy 1:1 and the proxy has the window's shape */
    if ((UINT64)up.dw * up.rh > (UINT64)up.dh * up.rw) { up.ph = up.rh; up.pw = (UINT)(((UINT64)up.rh * up.dw + up.dh / 2) / up.dh); }
    else { up.pw = up.rw; up.ph = (UINT)(((UINT64)up.rw * up.dh + up.dw / 2) / up.dw); }
    if (up.pw >= up.dw || up.n > UP_MAX_BUFFERS) {
        logmsg("upscale: off for %ux%u (render %ux%u, nothing to enlarge)", up.dw, up.dh, up.rw, up.rh);
        return;
    }
    if (!up_pipelines(up.fmt)) return;
    if (!up.fence) {
        hr = ID3D12Device_CreateFence(up.dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&up.fence);
        for (UINT i = 0; SUCCEEDED(hr) && i < UP_ALLOCS; i++)
            hr = ID3D12Device_CreateCommandAllocator(up.dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&up.alloc[i]);
        if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandList(up.dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, up.alloc[0], NULL,
                                                               &IID_ID3D12GraphicsCommandList, (void **)&up.list);
        if (SUCCEEDED(hr)) hr = ID3D12GraphicsCommandList_Close(up.list);
        up.event = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (FAILED(hr) || !up.event) { logmsg("upscale: fence/list failed %#lx", hr); return; }
    }
    memset(&hd, 0, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = up.n + 1;
    if (FAILED(ID3D12Device_CreateDescriptorHeap(up.dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&up.rtv))) goto fail;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(ID3D12Device_CreateDescriptorHeap(up.dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&up.srv))) goto fail;
    up.rtv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(up.dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    up.srv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(up.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!(up.mid = up_texture(up.dw, up.dh, D3D12_RESOURCE_STATE_RENDER_TARGET))) goto fail;
    ID3D12Device_CreateRenderTargetView(up.dev, up.mid, NULL, heap_cpu(up.rtv, up.n, up.rtv_inc));
    ID3D12Device_CreateShaderResourceView(up.dev, up.mid, NULL, heap_cpu(up.srv, up.n, up.srv_inc));
    for (UINT i = 0; i < up.n; i++) {
        if (FAILED(real_sc_get_buffer(up.sc, i, &IID_ID3D12Resource, (void **)&up.real[i]))) goto fail;
        if (!(up.proxy[i] = up_texture(up.pw, up.ph, D3D12_RESOURCE_STATE_PRESENT))) goto fail;
        ID3D12Device_CreateRenderTargetView(up.dev, up.real[i], NULL, heap_cpu(up.rtv, i, up.rtv_inc));
        ID3D12Device_CreateShaderResourceView(up.dev, up.proxy[i], NULL, heap_cpu(up.srv, i, up.srv_inc));
    }
    up_sx = (float)up.pw / up.dw;
    up_sy = (float)up.ph / up.dh;
    InterlockedExchange(&up_draw_logs, 0);
    InterlockedExchange(&up_copy_logs, 0);
    up.on = TRUE;
    logmsg("upscale: on, render %ux%u -> proxy %ux%u -> window %ux%u (%.2fx), %u buffers fmt %u, sharpening %.2f stops",
           up.rw, up.rh, up.pw, up.ph, up.dw, up.dh, (double)up.dw / up.pw, up.n, up.fmt, up_sharp_stops);
    return;
fail:
    logmsg("upscale: setup failed, presenting Azimuth's frame unchanged");
    up_release_buffers();
}

static void barrier(D3D12_RESOURCE_BARRIER *b, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    memset(b, 0, sizeof(*b));
    b->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b->Transition.pResource = r;
    b->Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b->Transition.StateBefore = from;
    b->Transition.StateAfter = to;
}

static void up_pass(ID3D12GraphicsCommandList *l, ID3D12PipelineState *pso, UINT rtv, UINT srv, const float *consts)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rt = heap_cpu(up.rtv, rtv, up.rtv_inc);
    ID3D12GraphicsCommandList_SetPipelineState(l, pso);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, 0, 8, consts, 0);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(l, 1, heap_gpu(up.srv, srv, up.srv_inc));
    ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rt, FALSE, NULL);
    ID3D12GraphicsCommandList_DrawInstanced(l, 3, 1, 0, 0);
}

/* proxy -> EASU -> mid -> RCAS -> real back buffer, on the swap chain's queue right before Present */
static void up_scale(void)
{
    UINT idx = IDXGISwapChain3_GetCurrentBackBufferIndex(up.sc), a = up.frame++ % UP_ALLOCS;
    ID3D12GraphicsCommandList *l = up.list;
    D3D12_RESOURCE_BARRIER b[3];
    D3D12_VIEWPORT vp = {0, 0, (float)up.dw, (float)up.dh, 0, 1};
    D3D12_RECT sr = {0, 0, (LONG)up.dw, (LONG)up.dh};
    float easu_c[8] = {(float)up.pw / up.dw, (float)up.ph / up.dh, 0.5f * up.pw / up.dw - 0.5f, 0.5f * up.ph / up.dh - 0.5f,
                       (float)up.pw, (float)up.ph, 0, 0};
    float rcas_c[8] = {0, 0, 0, 0, (float)up.dw, (float)up.dh, (float)pow(2.0, -up_sharp_stops), 0};
    if (idx >= up.n) return;
    up_wait(up.alloc_val[a]);
    if (FAILED(ID3D12CommandAllocator_Reset(up.alloc[a])) || FAILED(ID3D12GraphicsCommandList_Reset(l, up.alloc[a], NULL))) return;
    ID3D12GraphicsCommandList_SetDescriptorHeaps(l, 1, &up.srv);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(l, up_rs);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(l, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(l, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sr);
    barrier(&b[0], up.proxy[idx], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12GraphicsCommandList_ResourceBarrier(l, 1, b);
    up_pass(l, up_easu, up.n, idx, easu_c);
    barrier(&b[0], up.mid, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barrier(&b[1], up.real[idx], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_ResourceBarrier(l, 2, b);
    up_pass(l, up_rcas, idx, up.n, rcas_c);
    barrier(&b[0], up.real[idx], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    barrier(&b[1], up.mid, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(&b[2], up.proxy[idx], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
    ID3D12GraphicsCommandList_ResourceBarrier(l, 3, b);
    if (FAILED(ID3D12GraphicsCommandList_Close(l))) return;
    up_execute((ID3D12CommandList *)l);
    up.alloc_val[a] = ++up.fence_val;
    up_signal(up.alloc_val[a]);
}

static void log_window(const char *what, HWND w)
{
    RECT cr = {0};
    MONITORINFO mi = {sizeof(mi)};
    if (!w) return;
    GetClientRect(w, &cr);
    GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
    logmsg("present: %s: window client %ldx%ld, monitor %ldx%ld", what, cr.right - cr.left, cr.bottom - cr.top,
           mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
}

static HRESULT STDMETHODCALLTYPE hk_sc_present(IDXGISwapChain3 *sc, UINT interval, UINT flags)
{
    LONG n = InterlockedIncrement(&present_count);
    if (n <= 2) logmsg("present: Present #%ld interval %u flags %#x", n, interval, flags);
    EnterCriticalSection(&up_cs);
    if (up.on && sc == up.sc && !(flags & DXGI_PRESENT_TEST)) up_scale();
    LeaveCriticalSection(&up_cs);
    return real_sc_present(sc, interval, flags);
}

static HRESULT STDMETHODCALLTYPE hk_sc_present1(IDXGISwapChain3 *sc, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS *p)
{
    LONG n = InterlockedIncrement(&present_count);
    if (n <= 2) logmsg("present: Present1 #%ld interval %u flags %#x", n, interval, flags);
    EnterCriticalSection(&up_cs);
    if (up.on && sc == up.sc && !(flags & DXGI_PRESENT_TEST)) up_scale();
    LeaveCriticalSection(&up_cs);
    return real_sc_present1(sc, interval, flags, p);
}

static HRESULT STDMETHODCALLTYPE hk_sc_get_buffer(IDXGISwapChain3 *sc, UINT index, REFIID riid, void **out)
{
    HRESULT hr;
    EnterCriticalSection(&up_cs);
    if (up.on && sc == up.sc && index < up.n) {
        hr = ID3D12Resource_QueryInterface(up.proxy[index], riid, out);
        LeaveCriticalSection(&up_cs);
        return hr;
    }
    LeaveCriticalSection(&up_cs);
    hr = real_sc_get_buffer(sc, index, riid, out);
    if (InterlockedIncrement(&get_buffer_logs) <= 8) {
        D3D12_RESOURCE_DESC d = {0};
        if (SUCCEEDED(hr) && IsEqualGUID(riid, &IID_ID3D12Resource)) ((ID3D12Resource *)*out)->lpVtbl->GetDesc((ID3D12Resource *)*out, &d);
        logmsg("present: GetBuffer %u -> %#lx, %llux%u fmt %u", index, hr, (unsigned long long)d.Width, d.Height, d.Format);
    }
    return hr;
}

/* Azimuth sizes its last pass from the swap chain's description: it sees the proxy */
static HRESULT STDMETHODCALLTYPE hk_sc_get_desc(IDXGISwapChain3 *sc, DXGI_SWAP_CHAIN_DESC *d)
{
    HRESULT hr = real_sc_get_desc(sc, d);
    EnterCriticalSection(&up_cs);
    if (SUCCEEDED(hr) && up.on && sc == up.sc) { d->BufferDesc.Width = up.pw; d->BufferDesc.Height = up.ph; }
    LeaveCriticalSection(&up_cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_sc_get_desc1(IDXGISwapChain3 *sc, DXGI_SWAP_CHAIN_DESC1 *d)
{
    HRESULT hr = real_sc_get_desc1(sc, d);
    EnterCriticalSection(&up_cs);
    if (SUCCEEDED(hr) && up.on && sc == up.sc) { d->Width = up.pw; d->Height = up.ph; }
    LeaveCriticalSection(&up_cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_sc_resize(IDXGISwapChain3 *sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags)
{
    HRESULT hr;
    HWND win = NULL;
    EnterCriticalSection(&up_cs);
    if (sc == up.sc) up_release_buffers();
    hr = real_sc_resize(sc, n, w, h, f, flags);
    logmsg("present: ResizeBuffers %u, %ux%u fmt %u flags %#x -> %#lx", n, w, h, f, flags, hr);
    if (SUCCEEDED(IDXGISwapChain3_GetHwnd(sc, &win))) log_window("after ResizeBuffers", win);
    if (sc == up.sc) up_setup();
    LeaveCriticalSection(&up_cs);
    InterlockedExchange(&get_buffer_logs, 0);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_sc_resize1(IDXGISwapChain3 *sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags,
                                               const UINT *masks, IUnknown *const *queues)
{
    HRESULT hr;
    EnterCriticalSection(&up_cs);
    if (sc == up.sc) up_release_buffers();
    hr = real_sc_resize1(sc, n, w, h, f, flags, masks, queues);
    logmsg("present: ResizeBuffers1 %u, %ux%u fmt %u flags %#x -> %#lx", n, w, h, f, flags, hr);
    if (sc == up.sc) up_setup();
    LeaveCriticalSection(&up_cs);
    InterlockedExchange(&get_buffer_logs, 0);
    return hr;
}

/* point one vtable slot at a hook, keeping the original once */
static void patch_slot(void **slot, void *hook, void **orig)
{
    DWORD old;
    if (*slot == hook) return;
    if (!*orig) *orig = *slot;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) { logmsg("present: VirtualProtect failed"); return; }
    *slot = hook;
    VirtualProtect(slot, sizeof(*slot), old, &old);
}

/* hooks the swap chain; one asked for at a size other than its window's is the game's */
static void hook_swapchain(IUnknown *obj, IUnknown *queue, HWND w, UINT width, UINT height)
{
    IDXGISwapChain3 *sc = NULL;
    IDXGISwapChain3Vtbl *vt;
    RECT cr = {0};
    if (FAILED(IUnknown_QueryInterface(obj, &IID_IDXGISwapChain3, (void **)&sc))) {
        logmsg("present: swap chain has no IDXGISwapChain3, not hooked");
        return;
    }
    vt = sc->lpVtbl;
    patch_slot((void **)&vt->Present, (void *)hk_sc_present, (void **)&real_sc_present);
    patch_slot((void **)&vt->GetBuffer, (void *)hk_sc_get_buffer, (void **)&real_sc_get_buffer);
    patch_slot((void **)&vt->GetDesc, (void *)hk_sc_get_desc, (void **)&real_sc_get_desc);
    patch_slot((void **)&vt->ResizeBuffers, (void *)hk_sc_resize, (void **)&real_sc_resize);
    patch_slot((void **)&vt->GetDesc1, (void *)hk_sc_get_desc1, (void **)&real_sc_get_desc1);
    patch_slot((void **)&vt->Present1, (void *)hk_sc_present1, (void **)&real_sc_present1);
    patch_slot((void **)&vt->ResizeBuffers1, (void *)hk_sc_resize1, (void **)&real_sc_resize1);
    if (w) GetClientRect(w, &cr);
    if (width && height && (width != (UINT)cr.right || height != (UINT)cr.bottom)) {
        ID3D12CommandQueue *q = NULL;
        ID3D12Device *dev = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface(queue, &IID_ID3D12CommandQueue, (void **)&q)) &&
            SUCCEEDED(ID3D12CommandQueue_GetDevice(q, &IID_ID3D12Device, (void **)&dev))) {
            EnterCriticalSection(&up_cs);
            up_forget();
            up.sc = sc;
            up.queue = q;
            up.dev = dev;
            up.rw = width;
            up.rh = height;
            logmsg("present: game swap chain %p, render size %ux%u, window %ldx%ld", (void *)sc, width, height, cr.right, cr.bottom);
            up_setup();
            LeaveCriticalSection(&up_cs);
        } else {
            UP_RELEASE(q);
            logmsg("present: swap chain device is not a D3D12 queue, upscaler off");
        }
    }
    IDXGISwapChain3_Release(sc);
}

static HRESULT STDMETHODCALLTYPE hk_fac_create_sc(IDXGIFactory2 *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d, IDXGISwapChain **out)
{
    HRESULT hr = real_fac_create_sc(f, dev, d, out);
    if (d) logmsg("present: CreateSwapChain %ux%u fmt %u buffers %u -> %#lx", d->BufferDesc.Width, d->BufferDesc.Height,
                  d->BufferDesc.Format, d->BufferCount, hr);
    if (SUCCEEDED(hr) && *out && d) hook_swapchain((IUnknown *)*out, dev, d->OutputWindow, d->BufferDesc.Width, d->BufferDesc.Height);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_fac_create_hwnd(IDXGIFactory2 *f, IUnknown *dev, HWND w, const DXGI_SWAP_CHAIN_DESC1 *d,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs, IDXGIOutput *o, IDXGISwapChain1 **out)
{
    HRESULT hr = real_fac_create_hwnd(f, dev, w, d, fs, o, out);
    if (d) logmsg("present: CreateSwapChainForHwnd %ux%u fmt %u buffers %u swap %u flags %#x -> %#lx",
                  d->Width, d->Height, d->Format, d->BufferCount, d->SwapEffect, d->Flags, hr);
    log_window("CreateSwapChainForHwnd", w);
    if (SUCCEEDED(hr) && *out && d) hook_swapchain((IUnknown *)*out, dev, w, d->Width, d->Height);
    return hr;
}

static void hook_factory(void *obj)
{
    IDXGIFactory2 *f = NULL;
    IDXGIFactory2Vtbl *vt;
    if (FAILED(IUnknown_QueryInterface((IUnknown *)obj, &IID_IDXGIFactory2, (void **)&f))) {
        logmsg("present: factory has no IDXGIFactory2, not hooked");
        return;
    }
    vt = f->lpVtbl;
    patch_slot((void **)&vt->CreateSwapChain, (void *)hk_fac_create_sc, (void **)&real_fac_create_sc);
    patch_slot((void **)&vt->CreateSwapChainForHwnd, (void *)hk_fac_create_hwnd, (void **)&real_fac_create_hwnd);
    IDXGIFactory2_Release(f);
}

static HRESULT WINAPI hk_create_factory1(REFIID riid, void **out)
{
    HRESULT hr = real_create_factory1(riid, out);
    if (SUCCEEDED(hr) && *out) hook_factory(*out);
    return hr;
}

static HRESULT WINAPI hk_create_factory2(UINT flags, REFIID riid, void **out)
{
    HRESULT hr = real_create_factory2(flags, riid, out);
    if (SUCCEEDED(hr) && *out) hook_factory(*out);
    return hr;
}

/* point azimuth.exe's dxgi.dll imports at the wrappers above */
static void hook_dxgi(HMODULE exe)
{
    uint8_t *base = (uint8_t *)exe;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    char v[16] = "";
    int hooked = 0;
    InitializeCriticalSection(&up_cs);
    up_enabled = !(GetEnvironmentVariableA("FH2_UPSCALE", v, sizeof(v)) && v[0] == '0');
    if (GetEnvironmentVariableA("FH2_SHARP", v, sizeof(v))) up_sharp_stops = (float)atof(v);
    if (up_sharp_stops < 0) up_sharp_stops = 0;
    if (!dir->VirtualAddress) return;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); imp->Name; imp++) {
        IMAGE_THUNK_DATA *names, *slots;
        if (_stricmp((const char *)(base + imp->Name), "dxgi.dll")) continue;
        names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        slots = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            const char *name;
            void *hook;
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            name = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
            if (!strcmp(name, "CreateDXGIFactory1")) {
                real_create_factory1 = (create_factory1_fn)slots->u1.Function;
                hook = (void *)hk_create_factory1;
            } else if (!strcmp(name, "CreateDXGIFactory2")) {
                real_create_factory2 = (create_factory2_fn)slots->u1.Function;
                hook = (void *)hk_create_factory2;
            } else continue;
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old)) continue;
            slots->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
            hooked++;
        }
    }
    logmsg("present: %d dxgi imports of azimuth.exe hooked, upscaler %s", hooked, up_enabled ? "on when the window is bigger than the render size" : "off (FH2_UPSCALE=0)");
}

/* ---------------- Azimuth's last pass into a proxy ---------------- */

/* Azimuth sizes the viewport of its last pass from the window, not from the
 * back buffer, so on a proxy it would draw a window-sized picture and lose the
 * part past the proxy's edge. Command lists that render into a proxy get their
 * viewports and scissors scaled from window to proxy pixels; one that already
 * fits the proxy is left alone. */

typedef void (STDMETHODCALLTYPE *viewports_fn)(ID3D12GraphicsCommandList *, UINT, const D3D12_VIEWPORT *);
typedef void (STDMETHODCALLTYPE *scissors_fn)(ID3D12GraphicsCommandList *, UINT, const D3D12_RECT *);
typedef void (STDMETHODCALLTYPE *om_fn)(ID3D12GraphicsCommandList *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, BOOL,
                                        const D3D12_CPU_DESCRIPTOR_HANDLE *);
typedef void (STDMETHODCALLTYPE *copy_region_fn)(ID3D12GraphicsCommandList *, const D3D12_TEXTURE_COPY_LOCATION *, UINT, UINT, UINT,
                                                 const D3D12_TEXTURE_COPY_LOCATION *, const D3D12_BOX *);
typedef void (STDMETHODCALLTYPE *copy_resource_fn)(ID3D12GraphicsCommandList *, ID3D12Resource *, ID3D12Resource *);
typedef void (STDMETHODCALLTYPE *create_rtv_fn)(ID3D12Device *, ID3D12Resource *, const D3D12_RENDER_TARGET_VIEW_DESC *,
                                                D3D12_CPU_DESCRIPTOR_HANDLE);

#define UP_LISTS 2048
#define UP_VIEWS 4
typedef struct {
    void *list;
    BOOL on;
    UINT nvp, nsr;
    D3D12_VIEWPORT vp[UP_VIEWS];
    D3D12_RECT sr[UP_VIEWS];
} list_view;
static list_view up_lists[UP_LISTS];
static SRWLOCK up_lists_lock = SRWLOCK_INIT;
static volatile LONG up_lists_full;

/* call with up_lists_lock held */
static list_view *lv_find(void *l, BOOL create)
{
    UINT h = (UINT)(((uintptr_t)l >> 4) * 2654435761u) % UP_LISTS;
    for (UINT i = 0; i < UP_LISTS; i++, h = (h + 1) % UP_LISTS) {
        if (up_lists[h].list == l) return &up_lists[h];
        if (!up_lists[h].list) {
            if (!create) return NULL;
            memset(&up_lists[h], 0, sizeof(up_lists[h]));
            up_lists[h].list = l;
            return &up_lists[h];
        }
    }
    if (!InterlockedExchange(&up_lists_full, 1)) logmsg("upscale: command list table full, some lists not tracked");
    return NULL;
}

static void up_list_reset(void *l)
{
    list_view *e;
    AcquireSRWLockExclusive(&up_lists_lock);
    if ((e = lv_find(l, FALSE))) { e->on = FALSE; e->nvp = e->nsr = 0; }
    ReleaseSRWLockExclusive(&up_lists_lock);
}

static BOOL up_is_proxy(ID3D12Resource *r)
{
    if (!r || !up.on) return FALSE;
    for (UINT i = 0; i < up.n; i++) if (up.proxy[i] == r) return TRUE;
    return FALSE;
}

/* proxies, and Azimuth's render targets sized like them: its last pass draws into those */
static BOOL up_is_target(ID3D12Resource *r)
{
    if (up_is_proxy(r)) return TRUE;
    if (!r || !up.on) return FALSE;
    for (UINT i = 0; i < UP_SMALL; i++) if (up_small[i] == r) return TRUE;
    return FALSE;
}

static BOOL up_is_proxy_rtv(SIZE_T p)
{
    if (!p) return FALSE;
    for (UINT i = 0; i < UP_RTVS; i++) if (up_rtv[i] == p) return TRUE;
    return FALSE;
}

static void STDMETHODCALLTYPE hk_create_rtv(ID3D12Device *dev, ID3D12Resource *res, const D3D12_RENDER_TARGET_VIEW_DESC *d,
                                            D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    BOOL proxy = up_is_target(res);
    for (UINT i = 0; i < UP_RTVS; i++) {
        if (up_rtv[i] == h.ptr && !proxy) up_rtv[i] = 0;          /* descriptor reused for something else */
        else if (up_rtv[i] == h.ptr) proxy = FALSE;                /* already known */
    }
    if (proxy) {
        UINT i;
        for (i = 0; i < UP_RTVS && up_rtv[i]; i++);
        if (i < UP_RTVS) up_rtv[i] = h.ptr;
        logmsg("upscale: Azimuth made a render target view of a proxy-sized target %p (%zx)", (void *)res, (size_t)h.ptr);
    }
    ((create_rtv_fn)dev_vt_orig[20])(dev, res, d, h);
}

/* scales viewports that reach past the proxy; FALSE when they already fit */
static BOOL scale_viewports(UINT n, const D3D12_VIEWPORT *in, D3D12_VIEWPORT *out)
{
    BOOL past = FALSE;
    for (UINT i = 0; i < n; i++)
        if (in[i].TopLeftX + in[i].Width > up.pw + 0.5f || in[i].TopLeftY + in[i].Height > up.ph + 0.5f) past = TRUE;
    if (!past) return FALSE;
    for (UINT i = 0; i < n; i++) {
        out[i] = in[i];
        out[i].TopLeftX *= up_sx; out[i].Width *= up_sx;
        out[i].TopLeftY *= up_sy; out[i].Height *= up_sy;
    }
    return TRUE;
}

static BOOL scale_scissors(UINT n, const D3D12_RECT *in, D3D12_RECT *out)
{
    BOOL past = FALSE;
    for (UINT i = 0; i < n; i++) if (in[i].right > (LONG)up.pw || in[i].bottom > (LONG)up.ph) past = TRUE;
    if (!past) return FALSE;
    for (UINT i = 0; i < n; i++) {
        out[i].left = (LONG)(in[i].left * up_sx);
        out[i].top = (LONG)(in[i].top * up_sy);
        out[i].right = (LONG)(in[i].right * up_sx + 0.999f);
        out[i].bottom = (LONG)(in[i].bottom * up_sy + 0.999f);
    }
    return TRUE;
}

static void set_viewports(ID3D12GraphicsCommandList *l, list_view *e, UINT n, const D3D12_VIEWPORT *v)
{
    D3D12_VIEWPORT s[UP_VIEWS];
    if (e && e->on && n <= UP_VIEWS && scale_viewports(n, v, s)) {
        if (InterlockedIncrement(&up_draw_logs) <= 4)
            logmsg("upscale: viewport %.0f,%.0f %.0fx%.0f on a proxy -> %.0f,%.0f %.0fx%.0f", v[0].TopLeftX, v[0].TopLeftY,
                   v[0].Width, v[0].Height, s[0].TopLeftX, s[0].TopLeftY, s[0].Width, s[0].Height);
        ((viewports_fn)list_vt_orig[21])(l, n, s);
    } else {
        if (e && e->on && InterlockedIncrement(&up_draw_logs) <= 4)
            logmsg("upscale: viewport %.0f,%.0f %.0fx%.0f on a proxy fits, unchanged", v[0].TopLeftX, v[0].TopLeftY, v[0].Width, v[0].Height);
        ((viewports_fn)list_vt_orig[21])(l, n, v);
    }
}

static void set_scissors(ID3D12GraphicsCommandList *l, list_view *e, UINT n, const D3D12_RECT *r)
{
    D3D12_RECT s[UP_VIEWS];
    if (e && e->on && n <= UP_VIEWS && scale_scissors(n, r, s)) ((scissors_fn)list_vt_orig[22])(l, n, s);
    else ((scissors_fn)list_vt_orig[22])(l, n, r);
}

static void STDMETHODCALLTYPE hk_rs_viewports(ID3D12GraphicsCommandList *l, UINT n, const D3D12_VIEWPORT *v)
{
    list_view *e = NULL, copy;
    if (up.on) {
        AcquireSRWLockExclusive(&up_lists_lock);
        if ((e = lv_find(l, TRUE))) {
            e->nvp = n <= UP_VIEWS ? n : 0;
            if (e->nvp) memcpy(e->vp, v, n * sizeof(*v));
            copy = *e;
            e = &copy;
        }
        ReleaseSRWLockExclusive(&up_lists_lock);
    }
    set_viewports(l, e, n, v);
}

static void STDMETHODCALLTYPE hk_rs_scissors(ID3D12GraphicsCommandList *l, UINT n, const D3D12_RECT *r)
{
    list_view *e = NULL, copy;
    if (up.on) {
        AcquireSRWLockExclusive(&up_lists_lock);
        if ((e = lv_find(l, TRUE))) {
            e->nsr = n <= UP_VIEWS ? n : 0;
            if (e->nsr) memcpy(e->sr, r, n * sizeof(*r));
            copy = *e;
            e = &copy;
        }
        ReleaseSRWLockExclusive(&up_lists_lock);
    }
    set_scissors(l, e, n, r);
}

/* binding a proxy turns scaling on for the list, and re-applies the viewports set before */
static void STDMETHODCALLTYPE hk_om_targets(ID3D12GraphicsCommandList *l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE *rt,
                                            BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE *ds)
{
    ((om_fn)list_vt_orig[46])(l, n, rt, single, ds);
    if (up.on || up_rtv[0]) {
        BOOL on = up.on && n && rt && up_is_proxy_rtv(rt[0].ptr);
        list_view *e, copy;
        BOOL changed = FALSE;
        AcquireSRWLockExclusive(&up_lists_lock);
        if ((e = lv_find(l, on))) {
            changed = e->on != on;
            e->on = on;
            copy = *e;
        }
        ReleaseSRWLockExclusive(&up_lists_lock);
        if (e && changed) {
            if (copy.nvp) set_viewports(l, &copy, copy.nvp, copy.vp);
            if (copy.nsr) set_scissors(l, &copy, copy.nsr, copy.sr);
        }
    }
}

static void STDMETHODCALLTYPE hk_copy_region(ID3D12GraphicsCommandList *l, const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y,
                                             UINT z, const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box)
{
    if (up.on && dst && up_is_proxy(dst->pResource) && InterlockedIncrement(&up_copy_logs) <= 4)
        logmsg("upscale: CopyTextureRegion into a proxy at %u,%u box %ld,%ld-%ld,%ld", x, y,
               box ? (long)box->left : -1L, box ? (long)box->top : -1L, box ? (long)box->right : -1L, box ? (long)box->bottom : -1L);
    ((copy_region_fn)list_vt_orig[16])(l, dst, x, y, z, src, box);
}

static void STDMETHODCALLTYPE hk_copy_resource(ID3D12GraphicsCommandList *l, ID3D12Resource *dst, ID3D12Resource *src)
{
    if (up.on && up_is_proxy(dst) && InterlockedIncrement(&up_copy_logs) <= 4) {
        D3D12_RESOURCE_DESC d = {0};
        if (src) src->lpVtbl->GetDesc(src, &d);
        logmsg("upscale: CopyResource into a proxy from %p: %llux%u fmt %u flags %#x mips %u samples %u", (void *)src,
               (unsigned long long)d.Width, d.Height, d.Format, d.Flags, d.MipLevels, d.SampleDesc.Count);
    }
    ((copy_resource_fn)list_vt_orig[17])(l, dst, src);
}

/* diagnostic: textures as big as the window made while the upscaler is on */
static volatile LONG up_tex_logs;
static void up_note_texture(const char *what, const D3D12_RESOURCE_DESC *d, void *res)
{
    if (!up.on || !d || d->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;
    if (d->Width < up.pw || d->Height < up.ph) return;
    if (d->Width == up.pw && d->Height == up.ph && d->SampleDesc.Count == 1 && (d->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
        UINT i;
        for (i = 0; i < UP_SMALL && up_small[i]; i++);
        if (i < UP_SMALL) up_small[i] = res;
    }
    if (InterlockedIncrement(&up_tex_logs) > 12) return;
    logmsg("upscale: %s %p %llux%u fmt %u flags %#x mips %u samples %u", what, res, (unsigned long long)d->Width, d->Height,
           d->Format, d->Flags, d->MipLevels, d->SampleDesc.Count);
}
