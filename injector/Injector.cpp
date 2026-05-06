/*
 * Injector.cpp
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "Injector.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>   // CreateToolhelp32Snapshot, Process32First/Next
#include <psapi.h>      // EnumProcessModules, GetModuleFileNameEx
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <thread>

#pragma comment(lib, "psapi.lib")

// ─────────────────────────────────────────────────────────────────────────────
// Construtor
// ─────────────────────────────────────────────────────────────────────────────

Injector::Injector(const InjectorConfig& cfg) : m_cfg(cfg) {
    // Resolve caminho absoluto da DLL — necessário porque o processo alvo
    // vai resolver o caminho a partir do SEU diretório de trabalho, não do nosso
    if (!cfg.dll_path.empty()) {
        char abs[MAX_PATH] = {};
        if (GetFullPathNameA(cfg.dll_path.c_str(), MAX_PATH, abs, nullptr))
            m_cfg.dll_path = abs;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// API pública
// ─────────────────────────────────────────────────────────────────────────────

InjectResult Injector::InjectByName(const std::string& process_name) {
    Log("Procurando processo: " + process_name);

    DWORD pid = FindPIDByName(process_name);

    // Aguarda o processo aparecer se configurado
    if (pid == 0 && m_cfg.wait_for_process) {
        Log("Processo nao encontrado. Aguardando iniciar...");
        auto start = std::chrono::steady_clock::now();

        while (pid == 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(m_cfg.wait_interval_ms));

            pid = FindPIDByName(process_name);

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (elapsed >= (int64_t)m_cfg.wait_max_ms) {
                InjectResult r;
                r.status  = InjectStatus::ProcessNotFound;
                r.message = "Timeout aguardando '" + process_name + "'";
                Log(r.message);
                return r;
            }

            if (pid != 0)
                Log("Processo encontrado! Aguardando 2s para inicializar DX11...");
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        }
    }

    if (pid == 0) {
        InjectResult r;
        r.status  = InjectStatus::ProcessNotFound;
        r.message = "Processo '" + process_name + "' nao encontrado.";
        Log(r.message);
        return r;
    }

    return DoInject(pid);
}

InjectResult Injector::InjectByPID(DWORD pid) {
    Log("Injetando em PID: " + std::to_string(pid));
    return DoInject(pid);
}

InjectResult Injector::EjectByName(const std::string& process_name) {
    DWORD pid = FindPIDByName(process_name);
    if (pid == 0) {
        InjectResult r;
        r.status  = InjectStatus::ProcessNotFound;
        r.message = "Processo '" + process_name + "' nao encontrado para ejecao.";
        return r;
    }
    return DoEject(pid);
}

InjectResult Injector::EjectByPID(DWORD pid) {
    return DoEject(pid);
}

// ─────────────────────────────────────────────────────────────────────────────
// Injeção — LoadLibrary Injection
// ─────────────────────────────────────────────────────────────────────────────

InjectResult Injector::DoInject(DWORD pid) {
    InjectResult result;
    result.pid = pid;

    // ── Verifica se já foi injetado ───────────────────────────────────────────
    std::string dll_name = m_cfg.dll_path;
    size_t slash = dll_name.find_last_of("\\/");
    if (slash != std::string::npos) dll_name = dll_name.substr(slash + 1);

    if (IsInjected(pid, dll_name)) {
        result.status  = InjectStatus::AlreadyInjected;
        result.message = dll_name + " ja esta injetada no processo " + std::to_string(pid);
        Log(result.message);
        return result;
    }

    // ── Verifica se a DLL existe ──────────────────────────────────────────────
    if (GetFileAttributesA(m_cfg.dll_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        result.status  = InjectStatus::DLLNotFound;
        result.message = "DLL nao encontrada: " + m_cfg.dll_path;
        Log(result.message);
        return result;
    }

    Log("DLL: " + m_cfg.dll_path);
    Log("PID: " + std::to_string(pid) + " (" + GetProcessName(pid) + ")");

    // ── Abre o processo alvo ──────────────────────────────────────────────────
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD   |   // Para CreateRemoteThread
        PROCESS_VM_OPERATION    |   // Para VirtualAllocEx / VirtualFreeEx
        PROCESS_VM_WRITE        |   // Para WriteProcessMemory
        PROCESS_VM_READ         |   // Para verificação
        PROCESS_QUERY_INFORMATION,  // Para EnumProcessModules
        FALSE, pid);

    if (!hProc) {
        DWORD err = GetLastError();
        result.status  = (err == ERROR_ACCESS_DENIED)
                         ? InjectStatus::AccessDenied
                         : InjectStatus::UnknownError;
        result.message = "OpenProcess falhou (err=" + std::to_string(err) +
                         "). Tente rodar como Administrador.";
        Log(result.message);
        return result;
    }

    // ── Aloca memória no processo alvo para o caminho da DLL ─────────────────
    size_t path_size = m_cfg.dll_path.size() + 1; // +1 para o '\0'

    LPVOID remote_mem = VirtualAllocEx(
        hProc,
        nullptr,
        path_size,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);

    if (!remote_mem) {
        result.status  = InjectStatus::AllocFailed;
        result.message = "VirtualAllocEx falhou (err=" +
                         std::to_string(GetLastError()) + ")";
        Log(result.message);
        CloseHandle(hProc);
        return result;
    }

    // ── Escreve o caminho da DLL na memória remota ────────────────────────────
    SIZE_T written = 0;
    if (!WriteProcessMemory(hProc, remote_mem,
                            m_cfg.dll_path.c_str(), path_size, &written)
        || written != path_size)
    {
        result.status  = InjectStatus::WriteFailed;
        result.message = "WriteProcessMemory falhou (err=" +
                         std::to_string(GetLastError()) + ")";
        Log(result.message);
        VirtualFreeEx(hProc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return result;
    }

    // ── Cria thread remota apontando para LoadLibraryA ────────────────────────
    // LoadLibraryA está no mesmo endereço em todos os processos (kernel32 é
    // carregado no mesmo endereço virtual em todos os processos no Windows)
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    FARPROC load_lib = GetProcAddress(kernel32, "LoadLibraryA");

    Log("Criando thread remota em LoadLibraryA...");

    HANDLE hThread = CreateRemoteThread(
        hProc,
        nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(load_lib),
        remote_mem,
        0, nullptr);

    if (!hThread) {
        result.status  = InjectStatus::ThreadFailed;
        result.message = "CreateRemoteThread falhou (err=" +
                         std::to_string(GetLastError()) + ")";
        Log(result.message);
        VirtualFreeEx(hProc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return result;
    }

    // ── Aguarda a thread terminar (DllMain executou) ──────────────────────────
    DWORD wait = WaitForSingleObject(hThread, m_cfg.timeout_ms);

    if (wait == WAIT_TIMEOUT) {
        result.status  = InjectStatus::ThreadTimeout;
        result.message = "Timeout aguardando DllMain (" +
                         std::to_string(m_cfg.timeout_ms) + "ms)";
        Log(result.message);
        TerminateThread(hThread, 0);
        CloseHandle(hThread);
        VirtualFreeEx(hProc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return result;
    }

    // Verifica se LoadLibrary retornou um handle válido (não-nulo)
    DWORD thread_exit = 0;
    GetExitCodeThread(hThread, &thread_exit);

    // ── Limpeza ───────────────────────────────────────────────────────────────
    CloseHandle(hThread);
    VirtualFreeEx(hProc, remote_mem, 0, MEM_RELEASE);
    CloseHandle(hProc);

    if (thread_exit == 0) {
        // LoadLibrary retornou NULL — DLL falhou ao carregar (DllMain retornou FALSE)
        result.status  = InjectStatus::UnknownError;
        result.message = "LoadLibrary retornou NULL no processo alvo. "
                         "Verifique dependencias da DLL (MinHook.x64.dll, d3d11.dll).";
        Log(result.message);
        return result;
    }

    result.status  = InjectStatus::Success;
    result.message = dll_name + " injetada com sucesso em PID " +
                     std::to_string(pid);
    Log("[OK] " + result.message);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Ejeção — FreeLibrary Injection
// ─────────────────────────────────────────────────────────────────────────────

InjectResult Injector::DoEject(DWORD pid) {
    InjectResult result;
    result.pid = pid;

    std::string dll_name = m_cfg.dll_path;
    size_t slash = dll_name.find_last_of("\\/");
    if (slash != std::string::npos) dll_name = dll_name.substr(slash + 1);

    // Encontra o handle da DLL no processo alvo
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE, pid);

    if (!hProc) {
        result.status  = InjectStatus::AccessDenied;
        result.message = "OpenProcess falhou para ejecao.";
        Log(result.message);
        return result;
    }

    // Enumera módulos do processo para achar o handle da DLL
    HMODULE modules[1024] = {};
    DWORD   needed = 0;
    HMODULE dll_handle = nullptr;

    if (EnumProcessModules(hProc, modules, sizeof(modules), &needed)) {
        DWORD count = needed / sizeof(HMODULE);
        char mod_name[MAX_PATH] = {};
        for (DWORD i = 0; i < count; ++i) {
            GetModuleFileNameExA(hProc, modules[i], mod_name, MAX_PATH);
            std::string mname = mod_name;
            // Compara só o nome do arquivo, não o caminho completo
            size_t s = mname.find_last_of("\\/");
            if (s != std::string::npos) mname = mname.substr(s + 1);
            // Comparação case-insensitive
            std::transform(mname.begin(), mname.end(), mname.begin(), ::tolower);
            std::string target = dll_name;
            std::transform(target.begin(), target.end(), target.begin(), ::tolower);
            if (mname == target) {
                dll_handle = modules[i];
                break;
            }
        }
    }

    if (!dll_handle) {
        result.status  = InjectStatus::DLLNotFound;
        result.message = dll_name + " nao encontrada no processo " +
                         std::to_string(pid);
        Log(result.message);
        CloseHandle(hProc);
        return result;
    }

    // Cria thread remota em FreeLibrary passando o handle da DLL
    FARPROC free_lib = GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "FreeLibrary");

    HANDLE hThread = CreateRemoteThread(
        hProc, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(free_lib),
        dll_handle, 0, nullptr);

    if (!hThread) {
        result.status  = InjectStatus::ThreadFailed;
        result.message = "CreateRemoteThread (FreeLibrary) falhou.";
        Log(result.message);
        CloseHandle(hProc);
        return result;
    }

    WaitForSingleObject(hThread, m_cfg.timeout_ms);
    CloseHandle(hThread);
    CloseHandle(hProc);

    result.status  = InjectStatus::Success;
    result.message = dll_name + " ejetada com sucesso de PID " +
                     std::to_string(pid);
    Log("[OK] " + result.message);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Utilitários estáticos
// ─────────────────────────────────────────────────────────────────────────────

DWORD Injector::FindPIDByName(const std::string& name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;

    std::string target = name;
    std::transform(target.begin(), target.end(), target.begin(), ::tolower);

    if (Process32First(snap, &pe)) {
        do {
            std::string pname = pe.szExeFile;
            std::transform(pname.begin(), pname.end(), pname.begin(), ::tolower);
            if (pname == target) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }

    CloseHandle(snap);
    return pid;
}

std::string Injector::GetProcessName(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return "";

    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(pe);
    std::string name;

    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                name = pe.szExeFile;
                break;
            }
        } while (Process32Next(snap, &pe));
    }

    CloseHandle(snap);
    return name;
}

std::string Injector::GetWindowTitle(DWORD pid) {
    // Encontra a janela principal associada ao PID
    struct EnumData { DWORD pid; HWND hwnd; };
    EnumData data = { pid, nullptr };

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* d = reinterpret_cast<EnumData*>(lp);
        DWORD wpid = 0;
        GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid == d->pid && IsWindowVisible(hwnd)) {
            d->hwnd = hwnd;
            return FALSE; // para a enumeração
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));

    if (!data.hwnd) return "";
    char title[256] = {};
    GetWindowTextA(data.hwnd, title, sizeof(title));
    return title;
}

bool Injector::IsInjected(DWORD pid, const std::string& dll_name) {
    HANDLE hProc = OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProc) return false;

    HMODULE modules[1024] = {};
    DWORD needed = 0;
    bool found = false;

    if (EnumProcessModules(hProc, modules, sizeof(modules), &needed)) {
        DWORD count = needed / sizeof(HMODULE);
        char mod_name[MAX_PATH] = {};
        std::string target = dll_name;
        std::transform(target.begin(), target.end(), target.begin(), ::tolower);

        for (DWORD i = 0; i < count && !found; ++i) {
            GetModuleFileNameExA(hProc, modules[i], mod_name, MAX_PATH);
            std::string mname = mod_name;
            size_t s = mname.find_last_of("\\/");
            if (s != std::string::npos) mname = mname.substr(s + 1);
            std::transform(mname.begin(), mname.end(), mname.begin(), ::tolower);
            if (mname == target) found = true;
        }
    }

    CloseHandle(hProc);
    return found;
}

std::vector<ProcessInfo> Injector::ListProcesses() {
    std::vector<ProcessInfo> list;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return list;

    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(pe);

    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == 4) continue;
            ProcessInfo info;
            info.pid    = pe.th32ProcessID;
            info.name   = pe.szExeFile;
            info.window = GetWindowTitle(pe.th32ProcessID);
            list.push_back(info);
        } while (Process32Next(snap, &pe));
    }

    CloseHandle(snap);

    std::sort(list.begin(), list.end(),
        [](const ProcessInfo& a, const ProcessInfo& b) {
            return a.name < b.name;
        });

    return list;
}

// ─────────────────────────────────────────────────────────────────────────────
// Log
// ─────────────────────────────────────────────────────────────────────────────

void Injector::Log(const std::string& msg) const {
    if (OnLog) {
        OnLog(msg);
    } else {
        printf("[Injector] %s\n", msg.c_str());
    }
}

const char* Injector::StatusToString(InjectStatus s) {
    switch (s) {
        case InjectStatus::Success:         return "Success";
        case InjectStatus::ProcessNotFound: return "ProcessNotFound";
        case InjectStatus::AccessDenied:    return "AccessDenied";
        case InjectStatus::DLLNotFound:     return "DLLNotFound";
        case InjectStatus::AllocFailed:     return "AllocFailed";
        case InjectStatus::WriteFailed:     return "WriteFailed";
        case InjectStatus::ThreadFailed:    return "ThreadFailed";
        case InjectStatus::ThreadTimeout:   return "ThreadTimeout";
        case InjectStatus::AlreadyInjected: return "AlreadyInjected";
        default:                            return "UnknownError";
    }
}
