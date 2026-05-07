/*
 * Orchestrator.cpp
 * ─────────────────────────────────────────────────────────────────────────────
 * CORREÇÃO: include atualizado para "orchestrator/Orchestrator.h"
 * (caminho relativo à raiz do projeto)
 */

#include "orchestrator/Orchestrator.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <chrono>

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

static uint64_t EstimateCompressedSize(const TextureEntry& tex, CompressionLevel lvl) {
    switch (lvl) {
        case CompressionLevel::BC1:
            return (tex.width / 4) * (tex.height / 4) * 8;
        case CompressionLevel::BC3:
        case CompressionLevel::BC7:
            return (tex.width / 4) * (tex.height / 4) * 16;
        case CompressionLevel::HalfRes:    return tex.size_bytes / 4;
        case CompressionLevel::QuarterRes: return tex.size_bytes / 16;
        default:                           return tex.size_bytes;
    }
}

// ─── OrchestratorAction ───────────────────────────────────────────────────────

bool OrchestratorAction::IsReady(float current_percent, double now_sec) const {
    return (current_percent >= trigger_percent) &&
           ((now_sec - last_run_time) >= (double)cooldown_sec);
}

// ─── Construtor / Destrutor ───────────────────────────────────────────────────

Orchestrator::Orchestrator(const OrchestratorConfig& cfg)
    : m_cfg(cfg)
    , m_monitor(cfg.threshold_warning)
{
    if (cfg.log_to_file && cfg.enable_logging)
        fopen_s(&m_log_file, cfg.log_path, "a");
    RegisterDefaultActions();
    Log("=== Orchestrator inicializado | Backend: %s ===", m_monitor.ActiveBackend());
}

Orchestrator::~Orchestrator() {
    Stop();
    if (m_log_file) { fclose(m_log_file); m_log_file = nullptr; }
}

// ─── Ações padrão ─────────────────────────────────────────────────────────────

void Orchestrator::RegisterDefaultActions() {
    auto make = [&](const char* name, float pct, float cd,
                    uint64_t(Orchestrator::*fn)(const VRAMInfo&, std::vector<TextureEntry>&))
    {
        OrchestratorAction a;
        a.name            = name;
        a.trigger_percent = pct;
        a.cooldown_sec    = cd;
        a.execute = [this, fn](const VRAMInfo& info, std::vector<TextureEntry>& tex) {
            return (this->*fn)(info, tex);
        };
        m_actions.push_back(std::move(a));
    };

    make("LogWarning",          m_cfg.threshold_warning,   10.f, &Orchestrator::ActionLogWarning);
    make("CompressLowPriority", m_cfg.threshold_low,        5.f, &Orchestrator::ActionCompressLow);
    make("CompressMedium",      m_cfg.threshold_medium,     3.f, &Orchestrator::ActionCompressMedium);
    make("EmergencyReduction",  m_cfg.threshold_emergency,  2.f, &Orchestrator::ActionEmergency);

    std::sort(m_actions.begin(), m_actions.end(),
        [](const OrchestratorAction& a, const OrchestratorAction& b) {
            return a.trigger_percent < b.trigger_percent;
        });
}

// ─── Implementação das ações ──────────────────────────────────────────────────

uint64_t Orchestrator::ActionLogWarning(const VRAMInfo& info, std::vector<TextureEntry>&) {
    Log("[!] Aviso VRAM: %.1f%% | %.0f / %.0f MB",
        info.percent,
        info.used_bytes  / (1024.0 * 1024.0),
        info.total_bytes / (1024.0 * 1024.0));
    return 0;
}

uint64_t Orchestrator::ActionCompressLow(const VRAMInfo& info, std::vector<TextureEntry>& textures) {
    Log("[Orq] CompressLow acionado (%.1f%%)", info.percent);
    std::vector<TextureEntry*> cands;
    for (auto& t : textures)
        if (t.compression == CompressionLevel::None && t.priority >= TexturePriority::Low)
            cands.push_back(&t);
    std::sort(cands.begin(), cands.end(),
        [](const TextureEntry* a, const TextureEntry* b) { return a->size_bytes > b->size_bytes; });

    uint64_t saved = 0; uint32_t n = 0;
    for (auto* t : cands) {
        if (n++ >= m_cfg.max_compress_per_tick) break;
        saved += CompressTexture(*t, CompressionLevel::BC1);
    }
    Log("  → %u comprimidas | %.1f MB liberados", n, saved / (1024.0*1024.0));
    return saved;
}

uint64_t Orchestrator::ActionCompressMedium(const VRAMInfo& info, std::vector<TextureEntry>& textures) {
    Log("[Orq] CompressMedium acionado (%.1f%%)", info.percent);
    std::vector<TextureEntry*> cands;
    for (auto& t : textures)
        if (t.compression == CompressionLevel::None && t.priority >= TexturePriority::Medium)
            cands.push_back(&t);
    std::sort(cands.begin(), cands.end(),
        [](const TextureEntry* a, const TextureEntry* b) { return a->size_bytes > b->size_bytes; });

    uint64_t saved = 0; uint32_t n = 0;
    for (auto* t : cands) {
        if (n++ >= m_cfg.max_compress_per_tick * 2) break;
        CompressionLevel lvl = (t->priority == TexturePriority::Medium)
                               ? CompressionLevel::HalfRes : CompressionLevel::BC3;
        saved += CompressTexture(*t, lvl);
    }
    Log("  → %u comprimidas | %.1f MB liberados", n, saved / (1024.0*1024.0));
    return saved;
}

