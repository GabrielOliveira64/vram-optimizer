#pragma once

/*
 * IPCProtocol.h
 * ─────────────────────────────────────────────────────────────────────────────
 * Protocolo de comunicação entre o Hook DX11 (injetado no jogo)
 * e o Orchestrator (processo externo).
 *
 * Canal: Named Pipe bidirecional
 *   Pipe de eventos  (hook → orquestrador): "\\.\\pipe\\vram_opt_events"
 *   Pipe de comandos (orquestrador → hook): "\\.\\pipe\\vram_opt_cmds"
 *
 * Todas as structs são POD (plain old data) para transmissão direta via pipe.
 * Nenhuma dependência de runtime além de <windows.h>.
 */

#include <cstdint>

// ─── Nomes dos pipes ──────────────────────────────────────────────────────────

constexpr char PIPE_EVENTS[]   = "\\\\.\\pipe\\vram_opt_events";
constexpr char PIPE_COMMANDS[] = "\\\\.\\pipe\\vram_opt_cmds";
constexpr DWORD PIPE_TIMEOUT_MS = 2000;

// ─── Tipos de mensagem ────────────────────────────────────────────────────────

enum class EventType : uint32_t {
    TextureCreated   = 1,   // Hook → Orquestrador: nova textura detectada
    TextureReleased  = 2,   // Hook → Orquestrador: textura destruída
    FrameEnd         = 3,   // Hook → Orquestrador: fim de frame (para stats)
    VRAMSnapshot     = 4,   // Hook → Orquestrador: leitura de VRAM do próprio processo
};

enum class CommandType : uint32_t {
    CompressTexture  = 1,   // Orquestrador → Hook: comprime textura X com nível Y
    RestoreTexture   = 2,   // Orquestrador → Hook: restaura qualidade original
    SetMaxDimension  = 3,   // Orquestrador → Hook: muda limite global de resolução
    Ping             = 4,   // Orquestrador → Hook: verifica se o hook está vivo
};

enum class IPCCompressionLevel : uint32_t {
    None       = 0,
    BC1        = 1,
    BC3        = 2,
    BC7        = 3,
    HalfRes    = 4,
    QuarterRes = 5,
};

// ─── Formatos DXGI simplificados (subconjunto relevante) ─────────────────────

enum class IPCFormat : uint32_t {
    Unknown            = 0,
    RGBA8_UNORM        = 1,
    RGBA8_UNORM_SRGB   = 2,
    RGBA16_FLOAT       = 3,
    RGBA32_FLOAT       = 4,
    BC1_UNORM          = 5,
    BC3_UNORM          = 6,
    BC7_UNORM          = 7,
    Depth              = 99,  // Depth/stencil — não comprimimos
    Other              = 100,
};

// ─── Mensagens: Hook → Orquestrador ──────────────────────────────────────────

#pragma pack(push, 1)

struct EventHeader {
    EventType type;
    uint32_t  payload_size;  // bytes do payload após este header
};

struct TextureCreatedEvent {
    uint64_t  texture_id;    // ponteiro do recurso DX11 (cast para uint64_t)
    uint32_t  width;
    uint32_t  height;
    uint32_t  mip_levels;
    uint32_t  array_size;
    IPCFormat format;
    uint32_t  bind_flags;    // D3D11_BIND_FLAG original
    uint64_t  size_bytes;    // estimativa calculada pelo hook
};

struct TextureReleasedEvent {
    uint64_t texture_id;
};

struct VRAMSnapshotEvent {
    uint64_t used_bytes;
    uint64_t budget_bytes;
    uint32_t frame_number;
};

// ─── Mensagens: Orquestrador → Hook ──────────────────────────────────────────

struct CommandHeader {
    CommandType type;
    uint32_t    payload_size;
};

struct CompressTextureCommand {
    uint64_t              texture_id;
    IPCCompressionLevel   level;
    uint32_t              target_width;   // 0 = manter proporção
    uint32_t              target_height;  // 0 = manter proporção
};

struct SetMaxDimensionCommand {
    uint32_t max_dimension;
};

#pragma pack(pop)

// ─── Utilitário: converte formato DXGI para IPCFormat ────────────────────────
// (Implementação inline — usada tanto pelo hook quanto pelo orquestrador)

#ifdef __d3d11_h__
#include <dxgi.h>
inline IPCFormat DXGIToIPCFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:      return IPCFormat::RGBA8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return IPCFormat::RGBA8_UNORM_SRGB;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:  return IPCFormat::RGBA16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:  return IPCFormat::RGBA32_FLOAT;
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:      return IPCFormat::BC1_UNORM;
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:      return IPCFormat::BC3_UNORM;
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:      return IPCFormat::BC7_UNORM;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D16_UNORM:            return IPCFormat::Depth;
        default:                               return IPCFormat::Other;
    }
}
#endif
