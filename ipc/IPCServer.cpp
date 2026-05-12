/*
 * IPCServer.cpp — v2
 * ─────────────────────────────────────────────────────────────────────────────
 * Correções principais:
 *   1. Loop de reconexão — se o hook desconectar/reinjetar, reconecta
 *      automaticamente sem precisar reiniciar o orquestrador
 *   2. PIPE_MAXINSTANCES aumentado para 2 para evitar ERROR_PIPE_BUSY
 *   3. VRAMSnapshotEvent aproveitado para reportar uso real do jogo
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "IPCServer.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <vector>
#include <algorithm>

// ─── Start / Stop ─────────────────────────────────────────────────────────────

void IPCServer::Start() {
    if (m_running.exchange(true)) return;
    m_cmd_mutex   = CreateMutexA(nullptr, FALSE, nullptr);
    m_event_thread = std::thread(&IPCServer::EventListenerLoop, this);
    Log("[IPC] Servidor iniciado. Aguardando hook...");
}

void IPCServer::Stop() {
    if (!m_running.exchange(false)) return;

    // Cancela I/O pendente fechando os handles
    if (m_pipe_events != INVALID_HANDLE_VALUE) {
        CancelIo(m_pipe_events);
        DisconnectNamedPipe(m_pipe_events);
        CloseHandle(m_pipe_events);
        m_pipe_events = INVALID_HANDLE_VALUE;
    }
    if (m_pipe_commands != INVALID_HANDLE_VALUE) {
        CancelIo(m_pipe_commands);
        DisconnectNamedPipe(m_pipe_commands);
        CloseHandle(m_pipe_commands);
        m_pipe_commands = INVALID_HANDLE_VALUE;
    }
    if (m_event_thread.joinable()) m_event_thread.join();
    if (m_cmd_mutex) { CloseHandle(m_cmd_mutex); m_cmd_mutex = nullptr; }
    Log("[IPC] Servidor parado.");
}

// ─── Criação dos pipes ────────────────────────────────────────────────────────

static HANDLE CreateEventPipe() {
    HANDLE h = CreateNamedPipeA(
        PIPE_EVENTS,
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        2,      // 2 instâncias — evita ERROR_PIPE_BUSY na reinjeção
        8192, 8192, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        printf("[IPC] ERRO CreateNamedPipe(events): %lu\n", GetLastError());
    return h;
}

static HANDLE CreateCommandPipe() {
    HANDLE h = CreateNamedPipeA(
        PIPE_COMMANDS,
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        2,
        8192, 8192, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        printf("[IPC] ERRO CreateNamedPipe(commands): %lu\n", GetLastError());
    return h;
}

// ─── Loop principal com reconexão automática ──────────────────────────────────

void IPCServer::EventListenerLoop() {
    while (m_running.load()) {

        // Cria pipes frescos para cada conexão
        m_pipe_events   = CreateEventPipe();
        m_pipe_commands = CreateCommandPipe();

        if (m_pipe_events   == INVALID_HANDLE_VALUE ||
            m_pipe_commands == INVALID_HANDLE_VALUE)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        Log("[IPC] Aguardando hook conectar...");

        // ── Aguarda o hook conectar no pipe de eventos ────────────────────────
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        ConnectNamedPipe(m_pipe_events, &ov);

        // Polling com timeout para checar m_running
        bool connected = false;
        while (m_running.load()) {
            DWORD w = WaitForSingleObject(ov.hEvent, 500);
            if (w == WAIT_OBJECT_0) { connected = true; break; }
            // WAIT_TIMEOUT → continua o loop
        }
        CloseHandle(ov.hEvent);

        if (!connected || !m_running.load()) {
            CloseHandle(m_pipe_events);   m_pipe_events   = INVALID_HANDLE_VALUE;
            CloseHandle(m_pipe_commands); m_pipe_commands = INVALID_HANDLE_VALUE;
            break;
        }

        Log("[IPC] Hook conectado! Recebendo eventos...");

        // ── Conecta o pipe de comandos ────────────────────────────────────────
        {
            OVERLAPPED ovc = {};
            ovc.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
            ConnectNamedPipe(m_pipe_commands, &ovc);
            WaitForSingleObject(ovc.hEvent, 3000);
            CloseHandle(ovc.hEvent);
        }

        // ── Loop de leitura ───────────────────────────────────────────────────
        ReadLoop();

        // Hook desconectou — limpa e volta para aguardar nova conexão
        Log("[IPC] Hook desconectou. Aguardando reinjeção...");
        DisconnectNamedPipe(m_pipe_events);
        DisconnectNamedPipe(m_pipe_commands);
        CloseHandle(m_pipe_events);   m_pipe_events   = INVALID_HANDLE_VALUE;
        CloseHandle(m_pipe_commands); m_pipe_commands = INVALID_HANDLE_VALUE;
    }
}

// ─── Loop de leitura de eventos ───────────────────────────────────────────────

void IPCServer::ReadLoop() {
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

    while (m_running.load()) {
        EventHeader hdr = {};
        DWORD bytes_read = 0;
        ResetEvent(ov.hEvent);

        BOOL ok  = ReadFile(m_pipe_events, &hdr, sizeof(hdr), nullptr, &ov);
        DWORD err = GetLastError();

        if (!ok && err == ERROR_IO_PENDING) {
            DWORD w = WaitForSingleObject(ov.hEvent, 500);
            if (w == WAIT_TIMEOUT) continue;
            if (!GetOverlappedResult(m_pipe_events, &ov, &bytes_read, FALSE)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) break;
                continue;
            }
        } else if (!ok) {
            if (err == ERROR_BROKEN_PIPE) break;
            continue;
        }

        DispatchEvent(hdr);
    }

    CloseHandle(ov.hEvent);
}

// ─── Despacha um evento recebido ──────────────────────────────────────────────

void IPCServer::DispatchEvent(const EventHeader& hdr) {
    DWORD br = 0;
    switch (hdr.type) {

        case EventType::TextureCreated: {
            TextureCreatedEvent evt = {};
            ReadFile(m_pipe_events, &evt, sizeof(evt), &br, nullptr);
            if (br == sizeof(evt) && OnTextureCreated)
                OnTextureCreated(evt);
            break;
        }

        case EventType::TextureReleased: {
            TextureReleasedEvent evt = {};
            ReadFile(m_pipe_events, &evt, sizeof(evt), &br, nullptr);
            if (br == sizeof(evt) && OnTextureReleased)
                OnTextureReleased(evt);
            break;
        }

        case EventType::VRAMSnapshot: {
            VRAMSnapshotEvent evt = {};
            ReadFile(m_pipe_events, &evt, sizeof(evt), &br, nullptr);
            if (br == sizeof(evt) && OnVRAMSnapshot)
                OnVRAMSnapshot(evt);
            break;
        }

        case EventType::FrameEnd:
            // Frame end — sem payload, ignora silenciosamente
            break;

        default:
            // Descarta payload desconhecido
            if (hdr.payload_size > 0 && hdr.payload_size < 65536) {
                std::vector<char> discard(hdr.payload_size);
                ReadFile(m_pipe_events, discard.data(), hdr.payload_size, &br, nullptr);
            }
            Log("[IPC] Evento desconhecido: type=%u size=%u",
                (uint32_t)hdr.type, hdr.payload_size);
            break;
    }
}

// ─── Envio de comandos ────────────────────────────────────────────────────────

bool IPCServer::SendCompressCommand(uint64_t texture_id,
                                    IPCCompressionLevel level,
                                    uint32_t target_w, uint32_t target_h)
{
    if (m_pipe_commands == INVALID_HANDLE_VALUE) return false;
    CommandHeader hdr = { CommandType::CompressTexture, sizeof(CompressTextureCommand) };
    CompressTextureCommand cmd = { texture_id, level, target_w, target_h };
    WaitForSingleObject(m_cmd_mutex, INFINITE);
    DWORD w = 0;
    bool ok = WriteFile(m_pipe_commands, &hdr, sizeof(hdr), &w, nullptr)
           && WriteFile(m_pipe_commands, &cmd, sizeof(cmd), &w, nullptr);
    ReleaseMutex(m_cmd_mutex);
    return ok;
}

bool IPCServer::SendPing() {
    if (m_pipe_commands == INVALID_HANDLE_VALUE) return false;
    CommandHeader hdr = { CommandType::Ping, 0 };
    WaitForSingleObject(m_cmd_mutex, INFINITE);
    DWORD w = 0;
    bool ok = WriteFile(m_pipe_commands, &hdr, sizeof(hdr), &w, nullptr);
    ReleaseMutex(m_cmd_mutex);
    return ok;
}

// ─── Log ──────────────────────────────────────────────────────────────────────

void IPCServer::Log(const char* fmt, ...) const {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char buf[512];
    va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printf("[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
}