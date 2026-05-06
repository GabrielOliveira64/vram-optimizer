#pragma once

/*
 * HookDX11.h
 * ─────────────────────────────────────────────────────────────────────────────
 * Hook de texturas para DirectX 11 via MinHook.
 *
 * O que este módulo faz:
 *   1. Instala hooks em ID3D11Device::CreateTexture2D e ::Release
 *   2. Detecta cada textura criada/destruída pelo jogo
 *   3. Envia eventos ao Orchestrator via Named Pipe (IPCProtocol.h)
 *   4. Recebe comandos de compressão e aplica em tempo real
 *   5. Mantém um mapa interno de texturas ativas para substituição rápida
 *
 * Dependências:
 *   - MinHook: https://github.com/TsudaKageyu/minhook
 *     Baixe e coloque MinHook.h + MinHook.x64.lib em third_party/minhook/
 *   - DirectX 11 SDK (incluso no Windows SDK)
 *
 * Injeção:
 *   Compile como DLL e injete no processo do jogo com qualquer injector.
 *   Recomendados: dll_injector, Extreme Injector, ou injector próprio (Módulo 5).
 *
 * Compilar:
 *   cl /std:c++17 /EHsc /LD HookDX11.cpp ^
 *      /I"third_party/minhook" ^
 *      /link MinHook.x64.lib d3d11.lib dxgi.lib ^
 *      /OUT:vram_hook.dll
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include <atomic>

// ─────────────────────────────────────────────────────────────────────────────

class HookDX11 {
public:
    static HookDX11& Instance();

    // Chamado pelo DllMain quando a DLL é injetada
    bool Install();

    // Chamado pelo DllMain ao ser removida
    void Uninstall();

    bool IsInstalled() const { return m_installed.load(); }

private:
    HookDX11() = default;
    ~HookDX11() = default;
    HookDX11(const HookDX11&) = delete;
    HookDX11& operator=(const HookDX11&) = delete;

    // ── Setup ────────────────────────────────────────────────────────────────
    bool HookVTable();
    bool ConnectPipes();
    void StartCommandListener();
    void Log(const char* fmt, ...) const;

    // ── Funções hooked (estáticas para compatibilidade com MinHook) ──────────
    static HRESULT WINAPI Hooked_CreateTexture2D(
        ID3D11Device*                 pDevice,
        const D3D11_TEXTURE2D_DESC*   pDesc,
        const D3D11_SUBRESOURCE_DATA* pInitialData,
        ID3D11Texture2D**             ppTexture2D);

    static ULONG WINAPI Hooked_Release(IUnknown* pThis);

    // ── Lógica de processamento ───────────────────────────────────────────────
    void OnTextureCreated(ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& desc);
    void OnTextureReleased(uint64_t texture_id);
    void ApplyCompression(uint64_t texture_id, uint32_t level,
                          uint32_t target_w, uint32_t target_h);
    void CommandListenerLoop();

    // ── IPC ──────────────────────────────────────────────────────────────────
    bool SendEvent(const void* header, DWORD header_size,
                   const void* payload, DWORD payload_size);

    // ── Estado interno ────────────────────────────────────────────────────────
    std::atomic<bool> m_installed{ false };
    std::atomic<bool> m_running  { false };

    HANDLE m_pipe_events   = INVALID_HANDLE_VALUE;  // Hook → Orquestrador
    HANDLE m_pipe_commands = INVALID_HANDLE_VALUE;  // Orquestrador → Hook
    HANDLE m_log_mutex     = nullptr;

    FILE*  m_log_file      = nullptr;
    HANDLE m_cmd_thread    = nullptr;

    // Ponteiros para as funções originais (preenchidos pelo MinHook)
    using PFN_CreateTexture2D = HRESULT(WINAPI*)(
        ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
        const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
    using PFN_Release = ULONG(WINAPI*)(IUnknown*);

    PFN_CreateTexture2D m_orig_CreateTexture2D = nullptr;
    PFN_Release         m_orig_Release         = nullptr;

    // Mapa de texturas ativas: texture_id → cópia da desc original
    // (protegido por m_tex_mutex)
    CRITICAL_SECTION m_tex_cs;

    struct TrackedTexture {
        D3D11_TEXTURE2D_DESC orig_desc;
        uint64_t             size_bytes;
        bool                 is_compressed;
        uint32_t             compression_level;
    };

    // Array fixo para evitar alocação dinâmica no caminho crítico
    static constexpr int MAX_TEXTURES = 4096;
    uint64_t       m_tex_ids [MAX_TEXTURES] = {};
    TrackedTexture m_textures[MAX_TEXTURES] = {};
    int            m_tex_count = 0;

    TrackedTexture* FindTexture(uint64_t id);
    void AddTexture(uint64_t id, const TrackedTexture& t);
    void RemoveTexture(uint64_t id);
};
