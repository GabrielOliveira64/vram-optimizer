#pragma once

/*
 * IPCServer.h
 * ─────────────────────────────────────────────────────────────────────────────
 * Lado servidor do Named Pipe — roda dentro do Orchestrator.
 * Recebe eventos do hook DX11 e envia comandos de compressão de volta.
 *
 * Uso:
 *   IPCServer server;
 *   server.OnTextureCreated  = [&](const TextureCreatedEvent& e)  { ... };
 *   server.OnTextureReleased = [&](const TextureReleasedEvent& e) { ... };
 *   server.Start();   // inicia threads de pipe em background
 *   ...
 *   server.SendCompressCommand(texture_id, IPCCompressionLevel::BC1);
 *   server.Stop();
 */

#include "IPCProtocol.h"
#include <functional>
#include <atomic>
#include <thread>
#include <windows.h>

class IPCServer {
public:
    // Callbacks disparados quando o hook envia eventos
    std::function<void(const TextureCreatedEvent&)>  OnTextureCreated;
    std::function<void(const TextureReleasedEvent&)> OnTextureReleased;
    std::function<void(const VRAMSnapshotEvent&)>    OnVRAMSnapshot;

    IPCServer()  = default;
    ~IPCServer() { Stop(); }

    void Start();
    void Stop();
    bool IsRunning() const { return m_running.load(); }

    // Envia comando de compressão ao hook
    bool SendCompressCommand(uint64_t texture_id,
                             IPCCompressionLevel level,
                             uint32_t target_w = 0,
                             uint32_t target_h = 0);

    bool SendPing();

    IPCServer(const IPCServer&)            = delete;
    IPCServer& operator=(const IPCServer&) = delete;

private:
    void EventListenerLoop();   // Lê eventos vindos do hook
    void Log(const char* fmt, ...) const;

    std::atomic<bool> m_running{ false };
    std::thread       m_event_thread;

    HANDLE m_pipe_events   = INVALID_HANDLE_VALUE;  // Servidor escuta eventos
    HANDLE m_pipe_commands = INVALID_HANDLE_VALUE;  // Servidor envia comandos
    HANDLE m_cmd_mutex     = nullptr;
};
