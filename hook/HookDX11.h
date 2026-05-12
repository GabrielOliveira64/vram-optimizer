#pragma once

/*
 * HookDX11.h — v2
 * Adiciona VRAMReporter: lê uso de VRAM de dentro do processo do jogo.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdint>
#include <atomic>

class HookDX11 {
public:
    static HookDX11& Instance();

    bool Install();
    void Uninstall();
    bool IsInstalled() const { return m_installed.load(); }

private:
    HookDX11()  = default;
    ~HookDX11() = default;
    HookDX11(const HookDX11&)            = delete;
    HookDX11& operator=(const HookDX11&) = delete;

    // ── Setup ────────────────────────────────────────────────────────────────
    bool HookVTable();
    bool ConnectPipes();
    void StartCommandListener();
    void StartVRAMReporter();       // ← novo
    void Log(const char* fmt, ...) const;

    // ── Loops de thread ───────────────────────────────────────────────────────
    void CommandListenerLoop();
    void VRAMReporterLoop();        // ← novo: envia VRAMSnapshotEvent a cada 500ms

    // ── Hooks estáticos ───────────────────────────────────────────────────────
    static HRESULT WINAPI Hooked_CreateTexture2D(
        ID3D11Device*,
        const D3D11_TEXTURE2D_DESC*,
        const D3D11_SUBRESOURCE_DATA*,
        ID3D11Texture2D**);

    static ULONG WINAPI Hooked_Release(IUnknown*);

    // ── Lógica de textura ─────────────────────────────────────────────────────
    void OnTextureCreated (ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& desc);
    void OnTextureReleased(uint64_t texture_id);
    void ApplyCompression (uint64_t texture_id, uint32_t level,
                           uint32_t target_w, uint32_t target_h);

    // ── IPC ───────────────────────────────────────────────────────────────────
    bool SendEvent(const void* header, DWORD header_size,
                   const void* payload, DWORD payload_size);

    // ── Estado ────────────────────────────────────────────────────────────────
    std::atomic<bool> m_installed{ false };
    std::atomic<bool> m_running  { false };

    HANDLE m_pipe_events   = INVALID_HANDLE_VALUE;
    HANDLE m_pipe_commands = INVALID_HANDLE_VALUE;
    HANDLE m_log_mutex     = nullptr;
    FILE*  m_log_file      = nullptr;
    HANDLE m_cmd_thread    = nullptr;
    HANDLE m_vram_thread   = nullptr;    // ← novo

    // Tipos das funções originais
    using PFN_CreateTexture2D = HRESULT(WINAPI*)(
        ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
        const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
    using PFN_Release = ULONG(WINAPI*)(IUnknown*);

    PFN_CreateTexture2D m_orig_CreateTexture2D = nullptr;
    PFN_Release         m_orig_Release         = nullptr;

    // Mapa de texturas ativas
    CRITICAL_SECTION m_tex_cs;

    struct TrackedTexture {
        D3D11_TEXTURE2D_DESC orig_desc;
        uint64_t             size_bytes;
        bool                 is_compressed;
        uint32_t             compression_level;
    };

    static constexpr int MAX_TEXTURES = 4096;
    uint64_t       m_tex_ids [MAX_TEXTURES] = {};
    TrackedTexture m_textures[MAX_TEXTURES] = {};
    int            m_tex_count = 0;

    TrackedTexture* FindTexture  (uint64_t id);
    void            AddTexture   (uint64_t id, const TrackedTexture& t);
    void            RemoveTexture(uint64_t id);
};