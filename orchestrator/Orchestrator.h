#pragma once

/*
 * Orchestrator.h
 * ─────────────────────────────────────────────────────────────────────────────
 * Conecta o VRAMMonitor ao sistema de compressão de texturas.
 *
 * CORREÇÃO: include corrigido para "monitor/VRAMMonitor.h" (caminho relativo
 * à raiz do projeto, conforme target_include_directories no CMakeLists.txt).
 */

// windows.h primeiro para evitar conflitos com tipos Win32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "monitor/VRAMMonitor.h"   // ← corrigido (era "VRAMMonitor.h")
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>

// ─── Perfil de textura ────────────────────────────────────────────────────────

enum class TexturePriority : int {
    Critical   = 1,
    High       = 2,
    Medium     = 3,
    Low        = 4,
    Disposable = 5,
};

enum class CompressionLevel : int {
    None       = 0,
    BC1        = 1,
    BC3        = 2,
    BC7        = 3,
    HalfRes    = 4,
    QuarterRes = 5,
};

struct TextureEntry {
    uint64_t         id           = 0;
    std::string      name;
    uint64_t         size_bytes   = 0;
    uint32_t         width        = 0;
    uint32_t         height       = 0;
    TexturePriority  priority     = TexturePriority::Medium;
    CompressionLevel compression  = CompressionLevel::None;
    uint64_t         saved_bytes  = 0;
};

// ─── Ação de otimização ───────────────────────────────────────────────────────

struct OrchestratorAction {
    std::string  name;
    float        trigger_percent = 80.f;
    float        cooldown_sec    = 5.f;
    std::function<uint64_t(const VRAMInfo&, std::vector<TextureEntry>&)> execute;
    double last_run_time = 0.0;
    bool   IsReady(float current_percent, double now_sec) const;
};

// ─── Configuração ─────────────────────────────────────────────────────────────

struct OrchestratorConfig {
    float    monitor_interval_ms     = 500.f;
    float    threshold_warning       = 70.f;
    float    threshold_low           = 80.f;
    float    threshold_medium        = 90.f;
    float    threshold_emergency     = 95.f;
    uint32_t max_compress_per_tick   = 4;
    bool     enable_logging          = true;
    bool     log_to_file             = true;
    char     log_path[260]           = "vram_optimizer.log";
};

// ─── Estatísticas ─────────────────────────────────────────────────────────────

struct OrchestratorStats {
    uint64_t total_textures        = 0;
    uint64_t compressed_textures   = 0;
    uint64_t total_saved_bytes     = 0;
    uint32_t actions_fired         = 0;
    float    last_vram_percent     = 0.f;
    float    peak_vram_percent     = 0.f;
};

// ─── Orquestrador principal ───────────────────────────────────────────────────

class Orchestrator {
public:
    explicit Orchestrator(const OrchestratorConfig& cfg = {});
    ~Orchestrator();

    void Start();
    void Stop();
    bool IsRunning() const;

    void RegisterTexture  (const TextureEntry& entry);
    void UnregisterTexture(uint64_t id);
    void ClearTextures();

    void AddAction(OrchestratorAction action);

    OrchestratorStats GetStats()       const;
    VRAMInfo          GetLastVRAMInfo() const;

    std::function<void(const VRAMInfo&, const OrchestratorStats&)> OnUpdate;

    Orchestrator(const Orchestrator&)            = delete;
    Orchestrator& operator=(const Orchestrator&) = delete;

private:
    void MonitorLoop();
    void ProcessVRAM(const VRAMInfo& info);
    void Log(const char* fmt, ...) const;

    void RegisterDefaultActions();

    uint64_t ActionLogWarning    (const VRAMInfo&, std::vector<TextureEntry>&);
    uint64_t ActionCompressLow   (const VRAMInfo&, std::vector<TextureEntry>&);
    uint64_t ActionCompressMedium(const VRAMInfo&, std::vector<TextureEntry>&);
    uint64_t ActionEmergency     (const VRAMInfo&, std::vector<TextureEntry>&);

    uint64_t CompressTexture(TextureEntry& tex, CompressionLevel level);

    OrchestratorConfig              m_cfg;
    VRAMMonitor                     m_monitor;
    std::vector<TextureEntry>       m_textures;
    std::vector<OrchestratorAction> m_actions;
    OrchestratorStats               m_stats;
    VRAMInfo                        m_last_info;

    mutable std::mutex  m_tex_mutex;
    mutable std::mutex  m_stats_mutex;
    std::thread         m_thread;
    std::atomic<bool>   m_running{ false };

    FILE* m_log_file = nullptr;
};