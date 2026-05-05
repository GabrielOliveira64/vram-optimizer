/*
 * Orchestrator.cpp
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "Orchestrator.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <chrono>

// ─────────────────────────────────────────────────────────────────────────────
// Utilitários
// ─────────────────────────────────────────────────────────────────────────────

static double NowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static const char* CompressionLevelName(CompressionLevel lvl) {
    switch (lvl) {
        case CompressionLevel::BC1:        return "BC1";
        case CompressionLevel::BC3:        return "BC3";
        case CompressionLevel::BC7:        return "BC7";
        case CompressionLevel::HalfRes:    return "HalfRes";
        case CompressionLevel::QuarterRes: return "QuarterRes";
        default:                           return "None";
    }
}

// Estimativa de bytes após compressão (baseado em razões típicas)
static uint64_t EstimateCompressedSize(const TextureEntry& tex, CompressionLevel lvl) {
    switch (lvl) {
        case CompressionLevel::BC1:
            return (tex.width / 4) * (tex.height / 4) * 8;
        case CompressionLevel::BC3:
        case CompressionLevel::BC7:
            return (tex.width / 4) * (tex.height / 4) * 16;
        case CompressionLevel::HalfRes:
            return tex.size_bytes / 4;
        case CompressionLevel::QuarterRes:
            return tex.size_bytes / 16;
        default:
            return tex.size_bytes;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// OrchestratorAction
// ─────────────────────────────────────────────────────────────────────────────

bool OrchestratorAction::IsReady(float current_percent, double now_sec) const {
    bool triggered = current_percent >= trigger_percent;
    bool cooled    = (now_sec - last_run_time) >= (double)cooldown_sec;
    return triggered && cooled;
}

// ─────────────────────────────────────────────────────────────────────────────
// Construtor / Destrutor
// ─────────────────────────────────────────────────────────────────────────────

Orchestrator::Orchestrator(const OrchestratorConfig& cfg)
    : m_cfg(cfg)
    , m_monitor(cfg.threshold_warning)
{
    if (cfg.log_to_file && cfg.enable_logging) {
        fopen_s(&m_log_file, cfg.log_path, "a");
    }
    RegisterDefaultActions();
    Log("=== Orchestrator inicializado | Backend: %s ===", m_monitor.ActiveBackend());
}

Orchestrator::~Orchestrator() {
    Stop();
    if (m_log_file) {
        fclose(m_log_file);
        m_log_file = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Ações padrão
// ─────────────────────────────────────────────────────────────────────────────

void Orchestrator::RegisterDefaultActions() {
    // ── Aviso ────────────────────────────────────────────────────────────────
    {
        OrchestratorAction a;
        a.name            = "LogWarning";
        a.trigger_percent = m_cfg.threshold_warning;
        a.cooldown_sec    = 10.f;
        a.execute = [this](const VRAMInfo& info, std::vector<TextureEntry>& textures) {
            return ActionLogWarning(info, textures);
        };
        m_actions.push_back(std::move(a));
    }

    // ── Comprimir texturas de baixa prioridade ────────────────────────────
    {
        OrchestratorAction a;
        a.name            = "CompressLowPriority";
        a.trigger_percent = m_cfg.threshold_low;
        a.cooldown_sec    = 5.f;
        a.execute = [this](const VRAMInfo& info, std::vector<TextureEntry>& textures) {
            return ActionCompressLow(info, textures);
        };
        m_actions.push_back(std::move(a));
    }

    // ── Comprimir texturas de prioridade média ────────────────────────────
    {
        OrchestratorAction a;
        a.name            = "CompressMediumPriority";
        a.trigger_percent = m_cfg.threshold_medium;
        a.cooldown_sec    = 3.f;
        a.execute = [this](const VRAMInfo& info, std::vector<TextureEntry>& textures) {
            return ActionCompressMedium(info, textures);
        };
        m_actions.push_back(std::move(a));
    }

    // ── Redução emergencial ───────────────────────────────────────────────
    {
        OrchestratorAction a;
        a.name            = "EmergencyReduction";
        a.trigger_percent = m_cfg.threshold_emergency;
        a.cooldown_sec    = 2.f;
        a.execute = [this](const VRAMInfo& info, std::vector<TextureEntry>& textures) {
            return ActionEmergency(info, textures);
        };
        m_actions.push_back(std::move(a));
    }

    // Ordena por trigger_percent crescente
    std::sort(m_actions.begin(), m_actions.end(),
        [](const OrchestratorAction& a, const OrchestratorAction& b) {
            return a.trigger_percent < b.trigger_percent;
        });
}

// ─────────────────────────────────────────────────────────────────────────────
// Implementação das ações padrão
// ─────────────────────────────────────────────────────────────────────────────

uint64_t Orchestrator::ActionLogWarning(const VRAMInfo& info, std::vector<TextureEntry>&) {
    Log("[!] Aviso de VRAM: %.1f%% | %.0f / %.0f MB usados",
        info.percent,
        info.used_bytes  / (1024.0 * 1024.0),
        info.total_bytes / (1024.0 * 1024.0));
    return 0;
}

uint64_t Orchestrator::ActionCompressLow(const VRAMInfo& info, std::vector<TextureEntry>& textures) {
    Log("[Orcuestrador] CompressLowPriority acionado (%.1f%%)", info.percent);

    // Candidatos: não comprimidos, prioridade baixa ou descartavel, ordenados por tamanho
    std::vector<TextureEntry*> candidates;
    for (auto& tex : textures) {
        if (tex.compression != CompressionLevel::None) continue;
        if (tex.priority < TexturePriority::Low)       continue;
        candidates.push_back(&tex);
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const TextureEntry* a, const TextureEntry* b) {
            return a->size_bytes > b->size_bytes; // maior primeiro
        });

    uint64_t total_saved = 0;
    uint32_t count = 0;
    for (auto* tex : candidates) {
        if (count++ >= m_cfg.max_compress_per_tick) break;
        total_saved += CompressTexture(*tex, CompressionLevel::BC1);
    }

    Log("  → %u texturas comprimidas | %.1f MB liberados",
        count, total_saved / (1024.0 * 1024.0));
    return total_saved;
}

uint64_t Orchestrator::ActionCompressMedium(const VRAMInfo& info, std::vector<TextureEntry>& textures) {
    Log("[Orquestrador] CompressMediumPriority acionado (%.1f%%)", info.percent);

    std::vector<TextureEntry*> candidates;
    for (auto& tex : textures) {
        if (tex.compression != CompressionLevel::None) continue;
        if (tex.priority < TexturePriority::Medium)    continue;
        candidates.push_back(&tex);
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const TextureEntry* a, const TextureEntry* b) {
            return a->size_bytes > b->size_bytes;
        });

    uint64_t total_saved = 0;
    uint32_t count = 0;
    for (auto* tex : candidates) {
        if (count++ >= m_cfg.max_compress_per_tick * 2) break;
        // Usa HalfRes para texturas de média prioridade (mais agressivo que BC1)
        CompressionLevel lvl = (tex->priority == TexturePriority::Medium)
                               ? CompressionLevel::HalfRes
                               : CompressionLevel::BC3;
        total_saved += CompressTexture(*tex, lvl);
    }

    Log("  → %u texturas comprimidas | %.1f MB liberados",
        count, total_saved / (1024.0 * 1024.0));
    return total_saved;
}

uint64_t Orchestrator::ActionEmergency(const VRAMInfo& info, std::vector<TextureEntry>& textures) {
    Log("[EMERGENCIA] VRAM em %.1f%% — compressão total ativada!", info.percent);

    uint64_t total_saved = 0;
    uint32_t count = 0;
    for (auto& tex : textures) {
        if (tex.compression == CompressionLevel::QuarterRes) continue;
        // Preserva apenas prioridade critica
        if (tex.priority == TexturePriority::Critical)       continue;
        total_saved += CompressTexture(tex, CompressionLevel::QuarterRes);
        count++;
    }

    Log("  → [EMERGENCIA] %u texturas reduzidas | %.1f MB liberados",
        count, total_saved / (1024.0 * 1024.0));
    return total_saved;
}

// ─────────────────────────────────────────────────────────────────────────────
// Compressão de textura individual
// ─────────────────────────────────────────────────────────────────────────────

uint64_t Orchestrator::CompressTexture(TextureEntry& tex, CompressionLevel level) {
    uint64_t compressed_size = EstimateCompressedSize(tex, level);
    uint64_t saved = (compressed_size < tex.size_bytes)
                     ? tex.size_bytes - compressed_size : 0;

    tex.compression = level;
    tex.saved_bytes = saved;

    Log("  Textura [%s | %ux%u | %.1f MB] → %s | Economia: %.1f MB",
        tex.name.c_str(),
        tex.width, tex.height,
        tex.size_bytes / (1024.0 * 1024.0),
        CompressionLevelName(level),
        saved / (1024.0 * 1024.0));

    /*
     * TODO — Integração real com o hook DX11 via Named Pipe:
     *
     *   struct CompressCmd { uint64_t texture_id; int level; };
     *   CompressCmd cmd = { tex.id, (int)level };
     *   HANDLE pipe = CreateFileA("\\\\.\\pipe\\vram_optimizer_cmd", ...);
     *   WriteFile(pipe, &cmd, sizeof(cmd), &written, NULL);
     *   CloseHandle(pipe);
     */

    return saved;
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop de monitoramento
// ─────────────────────────────────────────────────────────────────────────────

