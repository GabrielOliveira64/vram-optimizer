/*
 * IPCStatusServer.cpp
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "IPCStatusServer.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <chrono>

// Nomes dos pipes para a UI
static constexpr char PIPE_STATUS_NAME[] = "\\\\.\\pipe\\vram_opt_status";
static constexpr char PIPE_UI_CMD_NAME[] = "\\\\.\\pipe\\vram_opt_ui_cmd";

// ─────────────────────────────────────────────────────────────────────────────
// Start / Stop
// ─────────────────────────────────────────────────────────────────────────────

void IPCStatusServer::Start() {
    if (m_running.exchange(true)) return;

    InitializeCriticalSection(&m_snap_cs);
    m_write_mutex = CreateMutexA(nullptr, FALSE, nullptr);

    // Cria pipe de status (escrita para a UI)
    m_pipe_status = CreateNamedPipeA(
        PIPE_STATUS_NAME,
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, 8192, 8192, 0, nullptr);

    // Cria pipe de comandos (leitura da UI)
    m_pipe_ui_cmd = CreateNamedPipeA(
        PIPE_UI_CMD_NAME,
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, 4096, 4096, 0, nullptr);

    if (m_pipe_status == INVALID_HANDLE_VALUE ||
        m_pipe_ui_cmd == INVALID_HANDLE_VALUE)
    {
        Log("[StatusServer] ERRO ao criar pipes para UI (%lu).", GetLastError());
        return;
    }

    Log("[StatusServer] Pipes criados. Aguardando conexao da UI...");

    m_status_thread = std::thread(&IPCStatusServer::StatusLoop, this);
    m_cmd_thread    = std::thread(&IPCStatusServer::CommandLoop, this);
}

void IPCStatusServer::Stop() {
    if (!m_running.exchange(false)) return;

    m_ui_connected = false;

    if (m_pipe_status != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(m_pipe_status);
        CloseHandle(m_pipe_status);
        m_pipe_status = INVALID_HANDLE_VALUE;
    }
    if (m_pipe_ui_cmd != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(m_pipe_ui_cmd);
        CloseHandle(m_pipe_ui_cmd);
        m_pipe_ui_cmd = INVALID_HANDLE_VALUE;
    }
    if (m_status_thread.joinable()) m_status_thread.join();
    if (m_cmd_thread.joinable())    m_cmd_thread.join();
    if (m_write_mutex) {
        CloseHandle(m_write_mutex);
        m_write_mutex = nullptr;
    }
    DeleteCriticalSection(&m_snap_cs);
    Log("[StatusServer] Parado.");
}

// ─────────────────────────────────────────────────────────────────────────────
// SendSnapshot — atualiza snapshot e envia para a UI se conectada
// ─────────────────────────────────────────────────────────────────────────────

void IPCStatusServer::SendSnapshot(
    float vram_pct, float peak_pct,
    uint64_t used_bytes, uint64_t total_bytes, uint64_t free_bytes,
    const char* backend,
    bool hook_connected,
    uint64_t saved_bytes,
    int total_tex, int comp_tex, int actions_fired)
{
    // Atualiza snapshot local
    EnterCriticalSection(&m_snap_cs);
    m_snapshot.vram_pct    = vram_pct;
    m_snapshot.peak_pct    = peak_pct;
    m_snapshot.used_bytes  = used_bytes;
    m_snapshot.total_bytes = total_bytes;
    m_snapshot.free_bytes  = free_bytes;
    m_snapshot.hook_conn   = hook_connected;
    m_snapshot.saved_bytes = saved_bytes;
    m_snapshot.total_tex   = total_tex;
    m_snapshot.comp_tex    = comp_tex;
    m_snapshot.actions     = actions_fired;
    strncpy_s(m_snapshot.backend, backend ? backend : "Unknown", 31);
    LeaveCriticalSection(&m_snap_cs);

    // Só envia se a UI estiver conectada
    if (!m_ui_connected.load()) return;
    if (m_pipe_status == INVALID_HANDLE_VALUE) return;

    // Monta JSON
    char json[512];
    snprintf(json, sizeof(json),
        "{\"pct\":%.1f,\"peak\":%.1f,"
        "\"used\":%llu,\"total\":%llu,\"free\":%llu,"
        "\"backend\":\"%s\",\"hook\":%d,"
        "\"saved\":%llu,\"total_tex\":%d,\"comp_tex\":%d,\"actions\":%d}\n",
        vram_pct, peak_pct,
        (unsigned long long)used_bytes,
        (unsigned long long)total_bytes,
        (unsigned long long)free_bytes,
        m_snapshot.backend,
        hook_connected ? 1 : 0,
        (unsigned long long)saved_bytes,
        total_tex, comp_tex, actions_fired);

    // Envia com mutex para evitar colisão entre threads
    WaitForSingleObject(m_write_mutex, INFINITE);
    DWORD written = 0;
    BOOL  ok = WriteFile(m_pipe_status, json, (DWORD)strlen(json),
                         &written, nullptr);
    ReleaseMutex(m_write_mutex);

    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
            m_ui_connected = false;
            Log("[StatusServer] UI desconectou.");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// StatusLoop — aceita conexão da UI e fica disponível para escrita
// ─────────────────────────────────────────────────────────────────────────────

void IPCStatusServer::StatusLoop() {
    while (m_running.load()) {
        Log("[StatusServer] Aguardando UI conectar no pipe de status...");

        OVERLAPPED ov = {};
        ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

        ConnectNamedPipe(m_pipe_status, &ov);
        DWORD wait = WaitForSingleObject(ov.hEvent, 3000);
        CloseHandle(ov.hEvent);

        if (!m_running.load()) break;

        if (wait == WAIT_OBJECT_0) {
            m_ui_connected = true;
            Log("[StatusServer] UI conectada ao pipe de status.");

            // Aguarda desconexão (o SendSnapshot detecta o broken pipe)
            while (m_running.load() && m_ui_connected.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(200));

            DisconnectNamedPipe(m_pipe_status);
            Log("[StatusServer] Pipe de status desconectado. Aguardando reconexao...");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CommandLoop — lê comandos e configs enviados pela UI
// ─────────────────────────────────────────────────────────────────────────────

void IPCStatusServer::CommandLoop() {
    while (m_running.load()) {
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

        ConnectNamedPipe(m_pipe_ui_cmd, &ov);
        DWORD wait = WaitForSingleObject(ov.hEvent, 3000);
        CloseHandle(ov.hEvent);

        if (!m_running.load()) break;
        if (wait != WAIT_OBJECT_0) continue;

        Log("[StatusServer] UI conectada ao pipe de comandos.");

        // Loop de leitura de linhas
        char   buf[1024];
        DWORD  bytes_read = 0;
        OVERLAPPED ov_read = {};
        ov_read.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

        while (m_running.load()) {
            memset(buf, 0, sizeof(buf));
            ResetEvent(ov_read.hEvent);

            BOOL ok = ReadFile(m_pipe_ui_cmd, buf, sizeof(buf) - 1,
                               nullptr, &ov_read);
            DWORD err = GetLastError();

            if (!ok && err == ERROR_IO_PENDING) {
                DWORD w = WaitForSingleObject(ov_read.hEvent, 500);
                if (w == WAIT_TIMEOUT) continue;
                GetOverlappedResult(m_pipe_ui_cmd, &ov_read, &bytes_read, FALSE);
            } else if (!ok) {
                if (err == ERROR_BROKEN_PIPE) break;
                continue;
            }

            if (bytes_read > 0) {
                buf[bytes_read] = '\0';
                // Processa cada linha
                char* line = strtok(buf, "\n\r");
                while (line) {
                    if (line[0] != '\0')
                        ParseCommand(line);
                    line = strtok(nullptr, "\n\r");
                }
            }
        }

        CloseHandle(ov_read.hEvent);
        DisconnectNamedPipe(m_pipe_ui_cmd);
        Log("[StatusServer] Pipe de comandos desconectado.");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ParseCommand — interpreta linhas recebidas da UI
// ─────────────────────────────────────────────────────────────────────────────

void IPCStatusServer::ParseCommand(const char* line) {
    // Formato CONFIG: "CONFIG chave=valor"
    if (strncmp(line, "CONFIG ", 7) == 0) {
        const char* kv = line + 7;

        static UIConfig pending_cfg;
        static int      received = 0;

        auto Parse = [](const char* src, const char* key) -> float {
            size_t klen = strlen(key);
            if (strncmp(src, key, klen) == 0 && src[klen] == '=')
                return (float)atof(src + klen + 1);
            return -1.f;
        };

        float v;
        if ((v = Parse(kv, "warn"))        >= 0) { pending_cfg.threshold_warning   = v; received++; }
        if ((v = Parse(kv, "low"))         >= 0) { pending_cfg.threshold_low        = v; received++; }
        if ((v = Parse(kv, "med"))         >= 0) { pending_cfg.threshold_medium     = v; received++; }
        if ((v = Parse(kv, "emerg"))       >= 0) { pending_cfg.threshold_emergency  = v; received++; }
        if ((v = Parse(kv, "max_tex"))     >= 0) { pending_cfg.max_compress_per_tick= (int)v; received++; }
        if ((v = Parse(kv, "interval"))    >= 0) { pending_cfg.monitor_interval_ms  = (int)v; received++; }
        if ((v = Parse(kv, "format"))      >= 0) { pending_cfg.format_preference    = (int)v; received++; }
        if ((v = Parse(kv, "half_res"))    >= 0) { pending_cfg.allow_half_res       = v > 0; received++; }
        if ((v = Parse(kv, "quarter_res")) >= 0) { pending_cfg.allow_quarter_res    = v > 0; received++; }
        if ((v = Parse(kv, "skip_critical"))>=0) { pending_cfg.skip_critical        = v > 0; received++; }

        // Quando recebe todos os 10 campos, dispara o callback
        if (received >= 10) {
            Log("[StatusServer] Config recebida da UI.");
            if (OnConfigReceived) OnConfigReceived(pending_cfg);
            pending_cfg = {};
            received    = 0;
        }
        return;
    }

    // Comandos simples
    if (strcmp(line, "restore_all") == 0) {
        Log("[StatusServer] Comando: restore_all");
        if (OnCommandReceived) OnCommandReceived("restore_all");
    } else if (strcmp(line, "ping") == 0) {
        Log("[StatusServer] Ping da UI.");
    } else {
        Log("[StatusServer] Comando desconhecido: %s", line);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Log
// ─────────────────────────────────────────────────────────────────────────────

void IPCStatusServer::Log(const char* fmt, ...) const {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printf("[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
}