uint64_t Orchestrator::ActionEmergency(const VRAMInfo& info, std::vector<TextureEntry>& textures) {
    Log("[EMERGENCIA] %.1f%% — compressao total!", info.percent);
    uint64_t saved = 0; uint32_t n = 0;
    for (auto& t : textures) {
        if (t.compression == CompressionLevel::QuarterRes) continue;
        if (t.priority    == TexturePriority::Critical)    continue;
        saved += CompressTexture(t, CompressionLevel::QuarterRes);
        n++;
    }
    Log("  → [EMERG] %u reduzidas | %.1f MB", n, saved / (1024.0*1024.0));
    return saved;
}

uint64_t Orchestrator::CompressTexture(TextureEntry& tex, CompressionLevel level) {
    uint64_t csz   = EstimateCompressedSize(tex, level);
    uint64_t saved = tex.size_bytes > csz ? tex.size_bytes - csz : 0;
    tex.compression = level;
    tex.saved_bytes = saved;
    Log("  [%s %ux%u %.1fMB] → %s (economia %.1fMB)",
        tex.name.c_str(), tex.width, tex.height,
        tex.size_bytes / (1024.0*1024.0),
        CompressionLevelName(level),
        saved / (1024.0*1024.0));
    return saved;
}

// ─── Loop de monitoramento ────────────────────────────────────────────────────

void Orchestrator::MonitorLoop() {
    m_monitor.Watch([this](const VRAMInfo& info) {
        ProcessVRAM(info);
        if (!m_running.load()) m_monitor.Stop();
    }, static_cast<uint32_t>(m_cfg.monitor_interval_ms));
}

void Orchestrator::ProcessVRAM(const VRAMInfo& info) {
    {
        std::lock_guard<std::mutex> lk(m_stats_mutex);
        m_last_info = info;
        m_stats.last_vram_percent = info.percent;
        if (info.percent > m_stats.peak_vram_percent)
            m_stats.peak_vram_percent = info.percent;
    }

    double now = NowSec();
    {
        std::lock_guard<std::mutex> lk(m_tex_mutex);
        for (auto& action : m_actions) {
            if (action.IsReady(info.percent, now)) {
                uint64_t freed = action.execute(info, m_textures);
                action.last_run_time = now;
                std::lock_guard<std::mutex> slk(m_stats_mutex);
                m_stats.actions_fired++;
                m_stats.total_saved_bytes += freed;
            }
        }
        std::lock_guard<std::mutex> slk(m_stats_mutex);
        m_stats.total_textures      = m_textures.size();
        m_stats.compressed_textures = 0;
        for (const auto& t : m_textures)
            if (t.compression != CompressionLevel::None)
                m_stats.compressed_textures++;
    }

    if (OnUpdate) { OrchestratorStats s = GetStats(); OnUpdate(info, s); }
}

// ─── API pública ──────────────────────────────────────────────────────────────

void Orchestrator::Start() {
    if (m_running.exchange(true)) return;
    Log("Orquestrador iniciado.");
    m_thread = std::thread(&Orchestrator::MonitorLoop, this);
}

void Orchestrator::Stop() {
    if (!m_running.exchange(false)) return;
    m_monitor.Stop();
    if (m_thread.joinable()) m_thread.join();
    Log("Orquestrador parado.");
}

bool Orchestrator::IsRunning() const { return m_running.load(); }

void Orchestrator::RegisterTexture(const TextureEntry& entry) {
    std::lock_guard<std::mutex> lk(m_tex_mutex);
    m_textures.erase(std::remove_if(m_textures.begin(), m_textures.end(),
        [&](const TextureEntry& t) { return t.id == entry.id; }), m_textures.end());
    m_textures.push_back(entry);
}

void Orchestrator::UnregisterTexture(uint64_t id) {
    std::lock_guard<std::mutex> lk(m_tex_mutex);
    m_textures.erase(std::remove_if(m_textures.begin(), m_textures.end(),
        [id](const TextureEntry& t) { return t.id == id; }), m_textures.end());
}

void Orchestrator::ClearTextures() {
    std::lock_guard<std::mutex> lk(m_tex_mutex); m_textures.clear();
}

void Orchestrator::AddAction(OrchestratorAction action) {
    m_actions.push_back(std::move(action));
    std::sort(m_actions.begin(), m_actions.end(),
        [](const OrchestratorAction& a, const OrchestratorAction& b) {
            return a.trigger_percent < b.trigger_percent; });
}

OrchestratorStats Orchestrator::GetStats() const {
    std::lock_guard<std::mutex> lk(m_stats_mutex); return m_stats;
}

VRAMInfo Orchestrator::GetLastVRAMInfo() const {
    std::lock_guard<std::mutex> lk(m_stats_mutex); return m_last_info;
}

void Orchestrator::Log(const char* fmt, ...) const {
    if (!m_cfg.enable_logging) return;
    time_t t = time(nullptr);
    struct tm tm_info = {};
    localtime_s(&tm_info, &t);
    char ts[20]; strftime(ts, sizeof(ts), "%H:%M:%S", &tm_info);
    char buf[512];
    va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printf("[%s] %s\n", ts, buf);
    if (m_log_file) { fprintf(m_log_file, "[%s] %s\n", ts, buf); fflush(m_log_file); }
}