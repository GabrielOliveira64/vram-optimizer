#pragma once

/*
 * IPCProtocol.h
 * ─────────────────────────────────────────────────────────────────────────────
 * Protocolo de comunicação entre o Hook DX11 e o Orchestrator.
 * Canal: Named Pipe bidirecional.
 *
 * CORREÇÃO: windows.h deve ser incluído ANTES de qualquer uso de DWORD.
 * O constexpr DWORD foi substituído por uint32_t para evitar conflito
 * quando este header é incluído antes do windows.h em alguma unidade.
 */

// windows.h PRIMEIRO — garante que DWORD e demais tipos Win32 já existam
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>

// ─── Nomes dos pipes ──────────────────────────────────────────────────────────

constexpr char     PIPE_EVENTS[]      = "\\\\.\\pipe\\vram_opt_events";
constexpr char     PIPE_COMMANDS[]    = "\\\\.\\pipe\\vram_opt_cmds";
constexpr uint32_t PIPE_TIMEOUT_MS   = 2000;   // uint32_t em vez de DWORD

// ─── Tipos de mensagem ────────────────────────────────────────────────────────

enum class EventType : uint32_t {
    TextureCreated   = 1,
    TextureReleased  = 2,
    FrameEnd         = 3,
    VRAMSnapshot     = 4,
};

enum class CommandType : uint32_t {
    CompressTexture  = 1,
    RestoreTexture   = 2,
    SetMaxDimension  = 3,
    Ping             = 4,
};

enum class IPCCompressionLevel : uint32_t {
    None       = 0,
    BC1        = 1,
    BC3        = 2,
    BC7        = 3,
    HalfRes    = 4,
    QuarterRes = 5,
};

enum class IPCFormat : uint32_t {
    Unknown          = 0,
    RGBA8_UNORM      = 1,
    RGBA8_UNORM_SRGB = 2,
    RGBA16_FLOAT     = 3,
    RGBA32_FLOAT     = 4,
    BC1_UNORM        = 5,
    BC3_UNORM        = 6,
    BC7_UNORM        = 7,
    Depth            = 99,
    Other            = 100,
};

// ─── Structs POD para transmissão direta via pipe ─────────────────────────────

#pragma pack(push, 1)

struct EventHeader {
    EventType type;
    uint32_t  payload_size;
};

struct TextureCreatedEvent {
    uint64_t  texture_id;
    uint32_t  width;
    uint32_t  height;
    uint32_t  mip_levels;
    uint32_t  array_size;
    IPCFormat format;
    uint32_t  bind_flags;
    uint64_t  size_bytes;
};

struct TextureReleasedEvent {
    uint64_t texture_id;
};

struct VRAMSnapshotEvent {
    uint64_t used_bytes;
    uint64_t budget_bytes;
    uint32_t frame_number;
};

struct CommandHeader {
    CommandType type;
    uint32_t    payload_size;
};

struct CompressTextureCommand {
    uint64_t            texture_id;
    IPCCompressionLevel level;
    uint32_t            target_width;
    uint32_t            target_height;
};

struct SetMaxDimensionCommand {
    uint32_t max_dimension;
};

#pragma pack(pop)

// ─── Converte DXGI_FORMAT → IPCFormat (só disponível se d3d11.h já foi incluído)

#ifdef __d3d11_h__
#include <dxgi.h>
inline IPCFormat DXGIToIPCFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:       return IPCFormat::RGBA8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:  return IPCFormat::RGBA8_UNORM_SRGB;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:   return IPCFormat::RGBA16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:   return IPCFormat::RGBA32_FLOAT;
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:       return IPCFormat::BC1_UNORM;
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:       return IPCFormat::BC3_UNORM;
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:       return IPCFormat::BC7_UNORM;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D16_UNORM:            return IPCFormat::Depth;
        default:                               return IPCFormat::Other;
    }
}
#endif