#pragma once

/*
 * Orchestrator.h
 * ─────────────────────────────────────────────────────────────────────────────
 * Conecta o VRAMMonitor ao sistema de compressão de texturas.
 *
 * Responsabilidades:
 *   - Registrar texturas carregadas pelo jogo (via hook ou IPC)
 *   - Definir ações em limiares crescentes de uso de VRAM
 *   - Executar ações com cooldown para não thrash a GPU
 *   - Rodar o loop de monitoramento em background (thread dedicada)
 *
 * Fluxo:
 *   VRAMMonitor  →  Orchestrator::OnVRAMUpdate()
 *                        │
 *                        ├─ 70%: LogWarning
 *                        ├─ 80%: CompressLowPriority
 *                        ├─ 90%: CompressMediumPriority
 *                        └─ 95%: EmergencyReduction
 */

#include "VRAMMonitor.h"
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>

// ─── Perfil de textura ────────────────────────────────────────────────────────

enum class TexturePriority : int {
    Critical  = 1,   // HUD, personagem principal — nunca comprimir
    High      = 2,   // NPCs próximos, objetos interativos
    Medium    = 3,   // Cenário próximo
    Low       = 4,   // Vegetação, objetos distantes
    Disposable= 5,   // Skybox, detalhes de fundo — comprimir primeiro
};

enum class CompressionLevel : int {
    None     = 0,   // Original
    BC1      = 1,   // Sem alpha, ~6:1 vs RGBA8  (menor custo de compressão)
    BC3      = 2,   // Com alpha, ~4:1 vs RGBA8
    BC7      = 3,   // Alta qualidade, ~8:1 vs RGBA8 (maior custo)
    HalfRes  = 4,   // Downscale 50% antes de comprimir
    QuarterRes = 5, // Downscale 75% — emergência
};

struct TextureEntry {
    uint64_t         id           = 0;       // Identificador único (ex: ptr do recurso DX)
    std::string      name;                   // Nome legível (para debug/UI)
    uint64_t         size_bytes   = 0;       // Tamanho original em bytes
    uint32_t         width        = 0;
    uint32_t         height       = 0;
    TexturePriority  priority     = TexturePriority::Medium;
    CompressionLevel compression  = CompressionLevel::None;
    uint64_t         saved_bytes  = 0;       // Bytes economizados após compressão
};

// ─── Ação de otimização ───────────────────────────────────────────────────────

struct OrchestratorAction {
    std::string  name;
    float        trigger_percent = 80.f;   // Dispara quando VRAM% >= este valor
    float        cooldown_sec    = 5.f;    // Mínimo entre execuções consecutivas

    // Callback: recebe o estado da VRAM e a lista de texturas gerenciadas.
    // Deve retornar o número de bytes efetivamente liberados.
    std::function<uint64_t(const VRAMInfo&, std::vector<TextureEntry>&)> execute;

    // Estado interno
    double last_run_time = 0.0;  // timestamp (segundos desde epoch)
    bool   IsReady(float current_percent, double now_sec) const;
};

// ─── Configuração do orquestrador ─────────────────────────────────────────────

struct OrchestratorConfig {
    float    monitor_interval_ms     = 500.f;
    float    threshold_warning       = 70.f;
    float    threshold_low           = 80.f;
    float    threshold_medium        = 90.f;
    float    threshold_emergency     = 95.f;
    uint32_t max_compress_per_tick   = 4;     // Máximo de texturas por ação
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

    // ── Ciclo de vida ────────────────────────────────────────────────────────
    void Start();                   // Inicia monitoramento em thread dedicada
    void Stop();                    // Para o loop de forma segura
    bool IsRunning() const;

    // ── Gerenciamento de texturas ─────────────────────────────────────────────
    void RegisterTexture  (const TextureEntry& entry);
    void UnregisterTexture(uint64_t id);
    void ClearTextures();

    // ── Ações customizadas ────────────────────────────────────────────────────
    // Adicione suas próprias ações além das padrão
    void AddAction(OrchestratorAction action);

    // ── Consultas ─────────────────────────────────────────────────────────────
    OrchestratorStats GetStats() const;
    VRAMInfo          GetLastVRAMInfo() const;

    // Callback opcional: chamado em cada atualização de VRAM
    std::function<void(const VRAMInfo&, const OrchestratorStats&)> OnUpdate;

    // Desabilita cópia
    Orchestrator(const Orchestrator&)            = delete;
    Orchestrator& operator=(const Orchestrator&) = delete;

private:
    void MonitorLoop();
    void ProcessVRAM(const VRAMInfo& info);
    void Log(const char* fmt, ...) const;

    // Ações padrão registradas no construtor
    void RegisterDefaultActions();

    uint64_t ActionLogWarning      (const VRAMInfo&, std::vector<TextureEntry>&);
    uint64_t ActionCompressLow     (const VRAMInfo&, std::vector<TextureEntry>&);
    uint64_t ActionCompressMedium  (const VRAMInfo&, std::vector<TextureEntry>&);
    uint64_t ActionEmergency       (const VRAMInfo&, std::vector<TextureEntry>&);

    // Compressão de uma textura individual
    // Na integração real, envia o comando via IPC para o hook DX11
    uint64_t CompressTexture(TextureEntry& tex, CompressionLevel level);

    OrchestratorConfig         m_cfg;
    VRAMMonitor                m_monitor;
    std::vector<TextureEntry>  m_textures;
    std::vector<OrchestratorAction> m_actions;
    OrchestratorStats          m_stats;
    VRAMInfo                   m_last_info;

    mutable std::mutex         m_tex_mutex;
    mutable std::mutex         m_stats_mutex;
    std::thread                m_thread;
    std::atomic<bool>          m_running{ false };

    FILE*                      m_log_file = nullptr;
};
