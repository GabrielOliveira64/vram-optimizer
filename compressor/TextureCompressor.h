#pragma once

/*
 * TextureCompressor.h
 * ─────────────────────────────────────────────────────────────────────────────
 * Módulo 4: Compressão real de pixels de textura.
 *
 * Responsabilidades:
 *   1. Receber pixels RGBA8 brutos de uma textura mapeada da GPU
 *   2. Executar compressão para BC1 / BC3 / BC7 (via ispc_texcomp ou software)
 *   3. Executar downscale bilinear (HalfRes / QuarterRes) com filtro de qualidade
 *   4. Retornar buffer comprimido pronto para criar nova ID3D11Texture2D
 *
 * Backends de compressão:
 *   - VRAM_USE_ISPC definido → ispc_texcomp (Intel, muito rápido com SIMD)
 *   - Sem ISPC              → compressor software incluído aqui (mais lento,
 *                             zero dependências, suficiente para testes)
 *
 * Uso típico (dentro do hook DX11, após receber comando do orquestrador):
 *
 *   // 1. Mapeia textura original para leitura em CPU
 *   D3D11_MAPPED_SUBRESOURCE mapped = {};
 *   pContext->Map(pOriginalTex, 0, D3D11_MAP_READ, 0, &mapped);
 *
 *   // 2. Comprime
 *   TextureCompressor comp;
 *   CompressResult result = comp.CompressBC1(
 *       (const uint8_t*)mapped.pData,
 *       desc.Width, desc.Height,
 *       mapped.RowPitch);
 *
 *   pContext->Unmap(pOriginalTex, 0);
 *
 *   // 3. Cria nova textura com dados comprimidos
 *   D3D11_SUBRESOURCE_DATA init = {};
 *   init.pSysMem     = result.data.data();
 *   init.SysMemPitch = result.row_pitch;
 *   pDevice->CreateTexture2D(&new_desc, &init, &pNewTex);
 */

#include <cstdint>
#include <vector>
#include <functional>

// ─── Resultado de uma operação de compressão ─────────────────────────────────

struct CompressResult {
    std::vector<uint8_t> data;        // Dados comprimidos prontos para DX11
    uint32_t             row_pitch;   // Bytes por linha (para SysMemPitch)
    uint32_t             width;       // Largura após processamento
    uint32_t             height;      // Altura após processamento
    uint64_t             size_bytes;  // Total de bytes
    bool                 success;
    char                 error[128];  // Mensagem de erro, se success=false
};

// Callback de progresso para operações longas (0.0 a 1.0)
using ProgressCallback = std::function<void(float progress)>;

// ─── Compressor ───────────────────────────────────────────────────────────────

class TextureCompressor {
public:
    TextureCompressor()  = default;
    ~TextureCompressor() = default;

    // ── Compressão BC1 (RGB, sem alpha, ~6:1 vs RGBA8) ───────────────────────
    // Melhor para: difuse maps, skyboxes, terreno
    CompressResult CompressBC1(
        const uint8_t* rgba_pixels,
        uint32_t width, uint32_t height,
        uint32_t src_row_pitch,
        ProgressCallback progress_cb = nullptr);

    // ── Compressão BC3 (RGBA, com alpha, ~4:1 vs RGBA8) ──────────────────────
    // Melhor para: texturas com transparência (folhagens, janelas)
    CompressResult CompressBC3(
        const uint8_t* rgba_pixels,
        uint32_t width, uint32_t height,
        uint32_t src_row_pitch,
        ProgressCallback progress_cb = nullptr);

    // ── Compressão BC7 (RGBA, alta qualidade, ~8:1 vs RGBA8) ─────────────────
    // Melhor para: personagens principais, objetos em destaque
    // Mais lento que BC1/BC3 — use apenas quando qualidade é crítica
    CompressResult CompressBC7(
        const uint8_t* rgba_pixels,
        uint32_t width, uint32_t height,
        uint32_t src_row_pitch,
        ProgressCallback progress_cb = nullptr);

    // ── Downscale bilinear ────────────────────────────────────────────────────
    // Reduz para target_w × target_h com filtro bilinear
    // Retorna buffer RGBA8 na nova resolução (não comprimido)
    CompressResult Downscale(
        const uint8_t* rgba_pixels,
        uint32_t src_width, uint32_t src_height,
        uint32_t src_row_pitch,
        uint32_t target_width, uint32_t target_height);

    // ── Downscale + Compressão em um passo ───────────────────────────────────
    // Conveniente para HalfRes/QuarterRes com BC1
    CompressResult DownscaleAndCompress(
        const uint8_t* rgba_pixels,
        uint32_t src_width, uint32_t src_height,
        uint32_t src_row_pitch,
        uint32_t target_width, uint32_t target_height,
        bool use_bc3 = false);   // false=BC1, true=BC3

    // Verifica se ispc_texcomp está disponível no build atual
    static bool IsISPCAvailable();

private:
    // ── Implementações internas ───────────────────────────────────────────────

    // Software fallback — sem dependências externas
    CompressResult SWCompressBC1(const uint8_t* rgba, uint32_t w, uint32_t h,
                                  uint32_t pitch, ProgressCallback cb);
    CompressResult SWCompressBC3(const uint8_t* rgba, uint32_t w, uint32_t h,
                                  uint32_t pitch, ProgressCallback cb);

    // Comprime um bloco 4×4 pixels → 8 bytes BC1
    static void CompressBC1Block(const uint8_t block[64], uint8_t out[8]);

    // Comprime um bloco 4×4 pixels → 16 bytes BC3
    static void CompressBC3Block(const uint8_t block[64], uint8_t out[16]);

    // Extrai bloco 4×4 de pixels RGBA8, com padding nas bordas
    static void ExtractBlock(const uint8_t* src, uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height,
                              uint32_t pitch, uint8_t block[64]);

    // Encontra as duas cores extremas de um bloco para BC1
    static void FindMinMaxColors(const uint8_t block[64],
                                  uint8_t min_color[4], uint8_t max_color[4]);

    // Codifica cor RGB em formato RGB565
    static uint16_t RGB888toRGB565(uint8_t r, uint8_t g, uint8_t b);
};
