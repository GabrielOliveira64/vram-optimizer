#pragma once

/*
 * VRAMMonitor.h
 * ─────────────────────────────────────────────────────────────────────────────
 * Lê o uso de VRAM em tempo real.
 * Suporta três backends, detectados automaticamente na ordem:
 *   1. NVAPI  → GPUs Nvidia  (mais preciso, inclui memória compartilhada)
 *   2. D3DKMT → Qualquer GPU via Windows Kernel (funciona para AMD/Intel também)
 *   3. DXGI   → Fallback universal via IDXGIAdapter (menos granular)
 *
 * Dependências externas:
 *   - NVAPI (opcional): baixe o SDK em https://developer.nvidia.com/nvapi
 *     e defina VRAM_USE_NVAPI=1 nas flags do compilador.
 *   - D3DKMT: já incluso no Windows SDK (d3dkmthk.h)
 *   - DXGI:   já incluso no Windows SDK (dxgi.h)
 *
 * Compilar (sem NVAPI):
 *   cl /std:c++17 VRAMMonitor.cpp /link dxgi.lib
 *
 * Compilar (com NVAPI):
 *   cl /std:c++17 /DVRAM_USE_NVAPI=1 VRAMMonitor.cpp /link dxgi.lib nvapi64.lib
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <string>
#include <functional>

// ─── Estruturas públicas ──────────────────────────────────────────────────────

struct VRAMInfo {
    uint64_t used_bytes   = 0;   // Bytes atualmente em uso
    uint64_t total_bytes  = 0;   // Capacidade total da VRAM
    uint64_t free_bytes   = 0;   // Bytes livres
    float    percent      = 0.f; // Percentual de uso (0–100)
    bool     is_critical  = false;
    char     backend[32]  = {};  // "NVAPI" | "D3DKMT" | "DXGI"
};

// Callback chamado a cada leitura no loop Watch()
using VRAMCallback = std::function<void(const VRAMInfo&)>;

// ─── Classe principal ─────────────────────────────────────────────────────────

class VRAMMonitor {
public:
    // threshold_percent: acima disto, VRAMInfo::is_critical = true
    explicit VRAMMonitor(float threshold_percent = 80.f, int gpu_index = 0);
    ~VRAMMonitor();

    // Leitura única — retorna false se nenhum backend disponível
    bool Read(VRAMInfo& out) const;

    // Loop de monitoramento contínuo. Chama cb a cada interval_ms.
    // Bloqueante — rode em thread separada se necessário.
    // Retorna quando stop_token for sinalizado ou cb retornar false.
    void Watch(VRAMCallback cb, uint32_t interval_ms = 500);

    // Para o loop Watch() a partir de outra thread
    void Stop();

    // Qual backend está ativo
    const char* ActiveBackend() const;

    // Desabilita cópia
    VRAMMonitor(const VRAMMonitor&)            = delete;
    VRAMMonitor& operator=(const VRAMMonitor&) = delete;

private:
    enum class Backend { None, NVAPI, D3DKMT, DXGI };

    bool InitNVAPI();
    bool InitD3DKMT();
    bool InitDXGI();

    bool ReadNVAPI (VRAMInfo& out) const;
    bool ReadD3DKMT(VRAMInfo& out) const;
    bool ReadDXGI  (VRAMInfo& out) const;

    Backend  m_backend       = Backend::None;
    float    m_threshold     = 80.f;
    int      m_gpu_index     = 0;
    volatile bool m_running  = false;

    // Handles internos — detalhes em VRAMMonitor.cpp
    void*    m_nvapi_handle  = nullptr;   // NvPhysicalGpuHandle (NVAPI)
    void*    m_dxgi_adapter  = nullptr;   // IDXGIAdapter* (DXGI)
    uint32_t m_d3dkmt_handle = 0;         // D3DKMT_HANDLE (D3DKMT)
};
