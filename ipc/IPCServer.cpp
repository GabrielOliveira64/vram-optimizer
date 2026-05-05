/*
 * IPCServer.cpp
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "IPCServer.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <windows.h>

// ─────────────────────────────────────────────────────────────────────────────
// Start / Stop
// ─────────────────────────────────────────────────────────────────────────────

void IPCServer::Start() {
    if (m_running.exchange(true)) return;

    m_cmd_mutex = CreateMutexA(nullptr, FALSE, nullptr);

    // ── Pipe de eventos: servidor cria, hook conecta como cliente ─────────────
    m_pipe_events = CreateNamedPipeA(
        PIPE_EVENTS,
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,          // máximo de instâncias (só 1 hook por vez)
        4096,       // buffer de saída
        4096,       // buffer de entrada
        PIPE_TIMEOUT_MS,
        nullptr);

    if (m_pipe_events == INVALID_HANDLE_VALUE) {
        Log("[IPC] ERRO ao criar pipe de eventos: %lu", GetLastError());
    } else {
        Log("[IPC] Pipe de eventos criado. Aguardando hook...");
    }

    // ── Pipe de comandos: servidor cria, hook conecta como cliente ────────────
    m_pipe_commands = CreateNamedPipeA(
        PIPE_COMMANDS,
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        4096, 4096,
        PIPE_TIMEOUT_MS,
        nullptr);

    if (m_pipe_commands == INVALID_HANDLE_VALUE) {
        Log("[IPC] ERRO ao criar pipe de comandos: %lu", GetLastError());
    }

    // Thread que aceita conexão e lê eventos
    m_event_thread = std::thread(&IPCServer::EventListenerLoop, this);
}

void IPCServer::Stop() {
    if (!m_running.exchange(false)) return;

    // Fecha os pipes para desbloquear ConnectNamedPipe / ReadFile em espera
    if (m_pipe_events   != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(m_pipe_events);
        CloseHandle(m_pipe_events);
        m_pipe_events = INVALID_HANDLE_VALUE;
    }
    if (m_pipe_commands != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(m_pipe_commands);
        CloseHandle(m_pipe_commands);
        m_pipe_commands = INVALID_HANDLE_VALUE;
    }
    if (m_event_thread.joinable()) m_event_thread.join();
    if (m_cmd_mutex) {
        CloseHandle(m_cmd_mutex);
        m_cmd_mutex = nullptr;
    }
    Log("[IPC] Servidor parado.");
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop de escuta de eventos (Hook → Orquestrador)
// ─────────────────────────────────────────────────────────────────────────────

void IPCServer::EventListenerLoop() {
    Log("[IPC] Aguardando conexao do hook...");

    // Aguarda o hook conectar (bloqueante com timeout)
    OVERLAPPED ov_connect = {};
    ov_connect.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

    ConnectNamedPipe(m_pipe_events, &ov_connect);
    DWORD wait = WaitForSingleObject(ov_connect.hEvent, 30000); // 30s timeout
    CloseHandle(ov_connect.hEvent);

    if (wait != WAIT_OBJECT_0 || !m_running.load()) {
        Log("[IPC] Hook nao conectou em 30s. Servidor encerrado.");
        return;
    }

    Log("[IPC] Hook conectado! Recebendo eventos...");

    // Conecta o pipe de comandos ao hook (agora que ele está pronto)
    OVERLAPPED ov_cmd = {};
    ov_cmd.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    ConnectNamedPipe(m_pipe_commands, &ov_cmd);
    WaitForSingleObject(ov_cmd.hEvent, 2000);
    CloseHandle(ov_cmd.hEvent);

    // Loop de leitura de eventos
    OVERLAPPED ov_read = {};
    ov_read.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

    while (m_running.load()) {
        // Lê o header
        EventHeader hdr = {};
        DWORD bytes_read = 0;
        ResetEvent(ov_read.hEvent);

        BOOL ok = ReadFile(m_pipe_events, &hdr, sizeof(hdr), nullptr, &ov_read);
        DWORD err = GetLastError();

        if (!ok && err == ERROR_IO_PENDING) {
            DWORD wr = WaitForSingleObject(ov_read.hEvent, 500);
            if (wr == WAIT_TIMEOUT) continue;
            if (!GetOverlappedResult(m_pipe_events, &ov_read, &bytes_read, FALSE)) {
                err = GetLastError();
                if (err == ERROR_BROKEN_PIPE) break;
                continue;
            }
        } else if (!ok) {
            if (err == ERROR_BROKEN_PIPE) break;
            continue;
        }

        // Lê e despacha o payload
        switch (hdr.type) {

            case EventType::TextureCreated: {
                TextureCreatedEvent evt = {};
                ReadFile(m_pipe_events, &evt, sizeof(evt), &bytes_read, nullptr);
                if (bytes_read == sizeof(evt) && OnTextureCreated)
                    OnTextureCreated(evt);
                break;
            }

            case EventType::TextureReleased: {
                TextureReleasedEvent evt = {};
                ReadFile(m_pipe_events, &evt, sizeof(evt), &bytes_read, nullptr);
                if (bytes_read == sizeof(evt) && OnTextureReleased)
                    OnTextureReleased(evt);
                break;
            }

            case EventType::VRAMSnapshot: {
                VRAMSnapshotEvent evt = {};
                ReadFile(m_pipe_events, &evt, sizeof(evt), &bytes_read, nullptr);
                if (bytes_read == sizeof(evt) && OnVRAMSnapshot)
                    OnVRAMSnapshot(evt);
                break;
            }

            default:
                // Payload desconhecido — descarta os bytes para não dessincronizar
                if (hdr.payload_size > 0 && hdr.payload_size < 65536) {
                    std::vector<char> discard(hdr.payload_size);
                    ReadFile(m_pipe_events, discard.data(), hdr.payload_size,
                             &bytes_read, nullptr);
                }
                Log("[IPC] Evento desconhecido: type=%u", (uint32_t)hdr.type);
                break;
        }
    }

    CloseHandle(ov_read.hEvent);
    Log("[IPC] Conexao com hook encerrada.");
}

// ─────────────────────────────────────────────────────────────────────────────
// Envio de comandos (Orquestrador → Hook)
// ─────────────────────────────────────────────────────────────────────────────

bool IPCServer::SendCompressCommand(uint64_t texture_id,
                                    IPCCompressionLevel level,
                                    uint32_t target_w,
                                    uint32_t target_h)
{
    if (m_pipe_commands == INVALID_HANDLE_VALUE) return false;

    CommandHeader hdr = {};
    hdr.type         = CommandType::CompressTexture;
    hdr.payload_size = sizeof(CompressTextureCommand);

    CompressTextureCommand cmd = {};
    cmd.texture_id    = texture_id;
    cmd.level         = level;
    cmd.target_width  = target_w;
    cmd.target_height = target_h;

    WaitForSingleObject(m_cmd_mutex, INFINITE);

    DWORD written = 0;
    bool ok = WriteFile(m_pipe_commands, &hdr, sizeof(hdr), &written, nullptr)
           && WriteFile(m_pipe_commands, &cmd, sizeof(cmd), &written, nullptr);

    ReleaseMutex(m_cmd_mutex);
    return ok;
}

bool IPCServer::SendPing() {
    if (m_pipe_commands == INVALID_HANDLE_VALUE) return false;

    CommandHeader hdr = {};
    hdr.type         = CommandType::Ping;
    hdr.payload_size = 0;

    WaitForSingleObject(m_cmd_mutex, INFINITE);
    DWORD written = 0;
    bool ok = WriteFile(m_pipe_commands, &hdr, sizeof(hdr), &written, nullptr);
    ReleaseMutex(m_cmd_mutex);
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// Log
// ─────────────────────────────────────────────────────────────────────────────

void IPCServer::Log(const char* fmt, ...) const {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printf("[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
}
