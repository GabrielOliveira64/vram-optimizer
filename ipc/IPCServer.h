#pragma once

/*
 * IPCServer.h — v2
 * Loop de reconexão automática quando o hook reinjetar.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "IPCProtocol.h"
#include <functional>
#include <atomic>
#include <thread>
#include <chrono>

class IPCServer {
public:
    std::function<void(const TextureCreatedEvent&)>  OnTextureCreated;
    std::function<void(const TextureReleasedEvent&)> OnTextureReleased;
    std::function<void(const VRAMSnapshotEvent&)>    OnVRAMSnapshot;

    IPCServer()  = default;
    ~IPCServer() { Stop(); }

    void Start();
    void Stop();
    bool IsRunning() const { return m_running.load(); }

    bool SendCompressCommand(uint64_t texture_id,
                             IPCCompressionLevel level,
                             uint32_t target_w = 0,
                             uint32_t target_h = 0);
    bool SendPing();

    IPCServer(const IPCServer&)            = delete;
    IPCServer& operator=(const IPCServer&) = delete;

private:
    void EventListenerLoop();
    void ReadLoop();
    void DispatchEvent(const EventHeader& hdr);
    void Log(const char* fmt, ...) const;

    std::atomic<bool> m_running{ false };
    std::thread       m_event_thread;

    HANDLE m_pipe_events   = INVALID_HANDLE_VALUE;
    HANDLE m_pipe_commands = INVALID_HANDLE_VALUE;
    HANDLE m_cmd_mutex     = nullptr;
};