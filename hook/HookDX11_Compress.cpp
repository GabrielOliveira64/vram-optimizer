/*
 * HookDX11_Compress.cpp — Parte 3: Compressão real dentro do hook
 * ─────────────────────────────────────────────────────────────────────────────
 * Este arquivo estende o HookDX11 com a lógica de compressão real de texturas.
 * Substitui o stub ApplyCompression de HookDX11_Textures.cpp com o fluxo:
 *
 *   1. Recebe ID da textura + nível de compressão (via comando IPC)
 *   2. Obtém o device context do jogo
 *   3. Mapeia a textura original (CPU read)
 *   4. Chama TextureCompressor para comprimir/downscalar
 *   5. Cria nova textura DX11 com os dados comprimidos
 *   6. Substitui internamente a referência (swap de pointer)
 *
 * NOTA IMPORTANTE SOBRE SWAP DE TEXTURA:
 *   DX11 não permite substituir um recurso "in-place". O hook precisa
 *   interceptar as chamadas de Draw que referenciam a textura original e
 *   redirecionar para a nova. Isso é feito via hook em:
 *     ID3D11DeviceContext::PSSetShaderResources (slot SRV)
 *
 *   O mapa m_srv_remap mantém: SRV_original → SRV_comprimida
 *   O hook de PSSetShaderResources substitui automaticamente na hora do draw.
 */

#define NOMINMAX
#include "HookDX11.h"
#include "../compressor/TextureCompressor.h"
#include "../ipc/IPCProtocol.h"
#include <MinHook.h>
#include <d3d11.h>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <mutex>

// ─────────────────────────────────────────────────────────────────────────────
// Estado adicional (estendemos o HookDX11 via variáveis de módulo)
// Mantemos separado para não poluir o header principal
// ─────────────────────────────────────────────────────────────────────────────

// Mapa SRV original → SRV comprimida (para substituição em PSSetShaderResources)
static std::unordered_map<ID3D11ShaderResourceView*, ID3D11ShaderResourceView*> g_srv_remap;
static std::mutex g_srv_mutex;

// Contexto do device capturado no primeiro frame
static ID3D11DeviceContext* g_context = nullptr;
static ID3D11Device*        g_device  = nullptr;
static std::mutex           g_ctx_mutex;

// Tipo do hook de PSSetShaderResources
using PFN_PSSetShaderResources = void (WINAPI*)(
    ID3D11DeviceContext*,
    UINT, UINT,
    ID3D11ShaderResourceView* const*);

static PFN_PSSetShaderResources g_orig_PSSetShaderResources = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Hook: PSSetShaderResources — substitui SRVs na hora do draw
// ─────────────────────────────────────────────────────────────────────────────

static void WINAPI Hooked_PSSetShaderResources(
    ID3D11DeviceContext*          pContext,
    UINT                          StartSlot,
    UINT                          NumViews,
    ID3D11ShaderResourceView* const* ppShaderResourceViews)
{
    if (!ppShaderResourceViews || NumViews == 0) {
        g_orig_PSSetShaderResources(pContext, StartSlot, NumViews, ppShaderResourceViews);
        return;
    }

    // Copia o array e substitui entradas mapeadas
    static thread_local ID3D11ShaderResourceView* patched[128];
    UINT count = std::min(NumViews, 128u);

    bool any_patched = false;
    {
        std::lock_guard<std::mutex> lock(g_srv_mutex);
        for (UINT i = 0; i < count; ++i) {
            auto it = g_srv_remap.find(ppShaderResourceViews[i]);
            if (it != g_srv_remap.end()) {
                patched[i]  = it->second;
                any_patched = true;
            } else {
                patched[i] = ppShaderResourceViews[i];
            }
        }
    }

    g_orig_PSSetShaderResources(
        pContext, StartSlot, count,
        any_patched ? patched : ppShaderResourceViews);
}

// ─────────────────────────────────────────────────────────────────────────────
// Captura do device context no primeiro Present (swap chain)
// ─────────────────────────────────────────────────────────────────────────────

using PFN_Present = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
static PFN_Present g_orig_Present = nullptr;

