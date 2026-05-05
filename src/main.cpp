/*
 * main.cpp
 * ─────────────────────────────────────────────────────────────────────────────
 * Ponto de entrada para testar o monitor + orquestrador juntos.
 * Simula texturas sendo carregadas como se viessem do hook DX11.
 *
 * Compilar com CMake:
 *   mkdir build && cd build
 *   cmake .. && cmake --build .
 */

#include "monitor/VRAMMonitor.h"
#include "orchestrator/Orchestrator.h"

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <csignal>
#include <string>

// ─── Sinal de parada ─────────────────────────────────────────────────────────

static Orchestrator* g_orc = nullptr;

static void OnSignal(int) {
    printf("\n[Main] Encerrando...\n");
    if (g_orc) g_orc->Stop();
}

// ─── Helper: cria textura simulada ───────────────────────────────────────────

static TextureEntry MakeTexture(
    uint64_t id, const char* name,
    uint32_t w, uint32_t h,
    TexturePriority prio)
{
    TextureEntry t;
    t.id         = id;
    t.name       = name;
    t.width      = w;
    t.height     = h;
    t.size_bytes = (uint64_t)w * h * 4; // RGBA8 sem compressão
    t.priority   = prio;
    return t;
}

// ─── Imprime estatísticas formatadas ─────────────────────────────────────────

static void PrintStats(const VRAMInfo& vram, const OrchestratorStats& stats) {
    // Barra de progresso
    constexpr int BAR = 30;
    int filled = (int)(vram.percent / 100.f * BAR);
    char bar[BAR + 1];
    for (int i = 0; i < BAR; i++) bar[i] = (i < filled) ? '#' : '.';
    bar[BAR] = '\0';

    printf("\r[%s] %5.1f%%  |  %.0f/%.0f MB  |  %llu/%llu texturas comprimidas  |  %.1f MB liberados",
        bar,
        vram.percent,
        vram.used_bytes  / (1024.0 * 1024.0),
        vram.total_bytes / (1024.0 * 1024.0),
        (unsigned long long)stats.compressed_textures,
        (unsigned long long)stats.total_textures,
        stats.total_saved_bytes / (1024.0 * 1024.0));
    fflush(stdout);
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    printf("=== VRAM Optimizer — Teste do Monitor + Orquestrador ===\n\n");

    // ── Configuração ─────────────────────────────────────────────────────────
    OrchestratorConfig cfg;
    cfg.monitor_interval_ms   = 500;
    cfg.threshold_warning     = 70.f;
    cfg.threshold_low         = 80.f;
    cfg.threshold_medium      = 90.f;
    cfg.threshold_emergency   = 95.f;
    cfg.max_compress_per_tick = 3;
    cfg.enable_logging        = true;
    cfg.log_to_file           = true;
    strcpy_s(cfg.log_path, "vram_optimizer.log");

    // ── Cria orquestrador ─────────────────────────────────────────────────────
    Orchestrator orc(cfg);
    g_orc = &orc;
    signal(SIGINT, OnSignal);

    // ── Callback de atualização (será usado pela UI C# no futuro) ────────────
    orc.OnUpdate = [](const VRAMInfo& vram, const OrchestratorStats& stats) {
        PrintStats(vram, stats);
    };

    // ── Registra texturas simuladas (normalmente vindas do hook DX11) ─────────
    printf("Registrando texturas simuladas...\n");

    orc.RegisterTexture(MakeTexture(0x1000, "tex_skybox_4k",      4096, 4096, TexturePriority::Disposable));
    orc.RegisterTexture(MakeTexture(0x1001, "tex_arvore_longe",   1024, 1024, TexturePriority::Disposable));
    orc.RegisterTexture(MakeTexture(0x1002, "tex_npc_fundo_a",    2048, 2048, TexturePriority::Low));
    orc.RegisterTexture(MakeTexture(0x1003, "tex_npc_fundo_b",    2048, 2048, TexturePriority::Low));
    orc.RegisterTexture(MakeTexture(0x1004, "tex_terreno_dist",   4096, 2048, TexturePriority::Medium));
    orc.RegisterTexture(MakeTexture(0x1005, "tex_cenario_proximo",2048, 2048, TexturePriority::Medium));
    orc.RegisterTexture(MakeTexture(0x1006, "tex_personagem",     1024,  512, TexturePriority::High));
    orc.RegisterTexture(MakeTexture(0x1007, "tex_arma_principal",  512,  512, TexturePriority::High));
    orc.RegisterTexture(MakeTexture(0x1008, "tex_ui_hud",          256,  256, TexturePriority::Critical));
    orc.RegisterTexture(MakeTexture(0x1009, "tex_ui_minimapa",     512,  512, TexturePriority::Critical));

    OrchestratorStats stats = orc.GetStats();
    printf("%llu texturas registradas (%.1f MB simulados)\n\n",
        (unsigned long long)stats.total_textures,
        // soma estimada: calculada a partir dos tamanhos
        (double)(4096*4096 + 1024*1024 + 2048*2048*2 + 4096*2048 + 2048*2048*2
                 + 1024*512 + 512*512 + 256*256 + 512*512) * 4 / (1024.0*1024.0));

    // ── Ação customizada de exemplo ────────────────────────────────────────────
    {
        OrchestratorAction custom;
        custom.name            = "CustomProfileLogger";
        custom.trigger_percent = 75.f;
        custom.cooldown_sec    = 15.f;
        custom.execute = [](const VRAMInfo& info, std::vector<TextureEntry>& textures) -> uint64_t {
            printf("\n[Custom] Perfil de texturas em %.1f%% de VRAM:\n", info.percent);
            for (const auto& t : textures) {
                printf("  %-30s  %5.1f MB  pri=%d  comp=%s\n",
                    t.name.c_str(),
                    t.size_bytes / (1024.0 * 1024.0),
                    (int)t.priority,
                    t.compression != CompressionLevel::None ? "SIM" : "nao");
            }
            return 0;
        };
        orc.AddAction(std::move(custom));
    }

    // ── Inicia monitoramento ─────────────────────────────────────────────────
    printf("Monitorando VRAM... (Ctrl+C para parar)\n");
    printf("─────────────────────────────────────────────────────────────────\n");
    orc.Start();

    // Aguarda o orquestrador terminar (parado por Ctrl+C ou pelo OnSignal)
    while (orc.IsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // ── Relatório final ───────────────────────────────────────────────────────
    stats = orc.GetStats();
    printf("\n\n=== Relatório Final ===\n");
    printf("  Texturas totais:          %llu\n", (unsigned long long)stats.total_textures);
    printf("  Texturas comprimidas:     %llu\n", (unsigned long long)stats.compressed_textures);
    printf("  Total liberado (estimado):%.1f MB\n", stats.total_saved_bytes / (1024.0*1024.0));
    printf("  Ações disparadas:         %u\n",   stats.actions_fired);
    printf("  Pico de uso de VRAM:      %.1f%%\n", stats.peak_vram_percent);
    printf("  Log salvo em:             vram_optimizer.log\n");

    return 0;
}
