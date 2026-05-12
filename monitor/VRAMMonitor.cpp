/*
 * VRAMMonitor.cpp
 * ─────────────────────────────────────────────────────────────────────────────
 * Backend NVAPI (opcional) + DXGI 1.4 (padrão).
 * Sem d3dkmthk.h — compatível com Windows SDK 10.0.26100+.
 */

#include "VRAMMonitor.h"

// windows.h já veio do header; incluímos DXGI diretamente
#include <dxgi.h>
#include <dxgi1_4.h>    // IDXGIAdapter3::QueryVideoMemoryInfo
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>

#pragma comment(lib, "dxgi.lib")

#ifdef VRAM_USE_NVAPI
#   include <nvapi.h>
#   pragma comment(lib, "nvapi64.lib")
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Construtor / Destrutor
// ─────────────────────────────────────────────────────────────────────────────

VRAMMonitor::VRAMMonitor(float threshold_percent, int gpu_index)
    : m_threshold(threshold_percent)
    , m_gpu_index(gpu_index)
{
#ifdef VRAM_USE_NVAPI
    if (InitNVAPI()) { m_backend = Backend::NVAPI; return; }
#endif
    if (InitDXGI())  { return; } // InitDXGI define m_backend internamente

    fprintf(stderr, "[VRAMMonitor] Nenhum backend disponivel.\n");
}

