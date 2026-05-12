/*
 * HookDX11_Textures.cpp — Parte 2: Interceptação, Compressão, DllMain
 * ─────────────────────────────────────────────────────────────────────────────
 * Separo da Parte 1 para manter cada arquivo com responsabilidade clara.
 * Ambos os .cpp são compilados juntos na mesma DLL.
 */

#define NOMINMAX
#include "HookDX11.h"
#include "../ipc/IPCProtocol.h"
#include <dxgi1_4.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Utilitários de textura
// ─────────────────────────────────────────────────────────────────────────────

// Calcula tamanho real de uma textura DX11 em bytes
static uint64_t CalcTextureSize(const D3D11_TEXTURE2D_DESC& desc) {
    uint64_t total = 0;
    uint32_t w = desc.Width;
    uint32_t h = desc.Height;

    for (uint32_t mip = 0; mip < desc.MipLevels; ++mip) {
        uint64_t mip_size = 0;
        switch (desc.Format) {
            // Formatos comprimidos em blocos 4×4
            case DXGI_FORMAT_BC1_UNORM:
            case DXGI_FORMAT_BC1_UNORM_SRGB:
                mip_size = ((w + 3) / 4) * ((h + 3) / 4) * 8ULL;
                break;
            case DXGI_FORMAT_BC2_UNORM:
            case DXGI_FORMAT_BC3_UNORM:
            case DXGI_FORMAT_BC3_UNORM_SRGB:
            case DXGI_FORMAT_BC5_UNORM:
            case DXGI_FORMAT_BC7_UNORM:
            case DXGI_FORMAT_BC7_UNORM_SRGB:
                mip_size = ((w + 3) / 4) * ((h + 3) / 4) * 16ULL;
                break;
            // Formatos não comprimidos comuns
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
                mip_size = (uint64_t)w * h * 4;
                break;
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                mip_size = (uint64_t)w * h * 8;
                break;
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
                mip_size = (uint64_t)w * h * 16;
                break;
            case DXGI_FORMAT_R32_FLOAT:
            case DXGI_FORMAT_D32_FLOAT:
                mip_size = (uint64_t)w * h * 4;
                break;
            default:
                mip_size = (uint64_t)w * h * 4; // estimativa conservadora
                break;
        }
        total += mip_size * desc.ArraySize;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    return total;
}

// Determina se é uma textura candidata à compressão
// (Filtra depth buffers, render targets, texturas minúsculas)
static bool IsCompressCandidate(const D3D11_TEXTURE2D_DESC& desc) {
    if (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL)  return false;
    if (desc.BindFlags & D3D11_BIND_RENDER_TARGET)  return false;
    if (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) return false;
    if (desc.Width  < 64 || desc.Height < 64)       return false;
    if (desc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) return false; // textura dinâmica
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Gerenciamento do mapa de texturas (array fixo, sem heap no caminho crítico)
// ─────────────────────────────────────────────────────────────────────────────

HookDX11::TrackedTexture* HookDX11::FindTexture(uint64_t id) {
    for (int i = 0; i < m_tex_count; ++i)
        if (m_tex_ids[i] == id) return &m_textures[i];
    return nullptr;
}

void HookDX11::AddTexture(uint64_t id, const TrackedTexture& t) {
    if (m_tex_count >= MAX_TEXTURES) {
        Log("[AVISO] Limite de texturas rastreadas atingido (%d).", MAX_TEXTURES);
        return;
    }
    m_tex_ids[m_tex_count]  = id;
    m_textures[m_tex_count] = t;
    ++m_tex_count;
}

void HookDX11::RemoveTexture(uint64_t id) {
    for (int i = 0; i < m_tex_count; ++i) {
        if (m_tex_ids[i] == id) {
            // Swap com último elemento
            m_tex_ids[i]  = m_tex_ids[m_tex_count - 1];
            m_textures[i] = m_textures[m_tex_count - 1];
            --m_tex_count;
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Hook: Hooked_CreateTexture2D (chamado no lugar do DX11 original)
// ─────────────────────────────────────────────────────────────────────────────

HRESULT WINAPI HookDX11::Hooked_CreateTexture2D(
    ID3D11Device*                 pDevice,
    const D3D11_TEXTURE2D_DESC*   pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData,
    ID3D11Texture2D**             ppTexture2D)
{
    HookDX11& hook = Instance();

    // Chama o original primeiro — deixa o jogo criar a textura normalmente
    HRESULT hr = hook.m_orig_CreateTexture2D(pDevice, pDesc, pInitialData, ppTexture2D);

    if (FAILED(hr) || !ppTexture2D || !*ppTexture2D) return hr;
    if (!pDesc) return hr;

    // Registra a textura criada (assíncrono — não bloqueia o jogo)
    hook.OnTextureCreated(*ppTexture2D, *pDesc);

    return hr;
}

// ─────────────────────────────────────────────────────────────────────────────
// OnTextureCreated: registra, filtra e notifica o orquestrador
// ─────────────────────────────────────────────────────────────────────────────

void HookDX11::OnTextureCreated(ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& desc) {
    uint64_t tex_id = reinterpret_cast<uint64_t>(tex);
    uint64_t size   = CalcTextureSize(desc);

    // Log resumido de cada textura (só candidatas para não poluir o log)
    if (IsCompressCandidate(desc)) {
        Log("[Tex+] %p | %4ux%-4u | mips=%-2u | fmt=%-2u | bind=0x%02X | %.1f MB",
            (void*)tex_id,
            desc.Width, desc.Height,
            desc.MipLevels,
            (uint32_t)desc.Format,
            desc.BindFlags,
            size / (1024.0 * 1024.0));
    }

    // Adiciona ao mapa local
    EnterCriticalSection(&m_tex_cs);
    TrackedTexture entry = {};
    entry.orig_desc         = desc;
    entry.size_bytes        = size;
    entry.is_compressed     = false;
    entry.compression_level = 0;
    AddTexture(tex_id, entry);
    LeaveCriticalSection(&m_tex_cs);

    // Notifica o orquestrador via pipe
    EventHeader ehdr = {};
    ehdr.type         = EventType::TextureCreated;
    ehdr.payload_size = sizeof(TextureCreatedEvent);

    TextureCreatedEvent evt = {};
    evt.texture_id = tex_id;
    evt.width      = desc.Width;
    evt.height     = desc.Height;
    evt.mip_levels = desc.MipLevels;
    evt.array_size = desc.ArraySize;
    evt.format     = DXGIToIPCFormat(desc.Format);
    evt.bind_flags = desc.BindFlags;
    evt.size_bytes = size;

    SendEvent(&ehdr, sizeof(ehdr), &evt, sizeof(evt));
}

// ─────────────────────────────────────────────────────────────────────────────
// OnTextureReleased: remove do mapa e notifica o orquestrador
// ─────────────────────────────────────────────────────────────────────────────

void HookDX11::OnTextureReleased(uint64_t texture_id) {
    EnterCriticalSection(&m_tex_cs);
    RemoveTexture(texture_id);
    LeaveCriticalSection(&m_tex_cs);

    EventHeader ehdr = {};
    ehdr.type         = EventType::TextureReleased;
    ehdr.payload_size = sizeof(TextureReleasedEvent);

    TextureReleasedEvent evt = {};
    evt.texture_id = texture_id;

    SendEvent(&ehdr, sizeof(ehdr), &evt, sizeof(evt));
}

// ─────────────────────────────────────────────────────────────────────────────
// ApplyCompression: recebe comando do orquestrador e substitui a textura
// ─────────────────────────────────────────────────────────────────────────────

void HookDX11::ApplyCompression(uint64_t texture_id, uint32_t level,
                                 uint32_t target_w, uint32_t target_h) {
    EnterCriticalSection(&m_tex_cs);
    TrackedTexture* entry = FindTexture(texture_id);
    if (!entry) {
        LeaveCriticalSection(&m_tex_cs);
        Log("[CMD] CompressTexture: textura %p nao encontrada.", (void*)texture_id);
        return;
    }

    // Copia a desc original para criar a versão comprimida
    D3D11_TEXTURE2D_DESC new_desc = entry->orig_desc;
    LeaveCriticalSection(&m_tex_cs);

    // Aplica downscale se solicitado
    if (target_w > 0 && target_h > 0) {
        new_desc.Width  = (target_w + 3) & ~3u; // arredonda para múltiplo de 4 (BC1/BC3)
        new_desc.Height = (target_h + 3) & ~3u;
    } else {
        // Reduz à metade (HalfRes)
        new_desc.Width  = std::max(4u, (new_desc.Width  / 2 + 3) & ~3u);
        new_desc.Height = std::max(4u, (new_desc.Height / 2 + 3) & ~3u);
    }

    // Converte para formato comprimido
    switch ((IPCCompressionLevel)level) {
        case IPCCompressionLevel::BC1:
            if (new_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                new_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM)
                new_desc.Format = DXGI_FORMAT_BC1_UNORM;
            break;
        case IPCCompressionLevel::BC3:
            if (new_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                new_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM)
                new_desc.Format = DXGI_FORMAT_BC3_UNORM;
            break;
        case IPCCompressionLevel::BC7:
            if (new_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                new_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM)
                new_desc.Format = DXGI_FORMAT_BC7_UNORM;
            break;
        case IPCCompressionLevel::QuarterRes:
            new_desc.Width  = std::max(4u, (entry->orig_desc.Width  / 4 + 3) & ~3u);
            new_desc.Height = std::max(4u, (entry->orig_desc.Height / 4 + 3) & ~3u);
            break;
        default:
            break;
    }

    uint64_t new_size = CalcTextureSize(new_desc);
    uint64_t saved    = entry->size_bytes > new_size
                        ? entry->size_bytes - new_size : 0;

    Log("[Compress] %p | %ux%u → %ux%u | fmt %u → %u | Economia: %.1f MB",
        (void*)texture_id,
        entry->orig_desc.Width, entry->orig_desc.Height,
        new_desc.Width, new_desc.Height,
        (uint32_t)entry->orig_desc.Format, (uint32_t)new_desc.Format,
        saved / (1024.0 * 1024.0));

    /*
     * ── PRÓXIMO PASSO: Compressão real dos pixels ────────────────────────────
     *
     * Para comprimir os dados reais da textura precisamos de ispc_texcomp:
     *   https://github.com/GameTechDev/ISPCTextureCompressor
     *
     * Fluxo:
     *   1. Map() a textura original para ler os pixels em CPU
     *   2. ispc_texcomp: CompressBlocksBC1 / CompressBlocksBC7
     *   3. CreateTexture2D com a nova desc + dados comprimidos
     *   4. CopySubresourceRegion ou substituição do ponteiro interno
     *
     * Exemplo (parcial):
     *
     *   D3D11_MAPPED_SUBRESOURCE mapped = {};
     *   pContext->Map(original_tex, 0, D3D11_MAP_READ, 0, &mapped);
     *
     *   rgba_surface input;
     *   input.ptr    = (uint8_t*)mapped.pData;
     *   input.width  = entry->orig_desc.Width;
     *   input.height = entry->orig_desc.Height;
     *   input.stride = mapped.RowPitch;
     *
     *   bc1_enc_settings settings;
     *   GetProfile_fast(&settings);
     *
     *   uint8_t* compressed = new uint8_t[new_size];
     *   CompressBlocksBC1(&input, compressed, &settings);
     *
     *   pContext->Unmap(original_tex, 0);
     *
     *   D3D11_SUBRESOURCE_DATA init_data = {};
     *   init_data.pSysMem          = compressed;
     *   init_data.SysMemPitch      = ((new_desc.Width + 3) / 4) * 8;
     *
     *   ID3D11Texture2D* new_tex = nullptr;
     *   pDevice->CreateTexture2D(&new_desc, &init_data, &new_tex);
     *
     *   delete[] compressed;
     */

    // Atualiza mapa local
    EnterCriticalSection(&m_tex_cs);
    if (TrackedTexture* t = FindTexture(texture_id)) {
        t->is_compressed     = true;
        t->compression_level = level;
    }
    LeaveCriticalSection(&m_tex_cs);
}

// ─────────────────────────────────────────────────────────────────────────────
// DllMain — ponto de entrada quando injetado no jogo
// ─────────────────────────────────────────────────────────────────────────────

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);
            // Instala o hook em uma thread separada para não bloquear o DllMain
            // (DllMain tem restrições severas — evitamos locks e I/O aqui)
            CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
                Sleep(300); // aguarda o jogo inicializar o runtime DX11
                HookDX11::Instance().Install();
                return 0;
            }, nullptr, 0, nullptr);
            break;
        }
        case DLL_PROCESS_DETACH:
            if (!reserved) { // não estamos sendo descarregados por TerminateProcess
                HookDX11::Instance().Uninstall();
            }
            break;
    }
    return TRUE;
}