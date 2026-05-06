#pragma once

/*
 * IPCStatusServer.h
 * ─────────────────────────────────────────────────────────────────────────────
 * Servidor de pipe dedicado à interface C#.
 * Enquanto o IPCServer (vram_opt_events/cmds) fala com o hook DX11,
 * este servidor fala com a UI:
 *
 *   Pipe de status  (orquestrador → UI): "vram_opt_status"
 *     → envia snapshot JSON a cada tick do orquestrador
 *
 *   Pipe de comandos UI (UI → orquestrador): "vram_opt_ui_cmd"
 *     → recebe CONFIG e comandos (restore_all, ping...)
 *
 * Formato do snapshot (uma linha JSON por tick):
 *   {"pct":42.5,"peak":75.0,"used":1073741824,"total":2147483648,
 *    "free":1073741824,"backend":"DXGI 1.4","hook":1,
 *    "saved":536870912,"total_tex":11,"comp_tex":3,"actions":7}
 *
 * Formato de comandos da UI (linhas de texto simples):
 *   CONFIG warn=70
 *   CONFIG low=80
 *   restore_all
 *   ping
 */

#include <windows.h>
#include <cstdint>
#include <atomic>
#include <thread>
#include <functional>

// Callback chamado quando a UI envia um comando de configuração
struct UIConfig {
    float threshold_warning   = 70.f;
    float threshold_low       = 80.f;
    float threshold_medium    = 90.f;
    float threshold_emergency = 95.f;
    int   max_compress_per_tick = 4;
    int   monitor_interval_ms   = 500;
    int   format_preference     = 0;   // 0=BC1, 1=BC3, 2=BC7
    bool  allow_half_res        = true;
    bool  allow_quarter_res     = true;
    bool  skip_critical         = true;
    bool  log_to_file           = true;
};

using UIConfigCallback   = std::function<void(const UIConfig&)>;
using UICommandCallback  = std::function<void(const char* cmd)>;

// ─────────────────────────────────────────────────────────────────────────────

class IPCStatusServer {
public:
    IPCStatusServer()  = default;
    ~IPCStatusServer() { Stop(); }

    // Callbacks disparados quando a UI envia dados
    UIConfigCallback  OnConfigReceived;
    UICommandCallback OnCommandReceived;

    void Start();
    void Stop();
    bool IsUIConnected() const { return m_ui_connected.load(); }

    // Envia snapshot JSON para a UI (chamado pelo Orchestrator a cada tick)
    void SendSnapshot(
        float    vram_pct,
        float    peak_pct,
        uint64_t used_bytes,
        uint64_t total_bytes,
        uint64_t free_bytes,
        const char* backend,
        bool     hook_connected,
        uint64_t saved_bytes,
        int      total_tex,
        int      comp_tex,
        int      actions_fired);

    IPCStatusServer(const IPCStatusServer&)            = delete;
    IPCStatusServer& operator=(const IPCStatusServer&) = delete;

private:
    void StatusLoop();    // Aceita conexão e envia snapshots
    void CommandLoop();   // Aceita conexão e lê comandos

    void ParseCommand(const char* line);
    void Log(const char* fmt, ...) const;

    std::atomic<bool> m_running     { false };
    std::atomic<bool> m_ui_connected{ false };

    HANDLE m_pipe_status  = INVALID_HANDLE_VALUE;
    HANDLE m_pipe_ui_cmd  = INVALID_HANDLE_VALUE;
    HANDLE m_write_mutex  = nullptr;

    std::thread m_status_thread;
    std::thread m_cmd_thread;

    // Snapshot mais recente para envio
    struct Snapshot {
        float    vram_pct    = 0;
        float    peak_pct    = 0;
        uint64_t used_bytes  = 0;
        uint64_t total_bytes = 0;
        uint64_t free_bytes  = 0;
        char     backend[32] = {};
        bool     hook_conn   = false;
        uint64_t saved_bytes = 0;
        int      total_tex   = 0;
        int      comp_tex    = 0;
        int      actions     = 0;
    };

    Snapshot      m_snapshot;
    CRITICAL_SECTION m_snap_cs;
};