void Orchestrator::MonitorLoop() {
    m_monitor.Watch([this](const VRAMInfo& info) {
        ProcessVRAM(info);
        if (!m_running.load()) {
            m_monitor.Stop();
        }
    }, static_cast<uint32_t>(m_cfg.monitor_interval_ms));
}

void Orchestrator::ProcessVRAM(const VRAMInfo& info) {
    // Atualiza estado compartilhado
    {
        std::lock_guard<std::mutex> lock(m_stats_mutex);
        m_last_info                  = info;
        m_stats.last_vram_percent    = info.percent;
        if (info.percent > m_stats.peak_vram_percent)
            m_stats.peak_vram_percent = info.percent;
    }

    // Dispara ações elegíveis
    double now = NowSec();
    uint64_t total_freed = 0;

    {
        std::lock_guard<std::mutex> lock(m_tex_mutex);
        for (auto& action : m_actions) {
            if (action.IsReady(info.percent, now)) {
                uint64_t freed = action.execute(info, m_textures);
                total_freed += freed;
                action.last_run_time = now;

                std::lock_guard<std::mutex> slock(m_stats_mutex);
                m_stats.actions_fired++;
                m_stats.total_saved_bytes += freed;
            }
        }

        // Atualiza contagens
        std::lock_guard<std::mutex> slock(m_stats_mutex);
        m_stats.total_textures      = m_textures.size();
        m_stats.compressed_textures = 0;
        for (const auto& t : m_textures)
            if (t.compression != CompressionLevel::None)
                m_stats.compressed_textures++;
    }

    // Dispara callback externo (para a futura interface C#)
    if (OnUpdate) {
        OrchestratorStats stats = GetStats();
        OnUpdate(info, stats);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// API pública
// ─────────────────────────────────────────────────────────────────────────────

void Orchestrator::Start() {
    if (m_running.exchange(true)) return; // já estava rodando
    Log("Orquestrador iniciado.");
    m_thread = std::thread(&Orchestrator::MonitorLoop, this);
}

void Orchestrator::Stop() {
    if (!m_running.exchange(false)) return;
    m_monitor.Stop();
    if (m_thread.joinable()) m_thread.join();
    Log("Orquestrador parado.");
}

bool Orchestrator::IsRunning() const {
    return m_running.load();
}

void Orchestrator::RegisterTexture(const TextureEntry& entry) {
    std::lock_guard<std::mutex> lock(m_tex_mutex);
    // Remove entrada anterior com mesmo id se existir
    m_textures.erase(
        std::remove_if(m_textures.begin(), m_textures.end(),
            [&](const TextureEntry& t) { return t.id == entry.id; }),
        m_textures.end());
    m_textures.push_back(entry);
}

void Orchestrator::UnregisterTexture(uint64_t id) {
    std::lock_guard<std::mutex> lock(m_tex_mutex);
    m_textures.erase(
        std::remove_if(m_textures.begin(), m_textures.end(),
            [id](const TextureEntry& t) { return t.id == id; }),
        m_textures.end());
}

void Orchestrator::ClearTextures() {
    std::lock_guard<std::mutex> lock(m_tex_mutex);
    m_textures.clear();
}

void Orchestrator::AddAction(OrchestratorAction action) {
    m_actions.push_back(std::move(action));
    std::sort(m_actions.begin(), m_actions.end(),
        [](const OrchestratorAction& a, const OrchestratorAction& b) {
            return a.trigger_percent < b.trigger_percent;
        });
}

OrchestratorStats Orchestrator::GetStats() const {
    std::lock_guard<std::mutex> lock(m_stats_mutex);
    return m_stats;
}

VRAMInfo Orchestrator::GetLastVRAMInfo() const {
    std::lock_guard<std::mutex> lock(m_stats_mutex);
    return m_last_info;
}

// ─────────────────────────────────────────────────────────────────────────────
// Log interno
// ─────────────────────────────────────────────────────────────────────────────

void Orchestrator::Log(const char* fmt, ...) const {
    if (!m_cfg.enable_logging) return;

    // Timestamp
    time_t t = time(nullptr);
    struct tm tm_info = {};
    localtime_s(&tm_info, &t);
    char ts[20];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm_info);

    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    printf("[%s] %s\n", ts, buf);

    if (m_log_file) {
        fprintf(m_log_file, "[%s] %s\n", ts, buf);
        fflush(m_log_file);
    }
}
