/*
 * TextureCompressor.cpp
 * ─────────────────────────────────────────────────────────────────────────────
 * Implementação do compressor de texturas.
 *
 * Inclui:
 *   - Downscale bilinear de alta qualidade
 *   - Compressor BC1 software (zero deps, bom para testes)
 *   - Compressor BC3 software
 *   - Stub para BC7 (requer ispc_texcomp em produção)
 *   - Integração com ispc_texcomp quando VRAM_USE_ISPC=1
 */

#include "TextureCompressor.h"
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <cassert>

#ifdef VRAM_USE_ISPC
#   include <ispc_texcomp.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Helpers internos
// ─────────────────────────────────────────────────────────────────────────────

static CompressResult MakeError(const char* msg) {
    CompressResult r{};
    r.success = false;
    strncpy_s(r.error, msg, sizeof(r.error) - 1);
    return r;
}

// Alinha valor para cima até o próximo múltiplo de 'align'
static uint32_t AlignUp(uint32_t v, uint32_t align) {
    return (v + align - 1) & ~(align - 1);
}

// Clamp de uint8
static uint8_t Clamp8(int v) {
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

bool TextureCompressor::IsISPCAvailable() {
#ifdef VRAM_USE_ISPC
    return true;
#else
    return false;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Downscale bilinear
// ─────────────────────────────────────────────────────────────────────────────

CompressResult TextureCompressor::Downscale(
    const uint8_t* src,
    uint32_t sw, uint32_t sh, uint32_t src_pitch,
    uint32_t dw, uint32_t dh)
{
    if (!src || sw == 0 || sh == 0 || dw == 0 || dh == 0)
        return MakeError("Downscale: parametros invalidos");

    CompressResult r{};
    r.width      = dw;
    r.height     = dh;
    r.row_pitch  = dw * 4;
    r.size_bytes = (uint64_t)dw * dh * 4;
    r.success    = true;
    r.data.resize(r.size_bytes);

    float x_ratio = (float)(sw - 1) / dw;
    float y_ratio = (float)(sh - 1) / dh;

    for (uint32_t dy = 0; dy < dh; ++dy) {
        float gy    = dy * y_ratio;
        uint32_t y0 = (uint32_t)gy;
        uint32_t y1 = std::min(y0 + 1, sh - 1);
        float    fy = gy - y0;

        for (uint32_t dx = 0; dx < dw; ++dx) {
            float gx    = dx * x_ratio;
            uint32_t x0 = (uint32_t)gx;
            uint32_t x1 = std::min(x0 + 1, sw - 1);
            float    fx = gx - x0;

            // 4 vizinhos
            const uint8_t* p00 = src + y0 * src_pitch + x0 * 4;
            const uint8_t* p10 = src + y0 * src_pitch + x1 * 4;
            const uint8_t* p01 = src + y1 * src_pitch + x0 * 4;
            const uint8_t* p11 = src + y1 * src_pitch + x1 * 4;

            uint8_t* dst = r.data.data() + dy * r.row_pitch + dx * 4;

            for (int c = 0; c < 4; ++c) {
                float top    = p00[c] + fx * (p10[c] - p00[c]);
                float bottom = p01[c] + fx * (p11[c] - p01[c]);
                dst[c] = Clamp8((int)(top + fy * (bottom - top)));
            }
        }
    }
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// Downscale + Compressão em um passo
// ─────────────────────────────────────────────────────────────────────────────

CompressResult TextureCompressor::DownscaleAndCompress(
    const uint8_t* rgba, uint32_t sw, uint32_t sh, uint32_t src_pitch,
    uint32_t dw, uint32_t dh, bool use_bc3)
{
    // Garante dimensões múltiplas de 4 (requisito BC1/BC3)
    dw = AlignUp(dw, 4);
    dh = AlignUp(dh, 4);

    CompressResult scaled = Downscale(rgba, sw, sh, src_pitch, dw, dh);
    if (!scaled.success) return scaled;

    return use_bc3
        ? CompressBC3(scaled.data.data(), dw, dh, scaled.row_pitch, nullptr)
        : CompressBC1(scaled.data.data(), dw, dh, scaled.row_pitch, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// BC1 — Ponto de entrada (escolhe ISPC ou software)
// ─────────────────────────────────────────────────────────────────────────────

CompressResult TextureCompressor::CompressBC1(
    const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t pitch,
    ProgressCallback cb)
{
    if (!rgba || w == 0 || h == 0)
        return MakeError("CompressBC1: parametros invalidos");

    // Dimensões devem ser múltiplas de 4
    uint32_t aw = AlignUp(w, 4);
    uint32_t ah = AlignUp(h, 4);

#ifdef VRAM_USE_ISPC
    // ── Caminho rápido: ispc_texcomp ─────────────────────────────────────────
    rgba_surface surface{};
    surface.ptr    = const_cast<uint8_t*>(rgba);
    surface.width  = w;
    surface.height = h;
    surface.stride = pitch;

    bc1_enc_settings settings{};
    GetProfile_fast(&settings);  // perfil de velocidade — use GetProfile_alpha para qualidade

    CompressResult r{};
    r.width      = aw;
    r.height     = ah;
    r.row_pitch  = (aw / 4) * 8;  // 8 bytes por bloco 4×4
    r.size_bytes = (uint64_t)(ah / 4) * r.row_pitch;
    r.success    = true;
    r.data.resize(r.size_bytes);

    CompressBlocksBC1(&surface, r.data.data(), &settings);
    return r;
#else
    // ── Fallback software ─────────────────────────────────────────────────────
    return SWCompressBC1(rgba, w, h, pitch, cb);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// BC3 — Ponto de entrada
// ─────────────────────────────────────────────────────────────────────────────

CompressResult TextureCompressor::CompressBC3(
    const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t pitch,
    ProgressCallback cb)
{
    if (!rgba || w == 0 || h == 0)
        return MakeError("CompressBC3: parametros invalidos");

#ifdef VRAM_USE_ISPC
    rgba_surface surface{};
    surface.ptr    = const_cast<uint8_t*>(rgba);
    surface.width  = w;
    surface.height = h;
    surface.stride = pitch;

    bc3_enc_settings settings{};
    GetProfile_fast(&settings);

    uint32_t aw = AlignUp(w, 4);
    uint32_t ah = AlignUp(h, 4);

    CompressResult r{};
    r.width      = aw;
    r.height     = ah;
    r.row_pitch  = (aw / 4) * 16;  // 16 bytes por bloco 4×4
    r.size_bytes = (uint64_t)(ah / 4) * r.row_pitch;
    r.success    = true;
    r.data.resize(r.size_bytes);

    CompressBlocksBC3(&surface, r.data.data(), &settings);
    return r;
#else
    return SWCompressBC3(rgba, w, h, pitch, cb);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// BC7 — Ponto de entrada (requer ispc_texcomp; software stub retorna erro)
// ─────────────────────────────────────────────────────────────────────────────

CompressResult TextureCompressor::CompressBC7(
    const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t pitch,
    ProgressCallback cb)
{
#ifdef VRAM_USE_ISPC
    rgba_surface surface{};
    surface.ptr    = const_cast<uint8_t*>(rgba);
    surface.width  = w;
    surface.height = h;
    surface.stride = pitch;

    bc7_enc_settings settings{};
    GetProfile_alpha_basic(&settings);  // Boa qualidade com alpha

    uint32_t aw = AlignUp(w, 4);
    uint32_t ah = AlignUp(h, 4);

    CompressResult r{};
    r.width      = aw;
    r.height     = ah;
    r.row_pitch  = (aw / 4) * 16;
    r.size_bytes = (uint64_t)(ah / 4) * r.row_pitch;
    r.success    = true;
    r.data.resize(r.size_bytes);

    CompressBlocksBC7(&surface, r.data.data(), &settings);
    return r;
#else
    (void)rgba; (void)w; (void)h; (void)pitch; (void)cb;
    return MakeError("BC7 requer ispc_texcomp. Compile com VRAM_USE_ISPC=ON.");
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Software BC1 — Compressor sem dependências
// ─────────────────────────────────────────────────────────────────────────────
// Algoritmo: para cada bloco 4×4, encontra as 2 cores extremas (min/max),
// codifica em RGB565, e para cada pixel escolhe o índice 0-3 mais próximo.

uint16_t TextureCompressor::RGB888toRGB565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void TextureCompressor::ExtractBlock(
    const uint8_t* src, uint32_t bx, uint32_t by,
    uint32_t width, uint32_t height, uint32_t pitch,
    uint8_t block[64])
{
    for (int row = 0; row < 4; ++row) {
        uint32_t sy = std::min(by + row, height - 1);
        for (int col = 0; col < 4; ++col) {
            uint32_t sx = std::min(bx + col, width - 1);
            const uint8_t* p = src + sy * pitch + sx * 4;
            uint8_t* b = block + (row * 4 + col) * 4;
            b[0] = p[0]; b[1] = p[1]; b[2] = p[2]; b[3] = p[3];
        }
    }
}

void TextureCompressor::FindMinMaxColors(
    const uint8_t block[64],
    uint8_t min_c[4], uint8_t max_c[4])
{
    // Inmin/max iniciais
    memcpy(min_c, block, 4);
    memcpy(max_c, block, 4);

    for (int i = 1; i < 16; ++i) {
        const uint8_t* p = block + i * 4;
        for (int c = 0; c < 3; ++c) {
            if (p[c] < min_c[c]) min_c[c] = p[c];
            if (p[c] > max_c[c]) max_c[c] = p[c];
        }
    }

    // Expande o range um pouco para melhorar qualidade
    for (int c = 0; c < 3; ++c) {
        int range  = (int)max_c[c] - min_c[c];
        int inset  = range / 16;
        min_c[c]   = (uint8_t)std::max(0,   (int)min_c[c] + inset);
        max_c[c]   = (uint8_t)std::min(255, (int)max_c[c] - inset);
    }
}

void TextureCompressor::CompressBC1Block(const uint8_t block[64], uint8_t out[8]) {
    uint8_t min_c[4], max_c[4];
    FindMinMaxColors(block, min_c, max_c);

    uint16_t c0 = RGB888toRGB565(max_c[0], max_c[1], max_c[2]);
    uint16_t c1 = RGB888toRGB565(min_c[0], min_c[1], min_c[2]);

    // BC1: c0 > c1 significa modo de 4 cores (sem transparência)
    if (c0 <= c1) { uint16_t tmp = c0; c0 = c1; c1 = tmp; }

    out[0] = (uint8_t)(c0 & 0xFF);
    out[1] = (uint8_t)(c0 >> 8);
    out[2] = (uint8_t)(c1 & 0xFF);
    out[3] = (uint8_t)(c1 >> 8);

    // Gera as 4 paletas intermediárias
    uint8_t pal[4][3];
    // c0
    pal[0][0] = (uint8_t)(((c0 >> 11) & 0x1F) * 255 / 31);
    pal[0][1] = (uint8_t)(((c0 >>  5) & 0x3F) * 255 / 63);
    pal[0][2] = (uint8_t)(((c0      ) & 0x1F) * 255 / 31);
    // c1
    pal[1][0] = (uint8_t)(((c1 >> 11) & 0x1F) * 255 / 31);
    pal[1][1] = (uint8_t)(((c1 >>  5) & 0x3F) * 255 / 63);
    pal[1][2] = (uint8_t)(((c1      ) & 0x1F) * 255 / 31);
    // 2/3 c0 + 1/3 c1
    pal[2][0] = (uint8_t)((2 * pal[0][0] + pal[1][0]) / 3);
    pal[2][1] = (uint8_t)((2 * pal[0][1] + pal[1][1]) / 3);
    pal[2][2] = (uint8_t)((2 * pal[0][2] + pal[1][2]) / 3);
    // 1/3 c0 + 2/3 c1
    pal[3][0] = (uint8_t)((pal[0][0] + 2 * pal[1][0]) / 3);
    pal[3][1] = (uint8_t)((pal[0][1] + 2 * pal[1][1]) / 3);
    pal[3][2] = (uint8_t)((pal[0][2] + 2 * pal[1][2]) / 3);

    // Para cada pixel, acha o índice 0-3 mais próximo (distância quadrática)
    uint32_t indices = 0;
    for (int i = 15; i >= 0; --i) {
        const uint8_t* px = block + i * 4;
        int best_idx  = 0;
        int best_dist = INT_MAX;
        for (int j = 0; j < 4; ++j) {
            int dr = (int)px[0] - pal[j][0];
            int dg = (int)px[1] - pal[j][1];
            int db = (int)px[2] - pal[j][2];
            int d  = dr*dr + dg*dg + db*db;
            if (d < best_dist) { best_dist = d; best_idx = j; }
        }
        indices = (indices << 2) | (uint32_t)best_idx;
    }

    out[4] = (uint8_t)( indices        & 0xFF);
    out[5] = (uint8_t)((indices >>  8) & 0xFF);
    out[6] = (uint8_t)((indices >> 16) & 0xFF);
    out[7] = (uint8_t)((indices >> 24) & 0xFF);
}

CompressResult TextureCompressor::SWCompressBC1(
    const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t pitch,
    ProgressCallback cb)
{
    uint32_t bw = AlignUp(w, 4) / 4;  // blocos na largura
    uint32_t bh = AlignUp(h, 4) / 4;  // blocos na altura

    CompressResult r{};
    r.width      = AlignUp(w, 4);
    r.height     = AlignUp(h, 4);
    r.row_pitch  = bw * 8;
    r.size_bytes = (uint64_t)bh * r.row_pitch;
    r.success    = true;
    r.data.resize(r.size_bytes);

    uint32_t total_blocks = bw * bh;
    uint32_t processed    = 0;

    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            uint8_t block[64];
            ExtractBlock(rgba, bx * 4, by * 4, w, h, pitch, block);

            uint8_t* dst = r.data.data() + by * r.row_pitch + bx * 8;
            CompressBC1Block(block, dst);

            ++processed;
            if (cb && (processed % 64 == 0 || processed == total_blocks))
                cb((float)processed / total_blocks);
        }
    }
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// Software BC3 — BC1 para RGB + codificação de alpha separada
// ─────────────────────────────────────────────────────────────────────────────

void TextureCompressor::CompressBC3Block(const uint8_t block[64], uint8_t out[16]) {
    // Primeiros 8 bytes: compressão do canal alpha (dois valores + 6×3bits de índices)
    uint8_t a0 = 0, a1 = 255;
    for (int i = 0; i < 16; ++i) {
        uint8_t a = block[i * 4 + 3];
        if (a > a0) a0 = a;
        if (a < a1) a1 = a;
    }

    out[0] = a0;
    out[1] = a1;

    // Paleta de 8 valores de alpha interpolados
    uint8_t apal[8];
    apal[0] = a0; apal[1] = a1;
    if (a0 > a1) {
        for (int i = 2; i < 8; ++i)
            apal[i] = (uint8_t)((a0 * (8 - i) + a1 * (i - 1)) / 7);
    } else {
        for (int i = 2; i < 6; ++i)
            apal[i] = (uint8_t)((a0 * (6 - i) + a1 * (i - 1)) / 5);
        apal[6] = 0; apal[7] = 255;
    }

    // Índices de alpha (48 bits = 6 bytes, 3 bits por pixel)
    uint64_t alpha_bits = 0;
    for (int i = 0; i < 16; ++i) {
        uint8_t a    = block[i * 4 + 3];
        int best_idx = 0, best_dist = INT_MAX;
        for (int j = 0; j < 8; ++j) {
            int d = abs((int)a - apal[j]);
            if (d < best_dist) { best_dist = d; best_idx = j; }
        }
        alpha_bits |= ((uint64_t)best_idx << (i * 3));
    }

    out[2] = (uint8_t)( alpha_bits        & 0xFF);
    out[3] = (uint8_t)((alpha_bits >>  8) & 0xFF);
    out[4] = (uint8_t)((alpha_bits >> 16) & 0xFF);
    out[5] = (uint8_t)((alpha_bits >> 24) & 0xFF);
    out[6] = (uint8_t)((alpha_bits >> 32) & 0xFF);
    out[7] = (uint8_t)((alpha_bits >> 40) & 0xFF);

    // Bytes 8-15: BC1 para os canais RGB (reutiliza CompressBC1Block)
    CompressBC1Block(block, out + 8);
}

CompressResult TextureCompressor::SWCompressBC3(
    const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t pitch,
    ProgressCallback cb)
{
    uint32_t bw = AlignUp(w, 4) / 4;
    uint32_t bh = AlignUp(h, 4) / 4;

    CompressResult r{};
    r.width      = AlignUp(w, 4);
    r.height     = AlignUp(h, 4);
    r.row_pitch  = bw * 16;
    r.size_bytes = (uint64_t)bh * r.row_pitch;
    r.success    = true;
    r.data.resize(r.size_bytes);

    uint32_t total = bw * bh, processed = 0;

    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            uint8_t block[64];
            ExtractBlock(rgba, bx * 4, by * 4, w, h, pitch, block);

            uint8_t* dst = r.data.data() + by * r.row_pitch + bx * 16;
            CompressBC3Block(block, dst);

            ++processed;
            if (cb && (processed % 64 == 0 || processed == total))
                cb((float)processed / total);
        }
    }
    return r;
}
