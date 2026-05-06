#pragma once

/*
 * Injector.h
 * ─────────────────────────────────────────────────────────────────────────────
 * Módulo 5: Injeta vram_hook.dll em um processo alvo (o jogo).
 *
 * Técnica: LoadLibrary Injection
 *   1. Abre o processo alvo com permissões adequadas
 *   2. Aloca memória no espaço de endereço do processo alvo
 *   3. Escreve o caminho da DLL nessa memória
 *   4. Cria uma thread remota em LoadLibraryA apontando para o caminho
 *   5. Aguarda a thread terminar (DllMain executou)
 *   6. Limpa a memória alocada
 *
 * Por que LoadLibrary e não outras técnicas?
 *   - Manual Map: mais furtivo, mas muito mais complexo (fase 6 do projeto)
 *   - Thread Hijacking: instável em jogos com anti-cheat
 *   - LoadLibrary: simples, estável, suficiente para jogos sem anti-cheat
 *
 * Segurança:
 *   Este injector é para uso em jogos SINGLE-PLAYER sem anti-cheat.
 *   Usar em jogos online pode resultar em ban. Use com responsabilidade.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

// ─── Resultado da injeção ─────────────────────────────────────────────────────

enum class InjectStatus {
    Success,
    ProcessNotFound,
    AccessDenied,        // Rode como Administrador
    DLLNotFound,
    AllocFailed,
    WriteFailed,
    ThreadFailed,
    ThreadTimeout,
    AlreadyInjected,
    UnknownError,
};

struct InjectResult {
    InjectStatus status  = InjectStatus::UnknownError;
    DWORD        pid     = 0;
    std::string  message;

    bool Ok() const { return status == InjectStatus::Success; }
};

// ─── Informações de um processo encontrado ────────────────────────────────────

struct ProcessInfo {
    DWORD       pid;
    std::string name;       // ex: "game.exe"
    std::string window;     // título da janela principal (pode estar vazio)
};

// ─── Configuração da injeção ──────────────────────────────────────────────────

struct InjectorConfig {
    std::string dll_path;             // Caminho completo da vram_hook.dll
    uint32_t    timeout_ms  = 5000;   // Timeout para a thread remota terminar
    bool        wait_for_process = true;  // Aguarda o processo aparecer se não existir
    uint32_t    wait_interval_ms = 1000;  // Intervalo de polling ao aguardar
    uint32_t    wait_max_ms  = 30000;     // Timeout máximo de espera pelo processo
};

// ─── Callback de progresso ────────────────────────────────────────────────────
using InjectorLogCallback = std::function<void(const std::string& msg)>;

// ─────────────────────────────────────────────────────────────────────────────

class Injector {
public:
    explicit Injector(const InjectorConfig& cfg);

    // ── Injeção por nome de processo (ex: "RDR2.exe") ─────────────────────────
    InjectResult InjectByName(const std::string& process_name);

    // ── Injeção por PID ───────────────────────────────────────────────────────
    InjectResult InjectByPID(DWORD pid);

    // ── Ejeção da DLL (descarrega do processo alvo) ───────────────────────────
    InjectResult EjectByName(const std::string& process_name);
    InjectResult EjectByPID (DWORD pid);

    // ── Listagem de processos ─────────────────────────────────────────────────
    static std::vector<ProcessInfo> ListProcesses();

    // ── Verifica se a DLL já está injetada no processo ────────────────────────
    static bool IsInjected(DWORD pid, const std::string& dll_name);

    // Log callback (opcional — se não definido, imprime no stdout)
    InjectorLogCallback OnLog;

private:
    InjectResult DoInject(DWORD pid);
    InjectResult DoEject (DWORD pid);

    static DWORD FindPIDByName(const std::string& name);
    static std::string GetProcessName(DWORD pid);
    static std::string GetWindowTitle(DWORD pid);

    void Log(const std::string& msg) const;
    static const char* StatusToString(InjectStatus s);

    InjectorConfig m_cfg;
};