static HRESULT WINAPI Hooked_Present(IDXGISwapChain* pSwap, UINT sync, UINT flags) {
    if (!g_context) {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        if (!g_context) {
            // Obtém device + context a partir do swap chain
            ID3D11Device* dev = nullptr;
            if (SUCCEEDED(pSwap->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) {
                g_device = dev;
                dev->GetImmediateContext(&g_context);
                // dev->Release() — mantemos referência intencionalmente
            }
        }
    }
    return g_orig_Present(pSwap, sync, flags);
}

// ─────────────────────────────────────────────────────────────────────────────
// Instala hooks adicionais (PSSetShaderResources + Present)
// Chamado pelo HookDX11::Install() após HookVTable()
// ─────────────────────────────────────────────────────────────────────────────

bool InstallCompressHooks() {
    // ── Hook em IDXGISwapChain::Present (para capturar o context) ────────────
    IDXGIFactory* factory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)))
        return false;

    ID3D11Device*        tmp_dev = nullptr;
    ID3D11DeviceContext* tmp_ctx = nullptr;
    D3D_FEATURE_LEVEL    fl;
    IDXGIAdapter*        adapter = nullptr;
    factory->EnumAdapters(0, &adapter);

    D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                      0, nullptr, 0, D3D11_SDK_VERSION,
                      &tmp_dev, &fl, &tmp_ctx);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount       = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.Width  = 1;
    scd.BufferDesc.Height = 1;
    scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow      = GetDesktopWindow();
    scd.SampleDesc.Count  = 1;
    scd.Windowed          = TRUE;

    IDXGISwapChain* tmp_swap = nullptr;
    HRESULT hr = factory->CreateSwapChain(tmp_dev, &scd, &tmp_swap);

    if (SUCCEEDED(hr) && tmp_swap) {
        // Present é slot 8 no vtable de IDXGISwapChain
        void** vtable      = *reinterpret_cast<void***>(tmp_swap);
        void*  pfn_present = vtable[8];

        MH_CreateHook(pfn_present,
                      reinterpret_cast<void*>(Hooked_Present),
                      reinterpret_cast<void**>(&g_orig_Present));
        MH_EnableHook(pfn_present);
        tmp_swap->Release();
    }

    // ── Hook em ID3D11DeviceContext::PSSetShaderResources ────────────────────
    if (tmp_ctx) {
        // PSSetShaderResources é slot 9 no vtable de ID3D11DeviceContext
        void** ctx_vtable = *reinterpret_cast<void***>(tmp_ctx);
        void*  pfn_pssrv  = ctx_vtable[9];

        MH_CreateHook(pfn_pssrv,
                      reinterpret_cast<void*>(Hooked_PSSetShaderResources),
                      reinterpret_cast<void**>(&g_orig_PSSetShaderResources));
        MH_EnableHook(pfn_pssrv);
        tmp_ctx->Release();
    }

    if (tmp_dev) tmp_dev->Release();
    if (adapter) adapter->Release();
    factory->Release();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ApplyCompression — Implementação real (substitui o stub em HookDX11_Textures)
// ─────────────────────────────────────────────────────────────────────────────

