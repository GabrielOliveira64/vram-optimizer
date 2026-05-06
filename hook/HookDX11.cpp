/*
 * HookDX11.cpp — Parte 1: Setup, VTable Hook, IPC
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "HookDX11.h"
#include "../ipc/IPCProtocol.h"
#include <MinHook.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <chrono>
#include <thread>

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

    // Log para arquivo e console
    AllocConsole();
    fopen_s(&m_log_file, "vram_hook.log", "a");
    m_log_mutex = CreateMutexA(nullptr, FALSE, nullptr);
    InitializeCriticalSection(&m_tex_cs);

    Log("====== VRAM Hook DX11 — Iniciando ======");

    // 1. Conecta aos pipes do orquestrador
    if (!ConnectPipes()) {
        Log("[AVISO] Orquestrador nao encontrado. Rodando em modo standalone (so log).");
    }

    // 2. Instala hooks no vtable do D3D11
    if (!HookVTable()) {
        Log("[ERRO] Falha ao instalar hooks no vtable.");
        return false;
    }

    // 3. Inicia thread que escuta comandos do orquestrador
    m_running = true;
    StartCommandListener();

    m_installed = true;
    Log("Hook instalado com sucesso.");
    return true;
}

void HookDX11::Uninstall() {
    if (!m_installed.load()) return;

    m_running = false;
    m_installed = false;

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (m_cmd_thread) {
        WaitForSingleObject(m_cmd_thread, 3000);
        CloseHandle(m_cmd_thread);
        m_cmd_thread = nullptr;
    }
    if (m_pipe_events   != INVALID_HANDLE_VALUE) CloseHandle(m_pipe_events);
    if (m_pipe_commands != INVALID_HANDLE_VALUE) CloseHandle(m_pipe_commands);
    if (m_log_mutex) CloseHandle(m_log_mutex);
    if (m_log_file)  fclose(m_log_file);

    DeleteCriticalSection(&m_tex_cs);

    Log("Hook removido.");
}

// ─────────────────────────────────────────────────────────────────────────────
// VTable Hook via MinHook
// ─────────────────────────────────────────────────────────────────────────────

bool HookDX11::HookVTable() {
    if (MH_Initialize() != MH_OK) {
        Log("[ERRO] MH_Initialize falhou.");
        return false;
    }

    // Precisamos de um device temporário só para ler o vtable.
    // O jogo já inicializou o DX11, mas queremos o endereço correto
    // da função — não importa qual device usamos.
    ID3D11Device*        tmp_device  = nullptr;
    ID3D11DeviceContext* tmp_context = nullptr;
    D3D_FEATURE_LEVEL    fl;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION,
        &tmp_device, &fl, &tmp_context);

    if (FAILED(hr)) {
        // Fallback: tenta com WARP (software renderer) — sempre disponível
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            0, nullptr, 0, D3D11_SDK_VERSION,
            &tmp_device, &fl, &tmp_context);
    }

    if (FAILED(hr)) {
        Log("[ERRO] Nao foi possivel criar device DX11 temporario: 0x%08X", hr);
        return false;
    }

    // ── CreateTexture2D: slot 5 no vtable do ID3D11Device ────────────────────
    // Layout do vtable de ID3D11Device (dx11.h):
    //   [0]  QueryInterface    [1]  AddRef        [2]  Release
    //   [3]  CreateBuffer      [4]  CreateTexture1D
    //   [5]  CreateTexture2D   ← aqui
    //   [6]  CreateTexture3D   [7]  CreateShaderResourceView ...
    void** vtable = *reinterpret_cast<void***>(tmp_device);
    void*  pfn_create = vtable[5];

    MH_STATUS status = MH_CreateHook(
        pfn_create,
        reinterpret_cast<void*>(&HookDX11::Hooked_CreateTexture2D),
        reinterpret_cast<void**>(&m_orig_CreateTexture2D));

    if (status != MH_OK) {
        Log("[ERRO] MH_CreateHook(CreateTexture2D) = %d", (int)status);
        tmp_context->Release();
        tmp_device->Release();
        return false;
    }

    MH_EnableHook(pfn_create);
    Log("Hook CreateTexture2D instalado (vtable slot 5, addr=%p)", pfn_create);

    tmp_context->Release();
    tmp_device->Release();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// IPC: Conecta aos Named Pipes do Orquestrador
// ─────────────────────────────────────────────────────────────────────────────

bool HookDX11::ConnectPipes() {
    // Tenta por até 3 segundos (orquestrador pode estar iniciando)
    for (int attempt = 0; attempt < 6; ++attempt) {
        m_pipe_events = CreateFileA(
            PIPE_EVENTS,
            GENERIC_WRITE,
            0, nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr);

        if (m_pipe_events != INVALID_HANDLE_VALUE) break;
        Sleep(500);
    }

    if (m_pipe_events == INVALID_HANDLE_VALUE) {
        Log("[IPC] Pipe de eventos nao disponivel (%lu). Modo standalone.", GetLastError());
        return false;
    }

    // Pipe de comandos (leitura de comandos vindos do orquestrador)
    m_pipe_commands = CreateFileA(
        PIPE_COMMANDS,
        GENERIC_READ,
        0, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);

    if (m_pipe_commands == INVALID_HANDLE_VALUE) {
        Log("[IPC] Pipe de comandos nao disponivel (%lu).", GetLastError());
        // Eventos ainda funcionam; seguimos sem comandos
    }

    Log("[IPC] Conectado ao orquestrador.");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread de escuta de comandos (Orquestrador → Hook)
// ─────────────────────────────────────────────────────────────────────────────

void HookDX11::StartCommandListener() {
    m_cmd_thread = CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
        reinterpret_cast<HookDX11*>(param)->CommandListenerLoop();
        return 0;
    }, this, 0, nullptr);
}

void HookDX11::CommandListenerLoop() {
    Log("[IPC] Thread de comandos iniciada.");
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

    while (m_running.load()) {
        if (m_pipe_commands == INVALID_HANDLE_VALUE) {
            Sleep(100);
            continue;
        }

        // Lê header do comando
        CommandHeader hdr = {};
        DWORD bytes_read = 0;
        ResetEvent(ov.hEvent);

        BOOL ok = ReadFile(m_pipe_commands, &hdr, sizeof(hdr), nullptr, &ov);
        DWORD err = GetLastError();

        if (!ok && err == ERROR_IO_PENDING) {
            // Aguarda com timeout para poder checar m_running
            DWORD wait = WaitForSingleObject(ov.hEvent, 200);
            if (wait == WAIT_TIMEOUT) continue;
            GetOverlappedResult(m_pipe_commands, &ov, &bytes_read, FALSE);
        } else if (!ok) {
            // Pipe quebrado — tenta reconectar
            Log("[IPC] Pipe de comandos encerrado (%lu). Tentando reconectar...", err);
            CloseHandle(m_pipe_commands);
            m_pipe_commands = INVALID_HANDLE_VALUE;
            Sleep(1000);
            ConnectPipes();
            continue;
        }

        // Processa o comando
        switch (hdr.type) {
            case CommandType::CompressTexture: {
                CompressTextureCommand cmd = {};
                DWORD br = 0;
                ReadFile(m_pipe_commands, &cmd, sizeof(cmd), &br, nullptr);
                if (br == sizeof(cmd)) {
                    ApplyCompression(cmd.texture_id, (uint32_t)cmd.level,
                                     cmd.target_width, cmd.target_height);
                }
                break;
            }
            case CommandType::SetMaxDimension: {
                SetMaxDimensionCommand cmd = {};
                DWORD br = 0;
                ReadFile(m_pipe_commands, &cmd, sizeof(cmd), &br, nullptr);
                Log("[CMD] SetMaxDimension: %u px", cmd.max_dimension);
                break;
            }
            case CommandType::Ping:
                Log("[CMD] Ping recebido do orquestrador.");
                break;
            default:
                Log("[CMD] Comando desconhecido: %u", (uint32_t)hdr.type);
                break;
        }
    }

    CloseHandle(ov.hEvent);
    Log("[IPC] Thread de comandos encerrada.");
}

// ─────────────────────────────────────────────────────────────────────────────
// Envio de eventos via pipe
// ─────────────────────────────────────────────────────────────────────────────

bool HookDX11::SendEvent(const void* header, DWORD hdr_size,
                         const void* payload, DWORD payload_size) {
    if (m_pipe_events == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    // Header
    if (!WriteFile(m_pipe_events, header, hdr_size, &written, nullptr))
        return false;
    // Payload
    if (payload && payload_size > 0) {
        if (!WriteFile(m_pipe_events, payload, payload_size, &written, nullptr))
            return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Log thread-safe
// ─────────────────────────────────────────────────────────────────────────────

void HookDX11::Log(const char* fmt, ...) const {
    if (m_log_mutex) WaitForSingleObject(m_log_mutex, INFINITE);

    SYSTEMTIME st = {};
    GetLocalTime(&st);

    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    printf("[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, buf);

    if (m_log_file) {
        fprintf(m_log_file, "[%02d:%02d:%02d] %s\n",
                st.wHour, st.wMinute, st.wSecond, buf);
        fflush(m_log_file);
    }

    if (m_log_mutex) ReleaseMutex(m_log_mutex);
}
