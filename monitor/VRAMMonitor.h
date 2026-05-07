#pragma once

/*
 * VRAMMonitor.h
 * ─────────────────────────────────────────────────────────────────────────────
 * Lê o uso de VRAM em tempo real.
 *
 * Backends suportados (detectados automaticamente):
 *   1. NVAPI  → GPUs Nvidia (mais preciso). Requer VRAM_USE_NVAPI=1 + SDK.
 *   2. DXGI 1.4 → Qualquer GPU, Windows 10+. Padrão sem deps extras.
 *   3. DXGI 1.0 → Fallback (só reporta total, não uso atual).
 *
 * NOTA: O backend D3DKMT foi removido por incompatibilidade com o
 * Windows SDK 10.0.26100+ (d3dkmthk.h conflita com headers recentes).
 * O DXGI 1.4 cobre o mesmo caso de uso com boa precisão.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <functional>

// ─── Estruturas públicas ──────────────────────────────────────────────────────

struct VRAMInfo {
    uint64_t used_bytes  = 0;
    uint64_t total_bytes = 0;
    uint64_t free_bytes  = 0;
    float    percent     = 0.f;
    bool     is_critical = false;
    char     backend[32] = {};
};

using VRAMCallback = std::function<void(const VRAMInfo&)>;

// ─── Classe principal ─────────────────────────────────────────────────────────

class VRAMMonitor {
public:
    explicit VRAMMonitor(float threshold_percent = 80.f, int gpu_index = 0);
    ~VRAMMonitor();

    bool        Read(VRAMInfo& out) const;
    void        Watch(VRAMCallback cb, uint32_t interval_ms = 500);
    void        Stop();
    const char* ActiveBackend() const;

    VRAMMonitor(const VRAMMonitor&)            = delete;
    VRAMMonitor& operator=(const VRAMMonitor&) = delete;

private:
    enum class Backend { None, NVAPI, DXGI14, DXGI10 };

    bool InitNVAPI();
    bool InitDXGI();

    bool ReadNVAPI(VRAMInfo& out) const;
    bool ReadDXGI (VRAMInfo& out) const;

    Backend      m_backend      = Backend::None;
    float        m_threshold    = 80.f;
    int          m_gpu_index    = 0;
    volatile bool m_running     = false;

    void*    m_nvapi_handle = nullptr;  // NvPhysicalGpuHandle (NVAPI)
    void*    m_dxgi_adapter = nullptr;  // IDXGIAdapter*
    bool     m_dxgi14       = false;    // true = DXGI 1.4 disponível
};