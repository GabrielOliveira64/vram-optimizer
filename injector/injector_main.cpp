/*
 * injector_main.cpp
 * ─────────────────────────────────────────────────────────────────────────────
 * CLI do Injector — executável separado do orquestrador.
 *
 * Uso:
 *   vram_injector.exe --list
 *       Lista todos os processos em execução.
 *
 *   vram_injector.exe --inject <processo.exe>
 *       Injeta vram_hook.dll no processo. Aguarda até 30s o processo iniciar.
 *
 *   vram_injector.exe --inject <processo.exe> --dll <caminho\vram_hook.dll>
 *       Injeta uma DLL específica.
 *
 *   vram_injector.exe --inject <processo.exe> --nowait
 *       Falha imediatamente se o processo não estiver rodando.
 *
 *   vram_injector.exe --eject <processo.exe>
 *       Remove a DLL do processo.
 *
 *   vram_injector.exe --watch <processo.exe>
 *       Modo watch: injeta quando o processo iniciar, e re-injeta se ele
 *       reiniciar (útil para sessões longas de jogo).
 *
 * Exemplos práticos:
 *   vram_injector.exe --inject RDR2.exe
 *   vram_injector.exe --inject eldenring.exe --dll C:\tools\vram_hook.dll
 *   vram_injector.exe --list
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>

#include "Injector.h"

// ─────────────────────────────────────────────────────────────────────────────
// Controle de parada
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_quit{ false };

static void OnSignal(int) {
    printf("\n[Injector] Encerrando...\n");
    g_quit = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers de display
// ─────────────────────────────────────────────────────────────────────────────

static void PrintBanner() {
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║          VRAM Optimizer — Injector v1.0          ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");
}

static void PrintUsage(const char* exe) {
    printf("Uso:\n");
    printf("  %s --list\n", exe);
    printf("      Lista processos em execucao.\n\n");
    printf("  %s --inject <processo.exe> [--dll <vram_hook.dll>] [--nowait]\n", exe);
    printf("      Injeta a DLL no processo.\n\n");
    printf("  %s --eject <processo.exe>\n", exe);
    printf("      Remove a DLL do processo.\n\n");
    printf("  %s --watch <processo.exe> [--dll <vram_hook.dll>]\n", exe);
    printf("      Injeta automaticamente quando o processo iniciar/reiniciar.\n\n");
    printf("Exemplos:\n");
    printf("  %s --inject RDR2.exe\n", exe);
    printf("  %s --inject eldenring.exe --dll C:\\tools\\vram_hook.dll\n", exe);
    printf("  %s --watch Cyberpunk2077.exe\n\n", exe);
}

// Resolução do caminho da DLL: procura no diretório do próprio executável
static std::string ResolveDLLPath(const std::string& hint = "") {
    if (!hint.empty()) return hint;

    char exe_path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);

    // Troca o nome do exe pelo nome da DLL
    std::string path = exe_path;
    size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos)
        path = path.substr(0, slash + 1);
    path += "vram_hook.dll";
    return path;
}

// ─────────────────────────────────────────────────────────────────────────────
// Modo --list
// ─────────────────────────────────────────────────────────────────────────────

static int CmdList() {
    auto procs = Injector::ListProcesses();
    printf("%-8s  %-40s  %s\n", "PID", "Nome", "Janela");
    printf("%-8s  %-40s  %s\n",
           "--------",
           "----------------------------------------",
           "──────────────────────────────");
    for (const auto& p : procs) {
        printf("%-8lu  %-40s  %s\n",
               (unsigned long)p.pid,
               p.name.c_str(),
               p.window.c_str());
    }
    printf("\n%zu processos encontrados.\n", procs.size());
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Modo --inject
// ─────────────────────────────────────────────────────────────────────────────

static int CmdInject(const std::string& process,
                     const std::string& dll_hint,
                     bool nowait)
{
    InjectorConfig cfg;
    cfg.dll_path         = ResolveDLLPath(dll_hint);
    cfg.wait_for_process = !nowait;
    cfg.wait_max_ms      = 30000;
    cfg.wait_interval_ms = 1000;
    cfg.timeout_ms       = 5000;

    printf("DLL   : %s\n", cfg.dll_path.c_str());
    printf("Alvo  : %s\n\n", process.c_str());

    Injector inj(cfg);
    auto result = inj.InjectByName(process);

    if (result.Ok()) {
        printf("\n[OK] %s\n", result.message.c_str());
        return 0;
    } else {
        printf("\n[ERRO] %s\n", result.message.c_str());
        if (result.status == InjectStatus::AccessDenied)
            printf("       Dica: Rode o injector como Administrador.\n");
        if (result.status == InjectStatus::DLLNotFound)
            printf("       Dica: Use --dll para especificar o caminho da DLL.\n");
        return 1;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Modo --eject
// ─────────────────────────────────────────────────────────────────────────────

static int CmdEject(const std::string& process, const std::string& dll_hint) {
    InjectorConfig cfg;
    cfg.dll_path = ResolveDLLPath(dll_hint);

    Injector inj(cfg);
    auto result = inj.EjectByName(process);

    if (result.Ok()) {
        printf("[OK] %s\n", result.message.c_str());
        return 0;
    } else {
        printf("[ERRO] %s\n", result.message.c_str());
        return 1;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Modo --watch
// Monitora o processo e (re)injeta automaticamente quando ele iniciar
// ─────────────────────────────────────────────────────────────────────────────

static int CmdWatch(const std::string& process, const std::string& dll_hint) {
    signal(SIGINT, OnSignal);

    InjectorConfig cfg;
    cfg.dll_path         = ResolveDLLPath(dll_hint);
    cfg.wait_for_process = false; // gerenciamos o loop aqui
    cfg.timeout_ms       = 5000;

    printf("DLL   : %s\n", cfg.dll_path.c_str());
    printf("Alvo  : %s\n", process.c_str());
    printf("Modo  : Watch — re-injeta automaticamente ao iniciar/reiniciar.\n");
    printf("Ctrl+C para parar.\n\n");

    Injector inj(cfg);
    DWORD last_pid = 0;

    while (!g_quit.load()) {
        // Procura o processo
        auto procs = Injector::ListProcesses();
        DWORD current_pid = 0;
        for (const auto& p : procs) {
            std::string pname = p.name;
            std::transform(pname.begin(), pname.end(), pname.begin(), ::tolower);
            std::string target = process;
            std::transform(target.begin(), target.end(), target.begin(), ::tolower);
            if (pname == target) { current_pid = p.pid; break; }
        }

        if (current_pid == 0) {
            // Processo não está rodando
            if (last_pid != 0) {
                printf("[Watch] Processo encerrado (PID %lu). Aguardando reinicio...\n",
                       (unsigned long)last_pid);
                last_pid = 0;
            } else {
                printf("\r[Watch] Aguardando '%s'...    ", process.c_str());
                fflush(stdout);
            }
        } else if (current_pid != last_pid) {
            // Processo iniciou (ou reiniciou com novo PID)
            printf("\n[Watch] Processo detectado! PID=%lu. Aguardando DX11 inicializar...\n",
                   (unsigned long)current_pid);

            // Aguarda 3s para o jogo inicializar o DirectX
            for (int i = 0; i < 6 && !g_quit.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));

            if (g_quit.load()) break;

            auto result = inj.InjectByPID(current_pid);
            if (result.Ok()) {
                printf("[Watch] [OK] Hook injetado em PID %lu.\n", (unsigned long)current_pid);
                last_pid = current_pid;
            } else {
                printf("[Watch] [ERRO] %s\n", result.message.c_str());
                // Não atualiza last_pid — vai tentar novamente
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    printf("\n[Watch] Encerrado.\n");
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    PrintBanner();

    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    // Parse de argumentos
    std::string cmd, process, dll_hint;
    bool nowait = false;

    for (int i = 1; i < argc; ++i) {
        if      (strcmp(argv[i], "--list")   == 0) cmd = "list";
        else if (strcmp(argv[i], "--inject") == 0 && i+1 < argc) { cmd = "inject"; process = argv[++i]; }
        else if (strcmp(argv[i], "--eject")  == 0 && i+1 < argc) { cmd = "eject";  process = argv[++i]; }
        else if (strcmp(argv[i], "--watch")  == 0 && i+1 < argc) { cmd = "watch";  process = argv[++i]; }
        else if (strcmp(argv[i], "--dll")    == 0 && i+1 < argc) { dll_hint = argv[++i]; }
        else if (strcmp(argv[i], "--nowait") == 0) { nowait = true; }
        else if (strcmp(argv[i], "--help")   == 0) { PrintUsage(argv[0]); return 0; }
    }

    if      (cmd == "list")   return CmdList();
    else if (cmd == "inject") return CmdInject(process, dll_hint, nowait);
    else if (cmd == "eject")  return CmdEject(process, dll_hint);
    else if (cmd == "watch")  return CmdWatch(process, dll_hint);
    else {
        printf("[ERRO] Comando invalido.\n\n");
        PrintUsage(argv[0]);
        return 1;
    }
}
