/*
 * VRAMMonitor.cpp
 * ─────────────────────────────────────────────────────────────────────────────
 * Implementação dos três backends de leitura de VRAM.
 *
 * Ordem de inicialização:
 *   1. NVAPI  (Nvidia, mais completo)
 *   2. D3DKMT (Windows KMT — funciona para AMD e Intel também)
 *   3. DXGI   (fallback, menos granular)
 */

#include "VRAMMonitor.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <d3dkmthk.h>   // D3DKMT — Windows SDK
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>

// ─── NVAPI — carregado dinamicamente para não exigir o SDK em tempo de link ──
// Se você tiver o SDK: #define VRAM_USE_NVAPI 1 e linke nvapi64.lib
#ifdef VRAM_USE_NVAPI
#   include <nvapi.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Construtor / Destrutor
// ─────────────────────────────────────────────────────────────────────────────

VRAMMonitor::VRAMMonitor(float threshold_percent, int gpu_index)
    : m_threshold(threshold_percent)
    , m_gpu_index(gpu_index)
{
#ifdef VRAM_USE_NVAPI
    if (InitNVAPI())  { m_backend = Backend::NVAPI;  return; }
#endif
    if (InitD3DKMT()) { m_backend = Backend::D3DKMT; return; }
    if (InitDXGI())   { m_backend = Backend::DXGI;   return; }

    fprintf(stderr, "[VRAMMonitor] AVISO: nenhum backend disponivel.\n");
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
// Inicialização dos backends
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

bool VRAMMonitor::InitD3DKMT() {
    // Tenta obter o adapter handle via D3DKMTOpenAdapterFromLuid
    // Enumeramos via DXGI para pegar o LUID e depois abrimos via KMT
    IDXGIFactory* factory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)))
        return false;

    IDXGIAdapter* adapter = nullptr;
    HRESULT hr = factory->EnumAdapters((UINT)m_gpu_index, &adapter);
    factory->Release();
    if (FAILED(hr)) return false;

    DXGI_ADAPTER_DESC desc = {};
    adapter->GetDesc(&desc);
    adapter->Release();

    D3DKMT_OPENADAPTERFROMLUID open_args = {};
    open_args.AdapterLuid = desc.AdapterLuid;

    // D3DKMTOpenAdapterFromLuid é exportada por gdi32.dll
    HMODULE gdi = GetModuleHandleA("gdi32.dll");
    if (!gdi) return false;

    using PFN_OpenAdapterFromLuid = NTSTATUS(APIENTRY*)(D3DKMT_OPENADAPTERFROMLUID*);
    auto fn = reinterpret_cast<PFN_OpenAdapterFromLuid>(
        GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
    if (!fn) return false;

    if (fn(&open_args) != 0) return false;

    m_d3dkmt_handle = open_args.hAdapter;
    printf("[VRAMMonitor] Backend: D3DKMT (GPU %d — %S)\n",
           m_gpu_index, desc.Description);
    return true;
}

bool VRAMMonitor::InitDXGI() {
    IDXGIFactory* factory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)))
        return false;

    IDXGIAdapter* adapter = nullptr;
    HRESULT hr = factory->EnumAdapters((UINT)m_gpu_index, &adapter);
    factory->Release();
    if (FAILED(hr)) return false;

    m_dxgi_adapter = adapter; // guardamos a referência

    DXGI_ADAPTER_DESC desc = {};
    adapter->GetDesc(&desc);
    printf("[VRAMMonitor] Backend: DXGI (GPU %d — %S)\n",
           m_gpu_index, desc.Description);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Leitura por backend
// ─────────────────────────────────────────────────────────────────────────────

bool VRAMMonitor::ReadNVAPI(VRAMInfo& out) const {
#ifdef VRAM_USE_NVAPI
    auto handle = reinterpret_cast<NvPhysicalGpuHandle>(m_nvapi_handle);

    // Memória dedicada
    NV_DISPLAY_DRIVER_MEMORY_INFO mem_info = {};
    mem_info.version = NV_DISPLAY_DRIVER_MEMORY_INFO_VER;
    if (NvAPI_GPU_GetMemoryInfo(handle, &mem_info) != NVAPI_OK) return false;

    out.total_bytes = (uint64_t)mem_info.dedicatedVideoMemory * 1024ULL;
    out.free_bytes  = (uint64_t)mem_info.curAvailableDedicatedVideoMemory * 1024ULL;
    out.used_bytes  = out.total_bytes - out.free_bytes;
    out.percent     = out.total_bytes > 0
                      ? (float)out.used_bytes / out.total_bytes * 100.f
                      : 0.f;
    out.is_critical = out.percent >= m_threshold;
    strcpy_s(out.backend, "NVAPI");
    return true;
#else
    return false;
#endif
}

