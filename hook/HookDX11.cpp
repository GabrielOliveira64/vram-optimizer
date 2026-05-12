/*
 * HookDX11.cpp — v2
 * Adicionado: VRAMReporterLoop — lê uso real de VRAM de dentro do processo
 * do jogo via DXGI 1.4 e envia VRAMSnapshotEvent ao orquestrador a cada 500ms.
 * Isso resolve o problema de VRAM sempre mostrando 0%.
 */

#define NOMINMAX
#include "HookDX11.h"
#include "../ipc/IPCProtocol.h"
#include <MinHook.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <chrono>
#include <thread>
#include <algorithm>

// ─── Singleton ────────────────────────────────────────────────────────────────

HookDX11& HookDX11::Instance() {
    static HookDX11 inst;
    return inst;
}

// ─────────────────────────────────────────────────────────────────────────────
// Install / Uninstall
// ─────────────────────────────────────────────────────────────────────────────

bool HookDX11::Install() {
    if (m_installed.load()) return true;

    AllocConsole();
    fopen_s(&m_log_file, "vram_hook.log", "a");
    m_log_mutex = CreateMutexA(nullptr, FALSE, nullptr);
    InitializeCriticalSection(&m_tex_cs);

    Log("====== VRAM Hook DX11 v2 ======");

    if (!ConnectPipes())
        Log("[AVISO] Orquestrador nao encontrado. Modo log apenas.");

    if (!HookVTable()) {
        Log("[ERRO] Falha ao instalar hooks.");
        return false;
    }

    m_running = true;
    StartCommandListener();
    StartVRAMReporter();

    m_installed = true;
    Log("Hook instalado com sucesso.");
    return true;
}

void HookDX11::Uninstall() {
    if (!m_installed.load()) return;
    m_running   = false;
    m_installed = false;

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (m_cmd_thread) {
        WaitForSingleObject(m_cmd_thread, 3000);
        CloseHandle(m_cmd_thread);
        m_cmd_thread = nullptr;
    }
    if (m_vram_thread) {
        WaitForSingleObject(m_vram_thread, 3000);
        CloseHandle(m_vram_thread);
        m_vram_thread = nullptr;
    }
    if (m_pipe_events   != INVALID_HANDLE_VALUE) CloseHandle(m_pipe_events);
    if (m_pipe_commands != INVALID_HANDLE_VALUE) CloseHandle(m_pipe_commands);
    if (m_log_mutex) CloseHandle(m_log_mutex);
    if (m_log_file)  fclose(m_log_file);
    DeleteCriticalSection(&m_tex_cs);
    Log("Hook removido.");
}

// ─────────────────────────────────────────────────────────────────────────────
// VRAMReporter — lê VRAM de DENTRO do processo do jogo
// ─────────────────────────────────────────────────────────────────────────────

void HookDX11::StartVRAMReporter() {
    m_vram_thread = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        reinterpret_cast<HookDX11*>(p)->VRAMReporterLoop();
        return 0;
    }, this, 0, nullptr);
}

void HookDX11::VRAMReporterLoop() {
    Log("[VRAM] Reporter iniciado.");

    // Inicializa DXGI dentro do processo do jogo para leitura local
    IDXGIFactory2* factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&factory);

    IDXGIAdapter3* adapter3 = nullptr;
    if (factory) {
        IDXGIAdapter1* a1 = nullptr;
        if (SUCCEEDED(factory->EnumAdapters1(0, &a1))) {
            a1->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&adapter3);
            a1->Release();
        }
        factory->Release();
    }

    if (!adapter3) {
        Log("[VRAM] IDXGIAdapter3 indisponivel.");
        return;
    }

    DXGI_ADAPTER_DESC desc = {};
    adapter3->GetDesc(&desc);
    uint64_t dedicated = desc.DedicatedVideoMemory;
    Log("[VRAM] GPU: %S | Dedicada: %.0f MB",
        desc.Description, dedicated / (1024.0 * 1024.0));

    uint32_t frame = 0;
    while (m_running.load()) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        {
            uint64_t budget = (info.Budget > 0) ? info.Budget : dedicated;
            uint64_t used   = info.CurrentUsage;

            EventHeader ehdr = { EventType::VRAMSnapshot, sizeof(VRAMSnapshotEvent) };
            VRAMSnapshotEvent evt = { used, budget, frame++ };
            SendEvent(&ehdr, sizeof(ehdr), &evt, sizeof(evt));
        }
        Sleep(500);
    }

    adapter3->Release();
    Log("[VRAM] Reporter encerrado.");
}

// ─────────────────────────────────────────────────────────────────────────────
// HookVTable
// ─────────────────────────────────────────────────────────────────────────────