void ApplyCompressionReal(
    ID3D11Device*        pDevice,
    ID3D11DeviceContext* pContext,
    ID3D11Texture2D*     pOrigTex,
    const D3D11_TEXTURE2D_DESC& orig_desc,
    uint32_t             level,
    uint32_t             target_w,
    uint32_t             target_h)
{
    if (!pDevice || !pContext || !pOrigTex) return;

    // ── Passo 1: Cria textura de staging para leitura na CPU ─────────────────
    D3D11_TEXTURE2D_DESC staging_desc = orig_desc;
    staging_desc.Usage          = D3D11_USAGE_STAGING;
    staging_desc.BindFlags      = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags      = 0;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(pDevice->CreateTexture2D(&staging_desc, nullptr, &staging))) {
        return; // GPU não suporta staging para este formato
    }

    pContext->CopyResource(staging, pOrigTex);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(pContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        staging->Release();
        return;
    }

    const uint8_t* pixels    = reinterpret_cast<const uint8_t*>(mapped.pData);
    uint32_t       src_pitch = mapped.RowPitch;

    // ── Passo 2: Comprime via TextureCompressor ───────────────────────────────
    TextureCompressor comp;
    CompressResult result;

    // Resolve resolução alvo
    uint32_t dw = (target_w > 0) ? target_w : orig_desc.Width  / 2;
    uint32_t dh = (target_h > 0) ? target_h : orig_desc.Height / 2;
    dw = std::max(4u, (dw + 3) & ~3u);
    dh = std::max(4u, (dh + 3) & ~3u);

    switch ((IPCCompressionLevel)level) {
        case IPCCompressionLevel::BC1:
            result = comp.CompressBC1(pixels, orig_desc.Width, orig_desc.Height,
                                      src_pitch, nullptr);
            break;
        case IPCCompressionLevel::BC3:
            result = comp.CompressBC3(pixels, orig_desc.Width, orig_desc.Height,
                                      src_pitch, nullptr);
            break;
        case IPCCompressionLevel::BC7:
            result = comp.CompressBC7(pixels, orig_desc.Width, orig_desc.Height,
                                      src_pitch, nullptr);
            break;
        case IPCCompressionLevel::HalfRes:
            result = comp.DownscaleAndCompress(pixels,
                         orig_desc.Width, orig_desc.Height, src_pitch,
                         dw, dh, false);
            break;
        case IPCCompressionLevel::QuarterRes:
            result = comp.DownscaleAndCompress(pixels,
                         orig_desc.Width, orig_desc.Height, src_pitch,
                         orig_desc.Width / 4, orig_desc.Height / 4, false);
            break;
        default:
            pContext->Unmap(staging, 0);
            staging->Release();
            return;
    }

    pContext->Unmap(staging, 0);
    staging->Release();

    if (!result.success) return;

    // ── Passo 3: Cria nova textura com dados comprimidos ──────────────────────
    D3D11_TEXTURE2D_DESC new_desc = orig_desc;
    new_desc.Width      = result.width;
    new_desc.Height     = result.height;
    new_desc.MipLevels  = 1;
    new_desc.Usage      = D3D11_USAGE_DEFAULT;
    new_desc.BindFlags  = D3D11_BIND_SHADER_RESOURCE;
    new_desc.CPUAccessFlags = 0;

    switch ((IPCCompressionLevel)level) {
        case IPCCompressionLevel::BC1:
            new_desc.Format = DXGI_FORMAT_BC1_UNORM; break;
        case IPCCompressionLevel::BC3:
            new_desc.Format = DXGI_FORMAT_BC3_UNORM; break;
        case IPCCompressionLevel::BC7:
            new_desc.Format = DXGI_FORMAT_BC7_UNORM; break;
        default:
            new_desc.Format = DXGI_FORMAT_BC1_UNORM; break;
    }

    D3D11_SUBRESOURCE_DATA init_data = {};
    init_data.pSysMem          = result.data.data();
    init_data.SysMemPitch      = result.row_pitch;
    init_data.SysMemSlicePitch = (UINT)result.size_bytes;

    ID3D11Texture2D* new_tex = nullptr;
    if (FAILED(pDevice->CreateTexture2D(&new_desc, &init_data, &new_tex)))
        return;

    // ── Passo 4: Cria SRV para a nova textura ────────────────────────────────
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format                    = new_desc.Format;
    srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels       = 1;
    srv_desc.Texture2D.MostDetailedMip = 0;

    ID3D11ShaderResourceView* new_srv  = nullptr;
    ID3D11ShaderResourceView* orig_srv = nullptr;

    pDevice->CreateShaderResourceView(new_tex,  &srv_desc, &new_srv);
    pDevice->CreateShaderResourceView(pOrigTex, nullptr,   &orig_srv);

    // ── Passo 5: Registra o remap SRV para substituição em PSSetShaderResources
    if (orig_srv && new_srv) {
        std::lock_guard<std::mutex> lock(g_srv_mutex);
        g_srv_remap[orig_srv] = new_srv;
    }

    // Libera referências temporárias (os SRVs ficam no mapa)
    new_tex->Release();
    // orig_srv e new_srv ficam no mapa; serão liberados quando a textura for destruída
}