VRAMMonitor::~VRAMMonitor() {
    Stop();

#ifdef VRAM_USE_NVAPI
    if (m_backend == Backend::NVAPI) {
        NvAPI_Unload();
    }
#endif

    if (m_dxgi_adapter) {
        reinterpret_cast<IDXGIAdapter*>(m_dxgi_adapter)->Release();
        m_dxgi_adapter = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// InitNVAPI
// ─────────────────────────────────────────────────────────────────────────────

bool VRAMMonitor::InitNVAPI() {
#ifdef VRAM_USE_NVAPI
    if (NvAPI_Initialize() != NVAPI_OK) return false;

    NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS] = {};
    NvU32 count = 0;
    if (NvAPI_EnumPhysicalGPUs(handles, &count) != NVAPI_OK) return false;
    if (m_gpu_index >= (int)count) return false;

    m_nvapi_handle = handles[m_gpu_index];
    printf("[VRAMMonitor] Backend: NVAPI (GPU %d)\n", m_gpu_index);
    return true;
#else
    return false;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// InitDXGI — tenta DXGI 1.4 (QueryVideoMemoryInfo), cai para DXGI 1.0
// ─────────────────────────────────────────────────────────────────────────────

bool VRAMMonitor::InitDXGI() {
    // Tenta criar IDXGIFactory2 (DXGI 1.2+) para chegar ao IDXGIAdapter3
    IDXGIFactory2* factory2 = nullptr;
    IDXGIFactory*  factory1 = nullptr;
    IDXGIAdapter*  adapter  = nullptr;

    // Tenta DXGI 1.2+ primeiro
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&factory2);
    if (SUCCEEDED(hr)) {
        IDXGIAdapter1* adapter1 = nullptr;
        hr = factory2->EnumAdapters1((UINT)m_gpu_index, &adapter1);
        factory2->Release();

        if (SUCCEEDED(hr)) {
            // Tenta promover para IDXGIAdapter3
            IDXGIAdapter3* adapter3 = nullptr;
            if (SUCCEEDED(adapter1->QueryInterface(__uuidof(IDXGIAdapter3),
                                                   (void**)&adapter3)))
            {
                m_dxgi_adapter = adapter3;  // guarda como IDXGIAdapter3
                m_dxgi14       = true;
                adapter1->Release();

                DXGI_ADAPTER_DESC desc = {};
                adapter3->GetDesc(&desc);
                printf("[VRAMMonitor] Backend: DXGI 1.4 (GPU %d — %S)\n",
                       m_gpu_index, desc.Description);
                m_backend = Backend::DXGI14;
                return true;
            }
            adapter = adapter1; // fallback para DXGI 1.0
        }
    }

    // Fallback: DXGI 1.0
    if (!adapter) {
        hr = CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory1);
        if (FAILED(hr)) return false;
        hr = factory1->EnumAdapters((UINT)m_gpu_index, &adapter);
        factory1->Release();
        if (FAILED(hr)) return false;
    }

    m_dxgi_adapter = adapter;
    m_dxgi14       = false;
    m_backend      = Backend::DXGI10;

    DXGI_ADAPTER_DESC desc = {};
    adapter->GetDesc(&desc);
    printf("[VRAMMonitor] Backend: DXGI 1.0 fallback (GPU %d — %S)\n",
           m_gpu_index, desc.Description);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReadNVAPI
// ─────────────────────────────────────────────────────────────────────────────

bool VRAMMonitor::ReadNVAPI(VRAMInfo& out) const {
#ifdef VRAM_USE_NVAPI
    auto handle = reinterpret_cast<NvPhysicalGpuHandle>(m_nvapi_handle);
    NV_DISPLAY_DRIVER_MEMORY_INFO mem = {};
    mem.version = NV_DISPLAY_DRIVER_MEMORY_INFO_VER;
    if (NvAPI_GPU_GetMemoryInfo(handle, &mem) != NVAPI_OK) return false;

    out.total_bytes = (uint64_t)mem.dedicatedVideoMemory * 1024ULL;
    out.free_bytes  = (uint64_t)mem.curAvailableDedicatedVideoMemory * 1024ULL;
    out.used_bytes  = out.total_bytes - out.free_bytes;
    out.percent     = out.total_bytes > 0
                      ? (float)out.used_bytes / out.total_bytes * 100.f : 0.f;
    out.is_critical = out.percent >= m_threshold;
    strcpy_s(out.backend, "NVAPI");
    return true;
#else
    return false;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// ReadDXGI — usa DXGI 1.4 se disponível, senão DXGI 1.0 (só total)
// ─────────────────────────────────────────────────────────────────────────────

bool VRAMMonitor::ReadDXGI(VRAMInfo& out) const {
    if (!m_dxgi_adapter) return false;

    // Pega o total real de VRAM dedicada via GetDesc() — sempre confiável
    DXGI_ADAPTER_DESC desc = {};
    if (m_dxgi14) {
        auto* a3 = reinterpret_cast<IDXGIAdapter3*>(m_dxgi_adapter);
        a3->GetDesc(&desc);
    } else {
        auto* a = reinterpret_cast<IDXGIAdapter*>(m_dxgi_adapter);
        a->GetDesc(&desc);
    }
    uint64_t dedicated = desc.DedicatedVideoMemory;

    if (m_dxgi14) {
        auto* adapter3 = reinterpret_cast<IDXGIAdapter3*>(m_dxgi_adapter);
        DXGI_QUERY_VIDEO_MEMORY_INFO local_info = {};
        HRESULT hr = adapter3->QueryVideoMemoryInfo(
            0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local_info);

        if (SUCCEEDED(hr)) {
            // Budget pode ser menor que DedicatedVideoMemory se o sistema
            // reservou parte. Usa o maior entre os dois como "total visível".
            uint64_t budget = local_info.Budget;
            out.total_bytes = (dedicated > budget && dedicated > 0)
                              ? dedicated : (budget > 0 ? budget : dedicated);
            out.used_bytes  = local_info.CurrentUsage;
            out.free_bytes  = out.total_bytes > out.used_bytes
                              ? out.total_bytes - out.used_bytes : 0;
            out.percent     = out.total_bytes > 0
                              ? (float)out.used_bytes / out.total_bytes * 100.f
                              : 0.f;
            out.is_critical = out.percent >= m_threshold;
            strcpy_s(out.backend, "DXGI 1.4");
            return true;
        }
    }

    // DXGI 1.0 fallback — reporta total mas não tem uso atual
    out.total_bytes = dedicated;
    out.used_bytes  = 0;
    out.free_bytes  = dedicated;
    out.percent     = 0.f;
    out.is_critical = false;
    strcpy_s(out.backend, "DXGI 1.0");
    return dedicated > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// API pública
// ─────────────────────────────────────────────────────────────────────────────

bool VRAMMonitor::Read(VRAMInfo& out) const {
    switch (m_backend) {
        case Backend::NVAPI:  return ReadNVAPI(out);
        case Backend::DXGI14: return ReadDXGI(out);
        case Backend::DXGI10: return ReadDXGI(out);
        default:              return false;
    }
}

void VRAMMonitor::Watch(VRAMCallback cb, uint32_t interval_ms) {
    m_running = true;
    while (m_running) {
        VRAMInfo info = {};
        if (Read(info)) cb(info);
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
}

void VRAMMonitor::Stop() {
    m_running = false;
}

const char* VRAMMonitor::ActiveBackend() const {
    switch (m_backend) {
        case Backend::NVAPI:  return "NVAPI";
        case Backend::DXGI14: return "DXGI 1.4";
        case Backend::DXGI10: return "DXGI 1.0";
        default:              return "None";
    }
}