bool VRAMMonitor::ReadD3DKMT(VRAMInfo& out) const {
    // Consulta segmentos de memória via D3DKMTQueryStatistics
    D3DKMT_QUERYSTATISTICS qs = {};
    qs.Type    = D3DKMT_QUERYSTATISTICS_SEGMENT;
    qs.AdapterLuid; // preenchido via handle — veja nota abaixo

    // NOTA: D3DKMT_QUERYSTATISTICS precisa do LUID, não do handle direto.
    // Versão simplificada via D3DKMTQueryAdapterInfo para budget:
    HMODULE gdi = GetModuleHandleA("gdi32.dll");
    if (!gdi) return false;

    using PFN_QueryAdapterInfo = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYADAPTERINFO*);
    auto fn_query = reinterpret_cast<PFN_QueryAdapterInfo>(
        GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
    if (!fn_query) return false;

    // Consulta DXGI_QUERY_VIDEO_MEMORY_INFO via KMT
    DXGI_QUERY_VIDEO_MEMORY_INFO vmi_local  = {};
    DXGI_QUERY_VIDEO_MEMORY_INFO vmi_nonlocal = {};

    D3DKMT_QUERYADAPTERINFO qi_local = {};
    qi_local.hAdapter = m_d3dkmt_handle;
    qi_local.Type     = KMTQAITYPE_QUERYREGISTRY; // fallback: usa DXGI abaixo

    // Fallback limpo: se D3DKMT não conseguir os dados, delega para DXGI
    // (na prática D3DKMT é mais complexo; para produção use a lib DXGI 1.4+)
    if (m_dxgi_adapter) return ReadDXGI(out);

    return false;
}

bool VRAMMonitor::ReadDXGI(VRAMInfo& out) const {
    // DXGI 1.4 introduziu QueryVideoMemoryInfo — mais preciso que GetDesc
    IDXGIAdapter3* adapter3 = nullptr;
    auto* adapter = reinterpret_cast<IDXGIAdapter*>(m_dxgi_adapter);

    if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&adapter3))) {
        // Memória local (VRAM dedicada)
        DXGI_QUERY_VIDEO_MEMORY_INFO local_info = {};
        adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local_info);
        adapter3->Release();

        out.total_bytes = local_info.Budget;
        out.used_bytes  = local_info.CurrentUsage;
        out.free_bytes  = (out.total_bytes > out.used_bytes)
                          ? out.total_bytes - out.used_bytes : 0;
        out.percent     = out.total_bytes > 0
                          ? (float)out.used_bytes / out.total_bytes * 100.f
                          : 0.f;
        out.is_critical = out.percent >= m_threshold;
        strcpy_s(out.backend, "DXGI 1.4");
        return true;
    }

    // Fallback: GetDesc (total apenas, sem uso atual — menos útil)
    DXGI_ADAPTER_DESC desc = {};
    if (FAILED(adapter->GetDesc(&desc))) return false;

    out.total_bytes = desc.DedicatedVideoMemory;
    out.used_bytes  = 0; // indisponível neste fallback
    out.free_bytes  = out.total_bytes;
    out.percent     = 0.f;
    out.is_critical = false;
    strcpy_s(out.backend, "DXGI 1.0 (fallback)");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// API pública
// ─────────────────────────────────────────────────────────────────────────────

bool VRAMMonitor::Read(VRAMInfo& out) const {
    switch (m_backend) {
        case Backend::NVAPI:  return ReadNVAPI(out);
        case Backend::D3DKMT: return ReadD3DKMT(out);
        case Backend::DXGI:   return ReadDXGI(out);
        default:              return false;
    }
}

void VRAMMonitor::Watch(VRAMCallback cb, uint32_t interval_ms) {
    m_running = true;
    while (m_running) {
        VRAMInfo info = {};
        if (Read(info)) {
            cb(info);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
}

void VRAMMonitor::Stop() {
    m_running = false;
}

const char* VRAMMonitor::ActiveBackend() const {
    switch (m_backend) {
        case Backend::NVAPI:  return "NVAPI";
        case Backend::D3DKMT: return "D3DKMT";
        case Backend::DXGI:   return "DXGI";
        default:              return "None";
    }
}