bool HookDX11::HookVTable() {
    if (MH_Initialize() != MH_OK) { Log("[ERRO] MH_Initialize falhou."); return false; }

    ID3D11Device*        tmp_dev = nullptr;
    ID3D11DeviceContext* tmp_ctx = nullptr;
    D3D_FEATURE_LEVEL    fl;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &tmp_dev, &fl, &tmp_ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
            nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &tmp_dev, &fl, &tmp_ctx);
    if (FAILED(hr)) { Log("[ERRO] D3D11CreateDevice: 0x%08X", hr); return false; }

    void** vtable = *reinterpret_cast<void***>(tmp_dev);
    void*  pfn    = vtable[5]; // CreateTexture2D

    MH_STATUS s = MH_CreateHook(pfn,
        reinterpret_cast<void*>(&HookDX11::Hooked_CreateTexture2D),
        reinterpret_cast<void**>(&m_orig_CreateTexture2D));

    if (s != MH_OK) {
        Log("[ERRO] MH_CreateHook = %d", (int)s);
        tmp_ctx->Release(); tmp_dev->Release(); return false;
    }
    MH_EnableHook(pfn);
    Log("Hook CreateTexture2D slot 5 = %p", pfn);

    tmp_ctx->Release(); tmp_dev->Release();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ConnectPipes
// ─────────────────────────────────────────────────────────────────────────────

bool HookDX11::ConnectPipes() {
    for (int i = 0; i < 6; ++i) {
        m_pipe_events = CreateFileA(PIPE_EVENTS, GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (m_pipe_events != INVALID_HANDLE_VALUE) break;
        Sleep(500);
    }
    if (m_pipe_events == INVALID_HANDLE_VALUE) {
        Log("[IPC] Pipe eventos: %lu", GetLastError()); return false;
    }
    m_pipe_commands = CreateFileA(PIPE_COMMANDS, GENERIC_READ,
        0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (m_pipe_commands == INVALID_HANDLE_VALUE)
        Log("[IPC] Pipe cmds: %lu", GetLastError());
    Log("[IPC] Conectado ao orquestrador.");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// CommandListenerLoop
// ─────────────────────────────────────────────────────────────────────────────

void HookDX11::StartCommandListener() {
    m_cmd_thread = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        reinterpret_cast<HookDX11*>(p)->CommandListenerLoop(); return 0;
    }, this, 0, nullptr);
}

void HookDX11::CommandListenerLoop() {
    OVERLAPPED ov = {}; ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    while (m_running.load()) {
        if (m_pipe_commands == INVALID_HANDLE_VALUE) { Sleep(100); continue; }
        CommandHeader hdr = {}; DWORD br = 0;
        ResetEvent(ov.hEvent);
        BOOL ok = ReadFile(m_pipe_commands, &hdr, sizeof(hdr), nullptr, &ov);
        DWORD err = GetLastError();
        if (!ok && err == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 200) == WAIT_TIMEOUT) continue;
            GetOverlappedResult(m_pipe_commands, &ov, &br, FALSE);
        } else if (!ok) {
            CloseHandle(m_pipe_commands); m_pipe_commands = INVALID_HANDLE_VALUE;
            Sleep(1000); ConnectPipes(); continue;
        }
        switch (hdr.type) {
            case CommandType::CompressTexture: {
                CompressTextureCommand cmd = {};
                ReadFile(m_pipe_commands, &cmd, sizeof(cmd), &br, nullptr);
                if (br == sizeof(cmd))
                    ApplyCompression(cmd.texture_id, (uint32_t)cmd.level,
                                     cmd.target_width, cmd.target_height);
                break;
            }
            case CommandType::Ping: Log("[CMD] Ping."); break;
            default: break;
        }
    }
    CloseHandle(ov.hEvent);
}

// ─────────────────────────────────────────────────────────────────────────────
// SendEvent / Log
// ─────────────────────────────────────────────────────────────────────────────

bool HookDX11::SendEvent(const void* hdr, DWORD hdr_size,
                         const void* payload, DWORD payload_size) {
    if (m_pipe_events == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    if (!WriteFile(m_pipe_events, hdr, hdr_size, &w, nullptr)) return false;
    if (payload && payload_size > 0)
        WriteFile(m_pipe_events, payload, payload_size, &w, nullptr);
    return true;
}

void HookDX11::Log(const char* fmt, ...) const {
    if (m_log_mutex) WaitForSingleObject(m_log_mutex, INFINITE);
    SYSTEMTIME st = {}; GetLocalTime(&st);
    char buf[512]; va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    printf("[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
    if (m_log_file) {
        fprintf(m_log_file, "[%02d:%02d:%02d] %s\n",
                st.wHour, st.wMinute, st.wSecond, buf);
        fflush(m_log_file);
    }
    if (m_log_mutex) ReleaseMutex(m_log_mutex);
}