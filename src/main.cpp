/*
 * main.cpp — v3
 * ─────────────────────────────────────────────────────────────────────────────
 * Integra: Monitor + Orchestrator + IPCServer (hook) + IPCStatusServer (UI)
 *
 * Modos:
 *   vram_optimizer.exe              → modo completo (hook + UI)
 *   vram_optimizer.exe --standalone → sem hook, texturas simuladas
 *
 * Compilar direto (Developer Command Prompt VS 2022):
 *   cl /std:c++17 /EHsc /O2 /W3 ^
 *      src\main.cpp ^
 *      monitor\VRAMMonitor.cpp ^
 *      orchestrator\Orchestrator.cpp ^
 *      ipc\IPCServer.cpp ^
 *      ipc\IPCStatusServer.cpp ^
 *      compressor\TextureCompressor.cpp ^
 *      /I . ^
 *      /link dxgi.lib d3d11.lib gdi32.lib ^
 *      /OUT:vram_optimizer.exe
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX        // evita que windows.h defina macros min/max
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <string>
#include <algorithm>

#include "monitor/VRAMMonitor.h"
#include "orchestrator/Orchestrator.h"
#include "ipc/IPCServer.h"
#include "ipc/IPCStatusServer.h"
#include "ipc/IPCProtocol.h"

// ─────────────────────────────────────────────────────────────────────────────
// Parada global
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_quit{ false };
static Orchestrator*     g_orc  = nullptr;

static void OnSignal(int) {
    printf("\n[Main] Ctrl+C — encerrando...\n");
    g_quit = true;
    if (g_orc) g_orc->Stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// Display no CMD
// ─────────────────────────────────────────────────────────────────────────────

static void PrintStatus(const VRAMInfo& v, const OrchestratorStats& s,
                        bool hook_ok, bool ui_ok)
{
    constexpr int BAR = 34;
    int filled = std::min((int)(v.percent / 100.f * BAR), BAR);
    char bar[BAR + 1];
    for (int i = 0; i < BAR; i++)
        bar[i] = i < filled ? (v.percent >= 90.f ? '!' :
                               v.percent >= 75.f ? '#' : '=') : '.';
    bar[BAR] = '\0';

    printf("\r[%s] %5.1f%%  %.0f/%.0f MB  "
           "tex:%llu/%llu  salvo:%.0fMB  hook:%s ui:%s   ",
        bar, v.percent,
        v.used_bytes  / (1024.0 * 1024.0),
        v.total_bytes / (1024.0 * 1024.0),
        (unsigned long long)s.compressed_textures,
        (unsigned long long)s.total_textures,
        s.total_saved_bytes / (1024.0 * 1024.0),
        hook_ok ? "OK" : "--",
        ui_ok   ? "OK" : "--");
    fflush(stdout);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper de textura simulada
// ─────────────────────────────────────────────────────────────────────────────

static TextureEntry MakeTex(uint64_t id, const char* name,
                             uint32_t w, uint32_t h, TexturePriority prio)
{
    TextureEntry t;
    t.id         = id;
    t.name       = name;
    t.width      = w;
    t.height     = h;
    t.size_bytes = (uint64_t)w * h * 4;
    t.priority   = prio;
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
// Conecta callbacks da UI → Orchestrator
// ─────────────────────────────────────────────────────────────────────────────

static void WireUICallbacks(IPCStatusServer& ui, Orchestrator& orc) {
    ui.OnCommandReceived = [&orc](const char* cmd) {
        if (strcmp(cmd, "restore_all") == 0) {
            orc.ClearTextures();
            printf("\n[Main] Texturas restauradas pela UI.\n");
        }
    };

    ui.OnConfigReceived = [&orc](const UIConfig& c) {
        // Aplica novos thresholds nas ações do orquestrador.
        // Abordagem simples: para, reconfigura, reinicia.
        // Em produção: implemente Orchestrator::Reconfigure(cfg).
        printf("\n[Main] Configuracao atualizada pela UI"
               " (warn=%.0f low=%.0f med=%.0f emerg=%.0f).\n",
               c.threshold_warning, c.threshold_low,
               c.threshold_medium,  c.threshold_emergency);
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// Envia snapshot do orquestrador para a UI
// ─────────────────────────────────────────────────────────────────────────────

static void SendToUI(IPCStatusServer& ui,
                     const VRAMInfo& vram,
                     const OrchestratorStats& stats,
                     bool hook_connected)
{
    ui.SendSnapshot(
        vram.percent,
        stats.peak_vram_percent,
        vram.used_bytes,
        vram.total_bytes,
        vram.free_bytes,
        vram.backend,
        hook_connected,
        stats.total_saved_bytes,
        (int)stats.total_textures,
        (int)stats.compressed_textures,
        (int)stats.actions_fired);
}

// ─────────────────────────────────────────────────────────────────────────────
// Relatório final
// ─────────────────────────────────────────────────────────────────────────────

static void PrintReport(const Orchestrator& orc) {
    auto s = orc.GetStats();
    auto v = orc.GetLastVRAMInfo();
    printf("\n\n=== Relatório Final ===\n");
    printf("  Backend         : %s\n",     v.backend);
    printf("  Pico VRAM       : %.1f%%\n", s.peak_vram_percent);
    printf("  Texturas totais : %llu\n",   (unsigned long long)s.total_textures);
    printf("  Comprimidas     : %llu\n",   (unsigned long long)s.compressed_textures);
    printf("  Liberado        : %.1f MB\n",s.total_saved_bytes / (1024.0*1024.0));
    printf("  Ações disparadas: %u\n",     s.actions_fired);
    printf("  Log             : vram_optimizer.log\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// RunStandalone — teste sem hook, texturas simuladas
// ─────────────────────────────────────────────────────────────────────────────

static int RunStandalone()
{
    printf("==========================================================\n");
    printf("  VRAM Optimizer  |  Standalone (sem hook DX11)\n");
    printf("==========================================================\n\n");

    OrchestratorConfig cfg;
    cfg.monitor_interval_ms   = 500;
    cfg.threshold_warning     = 50.f;  // limiares baixos p/ facilitar teste
    cfg.threshold_low         = 60.f;
    cfg.threshold_medium      = 75.f;
    cfg.threshold_emergency   = 85.f;
    cfg.max_compress_per_tick = 3;
    cfg.enable_logging        = true;
    cfg.log_to_file           = true;
    strcpy_s(cfg.log_path, "vram_optimizer.log");

    Orchestrator    orc(cfg);
    IPCStatusServer ui;
    g_orc = &orc;
    signal(SIGINT, OnSignal);

    WireUICallbacks(ui, orc);

    orc.OnUpdate = [&](const VRAMInfo& vram, const OrchestratorStats& stats) {
        PrintStatus(vram, stats, false, ui.IsUIConnected());
        SendToUI(ui, vram, stats, false);
    };

    // Texturas simuladas
    orc.RegisterTexture(MakeTex(0x1001, "tex_skybox_4k",       4096, 4096, TexturePriority::Disposable));
    orc.RegisterTexture(MakeTex(0x1002, "tex_clouds",          2048, 2048, TexturePriority::Disposable));
    orc.RegisterTexture(MakeTex(0x1003, "tex_arvores_dist",    1024, 1024, TexturePriority::Low));
    orc.RegisterTexture(MakeTex(0x1004, "tex_pedras_longe",    2048, 2048, TexturePriority::Low));
    orc.RegisterTexture(MakeTex(0x1005, "tex_terreno_a",       4096, 2048, TexturePriority::Medium));
    orc.RegisterTexture(MakeTex(0x1006, "tex_terreno_b",       4096, 2048, TexturePriority::Medium));
    orc.RegisterTexture(MakeTex(0x1007, "tex_npc_01",          1024,  512, TexturePriority::High));
    orc.RegisterTexture(MakeTex(0x1008, "tex_personagem_main", 2048, 2048, TexturePriority::High));
    orc.RegisterTexture(MakeTex(0x1009, "tex_arma",             512,  512, TexturePriority::High));
    orc.RegisterTexture(MakeTex(0x100A, "tex_hud",              256,  256, TexturePriority::Critical));
    orc.RegisterTexture(MakeTex(0x100B, "tex_minimapa",         512,  512, TexturePriority::Critical));

    {
        VRAMInfo tmp; VRAMMonitor m; m.Read(tmp);
        printf("Backend : %s\n", tmp.backend);
        printf("VRAM    : %.0f MB\n\n", tmp.total_bytes / (1024.0*1024.0));
    }
    printf("Ctrl+C para parar | UI C#: abra VRAMOptimizer.UI.exe\n");
    printf("----------------------------------------------------------\n\n");

    ui.Start();
    orc.Start();

    while (!g_quit.load() && orc.IsRunning())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ui.Stop();
    PrintReport(orc);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// RunFull — hook DX11 + UI C#
// ─────────────────────────────────────────────────────────────────────────────

static int RunFull()
{
    printf("==========================================================\n");
    printf("  VRAM Optimizer  |  Modo Completo (hook + UI)\n");
    printf("==========================================================\n\n");

    OrchestratorConfig cfg;
    cfg.monitor_interval_ms   = 500;
    cfg.threshold_warning     = 70.f;
    cfg.threshold_low         = 80.f;
    cfg.threshold_medium      = 90.f;
    cfg.threshold_emergency   = 95.f;
    cfg.max_compress_per_tick = 4;
    cfg.enable_logging        = true;
    cfg.log_to_file           = true;
    strcpy_s(cfg.log_path, "vram_optimizer.log");

    Orchestrator    orc(cfg);
    IPCServer       hook;   // ← vram_hook.dll
    IPCStatusServer ui;     // ← VRAMOptimizer.UI.exe
    g_orc = &orc;
    signal(SIGINT, OnSignal);

    // Hook → Orquestrador
    hook.OnTextureCreated = [&orc](const TextureCreatedEvent& e) {
        TextureEntry t;
        t.id         = e.texture_id;
        t.name       = "tex_" + std::to_string(e.texture_id & 0xFFFF);
        t.width      = e.width;
        t.height     = e.height;
        t.size_bytes = e.size_bytes;
        t.priority   = e.width >= 4096 ? TexturePriority::Disposable :
                       e.width >= 2048 ? TexturePriority::Low         :
                       e.width >= 1024 ? TexturePriority::Medium      :
                       e.width >=  512 ? TexturePriority::High        :
                                         TexturePriority::Critical;
        orc.RegisterTexture(t);
    };
    hook.OnTextureReleased = [&orc](const TextureReleasedEvent& e) {
        orc.UnregisterTexture(e.texture_id);
    };

    // UI → Orquestrador
    WireUICallbacks(ui, orc);

    // Orquestrador → UI + CMD display
    orc.OnUpdate = [&](const VRAMInfo& vram, const OrchestratorStats& stats) {
        PrintStatus(vram, stats, hook.IsRunning(), ui.IsUIConnected());
        SendToUI(ui, vram, stats, hook.IsRunning());
    };

    printf("Pipes ativos:\n");
    printf("  Hook  : %s\n", PIPE_EVENTS);
    printf("  Hook  : %s\n", PIPE_COMMANDS);
    printf("  UI    : vram_opt_status\n");
    printf("  UI    : vram_opt_ui_cmd\n\n");
    printf("Passos:\n");
    printf("  1. Abra VRAMOptimizer.UI.exe\n");
    printf("  2. Inicie o jogo\n");
    printf("  3. Use vram_injector.exe --inject <jogo.exe>\n\n");
    printf("Ctrl+C para parar.\n");
    printf("----------------------------------------------------------\n\n");

    ui.Start();
    hook.Start();
    orc.Start();

    while (!g_quit.load() && orc.IsRunning())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ui.Stop();
    hook.Stop();
    PrintReport(orc);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    bool standalone = false;
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--standalone") == 0) standalone = true;
    return standalone ? RunStandalone() : RunFull();
}