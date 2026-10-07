/*
 * d3d8_smoke - smoke-test harness for the xbox_d3d8 pure format layer.
 *
 * Exercises the real format tables, predicates, swizzle classification and
 * software conversions without needing a D3D11 device:
 *   - d3d8_to_dxgi_format, d3d8_format_bpp, d3d8_format_is_*
 *   - d3d8_format_is_swizzled / unswizzle round-trip (d3d8_swizzle.h)
 *   - swizzle_offset agreeing with xbox_swizzle_rect, texel for texel
 *   - DXT1/3/5 block decode (d3d8_dxt_decode_texel)
 *   - d3d8_convert_linear_pixels (channel swaps, sign extension)
 *
 * The D3D8 resource file is compiled alongside this driver; the handful of
 * device-accessor functions it references are satisfied by the stubs below.
 */

#define COBJMACROS
#include "d3d8_internal.h"
#include "d3d8_swizzle.h"
#include "d3d8_fvf.h"
#include "nv2a_shader_cpu.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- Stub accessors referenced by d3d8_resources.c (not used by the
 *      pure functions under test, but must link). ---- */
ID3D11Device        *d3d8_GetD3D11Device(void)   { return NULL; }
ID3D11DeviceContext *d3d8_GetD3D11Context(void)  { return NULL; }
ID3D11RenderTargetView *d3d8_GetDefaultRTV(void) { return NULL; }
IDirect3DDevice8      *xbox_GetD3DDevice(void)  { return NULL; }

static DWORD g_palette[256];
const DWORD *d3d8_GetPalette(DWORD stage) { (void)stage; return g_palette; }

static int failures = 0;

#define CHECK(name, cond) \
    do { if (cond) { /* pass */ } else { printf("FAIL: %s\n", name); failures++; } } while (0)

#define CHECK_INT(name, got, expected) \
    do { if ((got) == (expected)) { } else { printf("FAIL: %s (got %d, want %d)\n", name, (int)(got), (int)(expected)); failures++; } } while (0)

#define CHECK_FMT(name, got, expected) \
    do { if ((got) == (expected)) { } else { printf("FAIL: %s (got %d, want %d)\n", name, (int)(got), (int)(expected)); failures++; } } while (0)

static void test_to_dxgi(void)
{
    printf("test_to_dxgi\n");
    CHECK_FMT("R16F",      d3d8_to_dxgi_format(D3DFMT_R16F),          DXGI_FORMAT_R16_FLOAT);
    CHECK_FMT("R32F",      d3d8_to_dxgi_format(D3DFMT_R32F),          DXGI_FORMAT_R32_FLOAT);
    CHECK_FMT("G16R16F",   d3d8_to_dxgi_format(D3DFMT_G16R16F),       DXGI_FORMAT_R16G16_FLOAT);
    CHECK_FMT("G32R32F",   d3d8_to_dxgi_format(D3DFMT_G32R32F),       DXGI_FORMAT_R32G32_FLOAT);
    CHECK_FMT("A16B16G16R16F", d3d8_to_dxgi_format(D3DFMT_A16B16G16R16F), DXGI_FORMAT_R16G16B16A16_FLOAT);
    CHECK_FMT("A32B32G32R32F", d3d8_to_dxgi_format(D3DFMT_A32B32G32R32F), DXGI_FORMAT_R32G32B32A32_FLOAT);
    CHECK_FMT("G16R16",    d3d8_to_dxgi_format(D3DFMT_G16R16),        DXGI_FORMAT_R16G16_UNORM);
    CHECK_FMT("A16L16",    d3d8_to_dxgi_format(D3DFMT_A16L16),        DXGI_FORMAT_R16G16_UNORM);
    CHECK_FMT("A16B16G16R16", d3d8_to_dxgi_format(D3DFMT_A16B16G16R16), DXGI_FORMAT_R16G16B16A16_UNORM);
    CHECK_FMT("A32B32G32R32", d3d8_to_dxgi_format(D3DFMT_A32B32G32R32), DXGI_FORMAT_R32G32B32A32_FLOAT);
    CHECK_FMT("G32R32",    d3d8_to_dxgi_format(D3DFMT_G32R32),        DXGI_FORMAT_R32G32_FLOAT);
    CHECK_FMT("L32",       d3d8_to_dxgi_format(D3DFMT_L32),           DXGI_FORMAT_R32_FLOAT);
    CHECK_FMT("A32L32",    d3d8_to_dxgi_format(D3DFMT_A32L32),        DXGI_FORMAT_R32G32_FLOAT);
    CHECK_FMT("V32U32",    d3d8_to_dxgi_format(D3DFMT_V32U32),        DXGI_FORMAT_R32G32_FLOAT);
    CHECK_FMT("Q16W16V16U16", d3d8_to_dxgi_format(D3DFMT_Q16W16V16U16), DXGI_FORMAT_R16G16B16A16_SNORM);
    CHECK_FMT("Q32W32V32U32", d3d8_to_dxgi_format(D3DFMT_Q32W32V32U32), DXGI_FORMAT_R32G32B32A32_SINT);
    CHECK_FMT("A2R10G10B10", d3d8_to_dxgi_format(D3DFMT_A2R10G10B10), DXGI_FORMAT_R10G10B10A2_UNORM);
    CHECK_FMT("X2R10G10B10", d3d8_to_dxgi_format(D3DFMT_X2R10G10B10), DXGI_FORMAT_R10G10B10A2_UNORM);
    CHECK_FMT("A2B10G10R10", d3d8_to_dxgi_format(D3DFMT_A2B10G10R10), DXGI_FORMAT_R10G10B10A2_UNORM);
    CHECK_FMT("A2W10V10U10", d3d8_to_dxgi_format(D3DFMT_A2W10V10U10), DXGI_FORMAT_R10G10B10A2_UNORM);
    CHECK_FMT("R11G11B10", d3d8_to_dxgi_format(D3DFMT_R11G11B10),     DXGI_FORMAT_R11G11B10_FLOAT);
    CHECK_FMT("D24X8",     d3d8_to_dxgi_format(D3DFMT_D24X8),         DXGI_FORMAT_D24_UNORM_S8_UINT);
    CHECK_FMT("D24FS8",    d3d8_to_dxgi_format(D3DFMT_D24FS8),        DXGI_FORMAT_D24_UNORM_S8_UINT);
    CHECK_FMT("D32",       d3d8_to_dxgi_format(D3DFMT_D32),           DXGI_FORMAT_D32_FLOAT);
    CHECK_FMT("DXN",       d3d8_to_dxgi_format(D3DFMT_DXN),           DXGI_FORMAT_BC5_UNORM);
    CHECK_FMT("DXT3A",     d3d8_to_dxgi_format(D3DFMT_DXT3A),         DXGI_FORMAT_BC2_UNORM);
    CHECK_FMT("DXT5A",     d3d8_to_dxgi_format(D3DFMT_DXT5A),         DXGI_FORMAT_BC3_UNORM);
    CHECK_FMT("CTX1",      d3d8_to_dxgi_format(D3DFMT_CTX1),          DXGI_FORMAT_BC1_UNORM);
    /* LIN variants */
    CHECK_FMT("LIN_R16F",  d3d8_to_dxgi_format(D3DFMT_LIN_R16F),      DXGI_FORMAT_R16_FLOAT);
    CHECK_FMT("LIN_A16B16G16R16", d3d8_to_dxgi_format(D3DFMT_LIN_A16B16G16R16), DXGI_FORMAT_R16G16B16A16_UNORM);
    CHECK_FMT("LIN_Q16W16V16U16", d3d8_to_dxgi_format(D3DFMT_LIN_Q16W16V16U16), DXGI_FORMAT_R16G16B16A16_SNORM);
}

static void test_bpp(void)
{
    printf("test_bpp\n");
    CHECK_INT("R16F 16",  d3d8_format_bpp(D3DFMT_R16F), 16);
    CHECK_INT("R32F 32",  d3d8_format_bpp(D3DFMT_R32F), 32);
    CHECK_INT("G16R16F",  d3d8_format_bpp(D3DFMT_G16R16F), 32);
    CHECK_INT("G32R32F",  d3d8_format_bpp(D3DFMT_G32R32F), 64);
    CHECK_INT("A16B16G16R16F", d3d8_format_bpp(D3DFMT_A16B16G16R16F), 64);
    CHECK_INT("A32B32G32R32F", d3d8_format_bpp(D3DFMT_A32B32G32R32F), 128);
    CHECK_INT("G16R16",   d3d8_format_bpp(D3DFMT_G16R16), 32);
    CHECK_INT("A16L16",   d3d8_format_bpp(D3DFMT_A16L16), 32);
    CHECK_INT("A16B16G16R16", d3d8_format_bpp(D3DFMT_A16B16G16R16), 64);
    CHECK_INT("A32B32G32R32", d3d8_format_bpp(D3DFMT_A32B32G32R32), 128);
    CHECK_INT("G32R32",   d3d8_format_bpp(D3DFMT_G32R32), 64);
    CHECK_INT("L32",      d3d8_format_bpp(D3DFMT_L32), 32);
    CHECK_INT("A32L32",   d3d8_format_bpp(D3DFMT_A32L32), 64);
    CHECK_INT("V32U32",   d3d8_format_bpp(D3DFMT_V32U32), 64);
    CHECK_INT("Q16W16V16U16", d3d8_format_bpp(D3DFMT_Q16W16V16U16), 64);
    CHECK_INT("Q32W32V32U32", d3d8_format_bpp(D3DFMT_Q32W32V32U32), 128);
    CHECK_INT("A2R10G10B10", d3d8_format_bpp(D3DFMT_A2R10G10B10), 32);
    CHECK_INT("R11G11B10", d3d8_format_bpp(D3DFMT_R11G11B10), 32);
    CHECK_INT("D32",      d3d8_format_bpp(D3DFMT_D32), 32);
    CHECK_INT("DXN",      d3d8_format_bpp(D3DFMT_DXN), 8);
    CHECK_INT("DXT3A",    d3d8_format_bpp(D3DFMT_DXT3A), 8);
    CHECK_INT("DXT5A",    d3d8_format_bpp(D3DFMT_DXT5A), 8);
    CHECK_INT("CTX1",     d3d8_format_bpp(D3DFMT_CTX1), 4);
    CHECK_INT("LIN_A16B16G16R16", d3d8_format_bpp(D3DFMT_LIN_A16B16G16R16), 64);
    CHECK_INT("LIN_A2R10G10B10", d3d8_format_bpp(D3DFMT_LIN_A2R10G10B10), 32);
}

static void test_predicates(void)
{
    printf("test_predicates\n");
    CHECK("compressed CTX1",  d3d8_format_is_compressed(D3DFMT_CTX1));
    CHECK("compressed DXN",   d3d8_format_is_compressed(D3DFMT_DXN));
    CHECK("compressed DXT3A", d3d8_format_is_compressed(D3DFMT_DXT3A));
    CHECK("compressed DXT5A", d3d8_format_is_compressed(D3DFMT_DXT5A));
    CHECK("not compressed R32F", !d3d8_format_is_compressed(D3DFMT_R32F));
    CHECK("depth D24FS8",     d3d8_format_is_depth(D3DFMT_D24FS8));
    CHECK("depth D24X8",      d3d8_format_is_depth(D3DFMT_D24X8));
    CHECK("depth D32",        d3d8_format_is_depth(D3DFMT_D32));
    CHECK("not depth R32F",   !d3d8_format_is_depth(D3DFMT_R32F));
}

static void test_swizzle_classification(void)
{
    printf("test_swizzle_classification\n");
    /* Swizzled (default) */
    CHECK("swizzled R32F",     d3d8_format_is_swizzled(D3DFMT_R32F) == 1);
    CHECK("swizzled A16B16G16R16", d3d8_format_is_swizzled(D3DFMT_A16B16G16R16) == 1);
    CHECK("swizzled A2R10G10B10", d3d8_format_is_swizzled(D3DFMT_A2R10G10B10) == 1);
    CHECK("swizzled D24FS8-not",  d3d8_format_is_swizzled(D3DFMT_D24FS8) == 0);   /* depth */
    CHECK("swizzled D32-not",     d3d8_format_is_swizzled(D3DFMT_D32) == 0);
    CHECK("swizzled DXN-not",     d3d8_format_is_swizzled(D3DFMT_DXN) == 0);     /* compressed */
    CHECK("swizzled CTX1-not",    d3d8_format_is_swizzled(D3DFMT_CTX1) == 0);
    /* Linear */
    CHECK("linear LIN_R16F",      d3d8_format_is_swizzled(D3DFMT_LIN_R16F) == 0);
    CHECK("linear LIN_A16B16G16R16", d3d8_format_is_swizzled(D3DFMT_LIN_A16B16G16R16) == 0);
    CHECK("linear LIN_A2R10G10B10", d3d8_format_is_swizzled(D3DFMT_LIN_A2R10G10B10) == 0);
    CHECK("linear LIN_Q16W16V16U16", d3d8_format_is_swizzled(D3DFMT_LIN_Q16W16V16U16) == 0);
    CHECK("linear LIN_R32F",      d3d8_format_is_swizzled(D3DFMT_LIN_R32F) == 0);
    CHECK("linear LIN_D24FS8",    d3d8_format_is_swizzled(D3DFMT_LIN_D24FS8) == 0);
    CHECK("linear LIN_DXN",       d3d8_format_is_swizzled(D3DFMT_LIN_DXN) == 0);
}

static void test_unswizzle_roundtrip(void)
{
    printf("test_unswizzle_roundtrip\n");
    /* Real round-trip: linear -> swizzle -> unswizzle must recover the
     * original linear data exactly, for a few sizes and bpp values. */
    {
        struct { UINT w, h, bpp; } cases[] = {
            { 4,   4,   1 }, { 4, 4, 2 }, { 4, 4, 4 },
            { 8,   8,   4 }, { 16, 4, 4 }, { 4, 16, 2 }, { 32, 8, 4 },
        };
        int c;
        for (c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); c++) {
            UINT w = cases[c].w, h = cases[c].h, bpp = cases[c].bpp;
            UINT n = w * h * bpp;
            BYTE *lin = (BYTE *)malloc(n), *swz = (BYTE *)malloc(n), *re = (BYTE *)malloc(n);
            UINT i;
            for (i = 0; i < n; i++) lin[i] = (BYTE)(i * 31 + c);
            xbox_swizzle_rect(swz, lin, w, h, bpp);
            memset(re, 0, n);
            xbox_unswizzle_rect(re, swz, w, h, bpp);
            {
                char nm[64];
                snprintf(nm, sizeof(nm), "unswizzle roundtrip %ux%u bpp%u", w, h, bpp);
                CHECK(nm, memcmp(lin, re, n) == 0);
            }
            free(lin); free(swz); free(re);
        }
    }
}

/* swizzle_offset must land on the same byte that xbox_swizzle_rect wrote.
 *
 * The two are separate implementations of Morton order: xbox_swizzle_rect
 * walks rows and is what unswizzling (and therefore the D3D11 path) trusts,
 * while swizzle_offset computes one texel's address and is what the software
 * sampler calls per pixel. If they disagree, a texture decodes correctly in
 * one path and wrongly in the other -- which is how this was found: the
 * sampler read a column of the image for every row and drew vertical stripes.
 *
 * The old implementation spread both coordinates onto even bit positions and
 * masked, so the Y term was almost always zero: at 512x512 offset(0,1) equalled
 * offset(0,0) and 261,632 of 262,144 coordinates collided. Any size here with
 * height > 1 catches that.
 */
static void test_swizzle_offset(void)
{
    struct { UINT w, h; } cases[] = {
        { 4, 4 }, { 8, 8 }, { 64, 64 },
        { 16, 4 }, { 4, 16 }, { 32, 8 }, { 256, 64 },   /* non-square: uneven bit runs */
    };
    int c;

    printf("test_swizzle_offset\n");
    for (c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); c++) {
        UINT w = cases[c].w, h = cases[c].h, bpp = 4, n = w * h * bpp;
        UINT32 *lin = (UINT32 *)malloc(n), *swz = (UINT32 *)malloc(n);
        UINT bad = 0, x, y;

        /* One distinct value per texel, so a wrong address cannot alias. */
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                lin[y * w + x] = 0xC0DE0000u | (y << 8) | x;
        xbox_swizzle_rect((BYTE *)swz, (const BYTE *)lin, w, h, bpp);

        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                if (swz[swizzle_offset(x, y, w, h)] != lin[y * w + x])
                    bad++;
        {
            char nm[64];
            snprintf(nm, sizeof(nm), "swizzle_offset agrees %ux%u", w, h);
            CHECK_INT(nm, (int)bad, 0);
        }
        free(lin); free(swz);
    }
}

/* DXT1/3/5 decode, against blocks built here rather than a captured texture.
 *
 * Every case pins something a plausible bug would move: which endpoint is c0,
 * that a 5-bit channel widens to 0xFF and not 0xF8, that the 1/3 and 2/3
 * interpolants are not swapped, that c0 <= c1 means three colours and a
 * transparent index 3 -- but only for DXT1, because DXT3 and DXT5 carry their
 * own alpha and keep the four-colour block whatever the endpoints say.
 */
static void put16(BYTE *p, UINT v) { p[0] = (BYTE)(v & 0xFF); p[1] = (BYTE)(v >> 8); }

static void test_dxt_decode(void)
{
    BYTE blk[16];
    UINT32 argb;
    const UINT RED = 0xF800, BLUE = 0x001F;   /* RGB565 */

    printf("test_dxt_decode\n");

    CHECK_INT("dxt block bytes DXT1", (int)d3d8_format_dxt_block_bytes(0x0C), 8);
    CHECK_INT("dxt block bytes DXT3", (int)d3d8_format_dxt_block_bytes(0x0E), 16);
    CHECK_INT("dxt block bytes DXT5", (int)d3d8_format_dxt_block_bytes(0x0F), 16);
    CHECK_INT("dxt block bytes A8R8G8B8", (int)d3d8_format_dxt_block_bytes(0x06), 0);

    /* DXT1, four-colour mode (c0 > c1): index 0 is pure red, 1 pure blue,
     * 2 is two thirds red, 3 is one third red. All opaque. */
    memset(blk, 0, sizeof blk);
    put16(blk + 0, RED); put16(blk + 2, BLUE);
    blk[4] = 0x00 | (0x1 << 2) | (0x2 << 4) | (0x3 << 6);   /* texels 0..3 */
    CHECK("dxt1 idx0 red",  d3d8_dxt_decode_texel(blk, 0x0C, 0, 0, 4, &argb) && argb == 0xFFFF0000u);
    CHECK("dxt1 idx1 blue", d3d8_dxt_decode_texel(blk, 0x0C, 1, 0, 4, &argb) && argb == 0xFF0000FFu);
    d3d8_dxt_decode_texel(blk, 0x0C, 2, 0, 4, &argb);
    CHECK_INT("dxt1 idx2 red channel", (int)((argb >> 16) & 0xFF), 170);   /* (2*255+0)/3 */
    d3d8_dxt_decode_texel(blk, 0x0C, 3, 0, 4, &argb);
    CHECK_INT("dxt1 idx3 red channel", (int)((argb >> 16) & 0xFF), 85);    /* (255+0)/3  */

    /* DXT1, three-colour mode (c0 <= c1): index 2 is the midpoint, index 3 is
     * transparent. Getting this branch backwards is the classic DXT1 bug. */
    memset(blk, 0, sizeof blk);
    put16(blk + 0, BLUE); put16(blk + 2, RED);
    blk[4] = (0x2 << 0) | (0x3 << 2);
    d3d8_dxt_decode_texel(blk, 0x0C, 0, 0, 4, &argb);
    CHECK_INT("dxt1 3-colour midpoint red", (int)((argb >> 16) & 0xFF), 127);
    CHECK("dxt1 3-colour idx3 transparent",
          d3d8_dxt_decode_texel(blk, 0x0C, 1, 0, 4, &argb) && (argb >> 24) == 0);

    /* Same endpoints as a DXT5 block: alpha is its own, and the colour block
     * stays four-colour even though c0 <= c1. */
    memset(blk, 0, sizeof blk);
    blk[0] = 255; blk[1] = 0;                    /* a0 > a1: eight alphas */
    blk[2] = 0x00;                               /* texel 0 selects a0    */
    put16(blk + 8, BLUE); put16(blk + 10, RED);
    blk[12] = (0x2 << 0);
    d3d8_dxt_decode_texel(blk, 0x0F, 0, 0, 4, &argb);
    CHECK_INT("dxt5 alpha from a0", (int)(argb >> 24), 255);
    /* 85 is (2*c0 + c1)/3 with c0 = blue: the four-colour interpolant. Three-
     * colour mode would put the midpoint, 127, here instead, so this one
     * value separates the two branches. */
    CHECK_INT("dxt5 keeps 4-colour block", (int)((argb >> 16) & 0xFF), 85);

    /* DXT3's alpha is a raw 4-bit nibble per texel, low nibble first. */
    memset(blk, 0, sizeof blk);
    blk[0] = 0x0F;                               /* texel 0 = 15, texel 1 = 0 */
    put16(blk + 8, RED); put16(blk + 10, BLUE);
    d3d8_dxt_decode_texel(blk, 0x0E, 0, 0, 4, &argb);
    CHECK_INT("dxt3 alpha 15 -> 255", (int)(argb >> 24), 255);
    d3d8_dxt_decode_texel(blk, 0x0E, 1, 0, 4, &argb);
    CHECK_INT("dxt3 alpha 0 -> 0", (int)(argb >> 24), 0);

    /* Block addressing: texel (4,0) is the second block along, not texel 4 of
     * the first. A wrong blocks-per-row shows up here and nowhere else. */
    {
        BYTE img[32];
        memset(img, 0, sizeof img);
        put16(img + 0, RED);  put16(img + 2, RED);    /* block 0: all red   */
        put16(img + 8, BLUE); put16(img + 10, BLUE);  /* block 1: all blue  */
        CHECK("dxt1 block 0", d3d8_dxt_decode_texel(img, 0x0C, 0, 0, 8, &argb) && argb == 0xFFFF0000u);
        CHECK("dxt1 block 1", d3d8_dxt_decode_texel(img, 0x0C, 4, 0, 8, &argb) && argb == 0xFF0000FFu);
    }

    {
        d3d8_dxt_texel_cache cache = {0};
        BYTE image[144];
        const UINT formats[] = {0x0C, 0x0E, 0x0F};
        UINT format_index, pass, horizontal, vertical, byte_index;
        UINT32 expected, cached;
        for (byte_index = 0; byte_index < sizeof image; byte_index++)
            image[byte_index] = (BYTE)(byte_index * 73u + 19u);
        for (format_index = 0; format_index < 3; format_index++) {
            for (pass = 0; pass < 3; pass++) {
                for (vertical = 0; vertical < 9; vertical++) {
                    for (horizontal = 0; horizontal < 9; horizontal++) {
                        CHECK("dxt reference decodes",
                              d3d8_dxt_decode_texel(image, formats[format_index],
                                                   horizontal, vertical, 9, &expected));
                        CHECK("dxt cached decodes",
                              d3d8_dxt_decode_texel_cached(&cache, image, formats[format_index],
                                                          horizontal, vertical, 9, &cached));
                        CHECK("dxt cache matches reference", cached == expected);
                    }
                }
                if (pass == 1)
                    for (byte_index = 0; byte_index < sizeof image; byte_index++)
                        image[byte_index] ^= 0x5Au;
            }
        }
        CHECK("dxt cache rejects unsupported format",
              !d3d8_dxt_decode_texel_cached(&cache, image, 0x06, 0, 0, 9, &cached));
        CHECK("dxt cache rejects zero width",
              !d3d8_dxt_decode_texel_cached(&cache, image, 0x0F, 0, 0, 0, &cached));
        CHECK("dxt cache reflects source update",
              d3d8_dxt_decode_texel_cached(&cache, blk, 0x0E, 0, 0, 4, &cached)
              && cached == 0xFFFF0000u);
        blk[0] = 0;
        CHECK("dxt cache invalidates changed alpha",
              d3d8_dxt_decode_texel_cached(&cache, blk, 0x0E, 0, 0, 4, &cached)
              && cached == 0x00FF0000u);
    }
    if (GetEnvironmentVariableA("D3D8_DXT_BENCH", NULL, 0)) {
        BYTE texture[4096];
        const UINT formats[] = {0x0C, 0x0E, 0x0F};
        UINT format_index, byte_index, sample_index, cached_pass;
        LARGE_INTEGER frequency, start, finish;
        UINT32 checksums[2];
        QueryPerformanceFrequency(&frequency);
        for (byte_index = 0; byte_index < sizeof texture; byte_index++)
            texture[byte_index] = (BYTE)(byte_index * 73u + 19u);
        for (format_index = 0; format_index < 3; format_index++) {
            for (cached_pass = 0; cached_pass < 2; cached_pass++) {
                d3d8_dxt_texel_cache cache = {0};
                volatile UINT32 checksum = 0;
                QueryPerformanceCounter(&start);
                for (sample_index = 0; sample_index < 5000000u; sample_index++) {
                    UINT horizontal = ((sample_index % 640u) * 64u) / 640u;
                    UINT vertical = (sample_index / 640u) % 64u;
                    if (cached_pass)
                        d3d8_dxt_decode_texel_cached(&cache, texture, formats[format_index],
                                                    horizontal, vertical, 64, &argb);
                    else
                        d3d8_dxt_decode_texel(texture, formats[format_index],
                                             horizontal, vertical, 64, &argb);
                    checksum += argb;
                }
                QueryPerformanceCounter(&finish);
                checksums[cached_pass] = checksum;
                printf("DXT benchmark format 0x%02X %s: %.3f ms checksum %08X\n",
                       formats[format_index], cached_pass ? "cached" : "reference",
                       1000.0 * (double)(finish.QuadPart - start.QuadPart) /
                           (double)frequency.QuadPart, (unsigned)checksum);
            }
            CHECK("dxt benchmark preserves checksum", checksums[0] == checksums[1]);
        }
    }
}

/* Forward the D3D8 conversion helper through a tiny local wrapper so we
 * test the real d3d8_resources.c implementation. */
static void test_convert_linear(void)
{
    BYTE src[64], dst[64];
    printf("test_convert_linear\n");

    /* A16B16G16R16 -> R16G16B16A16 (channel swap, 64 bpp, 1 px).
     * src[0..7] = A(LE) B G R. Use A=1, B=2, G=3, R=4. */
    {
        UINT16 s[4] = { 1, 2, 3, 4 };  /* A=1, B=2, G=3, R=4 */
        UINT16 d[4] = { 0, 0, 0, 0 };
        d3d8_convert_linear_pixels(D3DFMT_A16B16G16R16, 1, 1, (const BYTE *)s, (BYTE *)d, 0);
        CHECK_INT("A16B16G16R16 R", d[0], 4);
        CHECK_INT("A16B16G16R16 G", d[1], 3);
        CHECK_INT("A16B16G16R16 B", d[2], 2);
        CHECK_INT("A16B16G16R16 A", d[3], 1);
        CHECK("has_conversion A16B16G16R16", d3d8_format_has_conversion(D3DFMT_A16B16G16R16));
    }

    /* A32B32G32R32 -> R32G32B32A32 (channel swap, 128 bpp, 1 px). */
    {
        UINT32 s[4] = { 1, 2, 3, 4 };  /* A=1, B=2, G=3, R=4 */
        UINT32 d[4] = { 0, 0, 0, 0 };
        d3d8_convert_linear_pixels(D3DFMT_A32B32G32R32, 1, 1, (const BYTE *)s, (BYTE *)d, 0);
        CHECK_INT("A32B32G32R32 R", d[0], 4);
        CHECK_INT("A32B32G32R32 G", d[1], 3);
        CHECK_INT("A32B32G32R32 B", d[2], 2);
        CHECK_INT("A32B32G32R32 A", d[3], 1);
        CHECK("has_conversion A32B32G32R32", d3d8_format_has_conversion(D3DFMT_A32B32G32R32));
    }

    /* A2R10G10B10 -> R10G10B10A2 (swap A and R fields).
     * Xbox: A=0x3, R=0x3FF (bits 20-29), G=0x155, B=0x0AA.
     * word = (0x3<<30)|(0x3FF<<20)|(0x155<<10)|0x0AA */
    {
        UINT32 s, d;
        s = (0x3u << 30) | (0x3FFu << 20) | (0x155u << 10) | (0x0AAu);
        d3d8_convert_linear_pixels(D3DFMT_A2R10G10B10, 1, 1, (const BYTE *)&s, (BYTE *)&d, 0);
        CHECK_INT("A2R10G10B10->R10G10B10A2", d, (0x3u << 30) | (0x3FFu) | (0x155u << 10) | (0x0AAu << 20));
        CHECK("has_conversion A2R10G10B10", d3d8_format_has_conversion(D3DFMT_A2R10G10B10));
    }

    /* L6V5U5 sign extension: V=0x10 (=-16), U=0x10, L=0x20.
     * word = (U<<11)|(L<<5)|V = (0x10<<11)|(0x20<<5)|0x10 */
    {
        UINT16 w = (UINT16)((0x10u << 11) | (0x20u << 5) | 0x10u);
        BYTE d[2] = { 0, 0 };
        d3d8_convert_linear_pixels(D3DFMT_L6V5U5, 1, 1, (const BYTE *)&w, d, 0);
        CHECK_INT("L6V5U5 V->R sign", (INT8)d[0], -16 * 8);   /* -16 << 3 */
        CHECK_INT("L6V5U5 U->G sign", (INT8)d[1], -16 * 8);
        CHECK("has_conversion L6V5U5", d3d8_format_has_conversion(D3DFMT_L6V5U5));
    }

    /* A2B10G10R10 is a direct map (no conversion). */
    CHECK("no conv A2B10G10R10", !d3d8_format_has_conversion(D3DFMT_A2B10G10R10));

    (void)src; (void)dst;
}

static void test_fvf_position(void)
{
    static const struct { DWORD position; UINT bytes; int transformed; } cases[] = {
        {D3DFVF_XYZ, 12, 0}, {D3DFVF_XYZRHW, 16, 1},
        {D3DFVF_XYZB1, 16, 0}, {D3DFVF_XYZB2, 20, 0},
        {D3DFVF_XYZB3, 24, 0}, {D3DFVF_XYZB4, 28, 0},
        {D3DFVF_XYZB5, 32, 0}, {0, 0, 0},
    };
    UINT i;
    printf("test_fvf_position\n");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        DWORD fvf = cases[i].position | D3DFVF_DIFFUSE | D3DFVF_TEX1;
        CHECK_INT("position span", d3d8_fvf_position_bytes(fvf), cases[i].bytes);
        CHECK_INT("transformed", d3d8_fvf_transformed(fvf), cases[i].transformed);
    }
    /* A packed last beta still occupies one four-byte slot. */
    CHECK_INT("indexed beta span", d3d8_fvf_position_bytes(0x1000u | D3DFVF_XYZB3), 24);
    CHECK_INT("weighted normal offset", d3d8_fvf_position_bytes(0x118u), 20);
}

static void test_vertex_program(void)
{
    uint32_t program[136][4] = {{0}}, valid[136] = {0};
    float attributes[16][4] = {{0}}, constants[192][4] = {{0}};
    Nv2aCpuVertex vertex;
    printf("test_vertex_program\n");
    attributes[0][0] = 120.0f; attributes[0][1] = 80.0f; attributes[0][3] = 1.0f;
    attributes[3][0] = 0.8f; attributes[3][1] = 0.6f; attributes[3][3] = 1.0f;
    constants[2][0] = 0.5f; constants[2][1] = 0.25f; constants[2][3] = 1.0f;
    program[0][1] = (1u << 21) | 0x1Bu;
    program[0][2] = 2u << 26;
    program[0][3] = (15u << 12) | 0x800u;
    program[1][1] = (2u << 21) | (2u << 13) | (3u << 9) | 0x1Bu;
    program[1][2] = (2u << 26) | (3u << 11) | (0x1Bu << 17);
    program[1][3] = (15u << 12) | 0x800u | (3u << 3) | 1u;
    valid[0] = valid[1] = 15;
    CHECK("vertex program executes", nv_cpu_vertex_execute(program, valid, 0, attributes, constants, &vertex));
    CHECK("vertex output position", vertex.output[0][0] == 120.0f && vertex.output[0][1] == 80.0f);
    CHECK("vertex shader computes lighting", fabsf(vertex.output[3][0] - 0.4f) < 0.00001f
          && fabsf(vertex.output[3][1] - 0.15f) < 0.00001f);
    valid[1] = 7;
    CHECK("reject incomplete instruction", !nv_cpu_vertex_execute(program, valid, 0, attributes, constants, &vertex));
    valid[1] = 15;
    program[1][1] |= 15u << 21;
    CHECK("reject invalid MAC opcode", !nv_cpu_vertex_execute(program, valid, 0, attributes, constants, &vertex));
    {
        const float expected_x[] = {0, 1, 2, 3, 4, 20, 25, 40, 1, 1, 2, 1, 0};
        uint32_t opcode;
        attributes[0][0] = 1; attributes[0][1] = 2; attributes[0][2] = 3; attributes[0][3] = 4;
        constants[1][0] = 2; constants[1][1] = 3; constants[1][2] = 4; constants[1][3] = 5;
        for (opcode = 1; opcode <= 12; opcode++) {
            memset(program, 0, sizeof program);
            program[0][1] = (opcode << 21) | (1u << 13) | 0x1Bu;
            program[0][2] = (2u << 26) | (3u << 11) | (0x1Bu << 17) | (0x1Bu << 2);
            program[0][3] = (3u << 28) | (15u << 12) | 0x800u | 1u;
            CHECK("MAC arithmetic executes", nv_cpu_vertex_execute(program, valid, 0, attributes, constants, &vertex));
            CHECK("MAC arithmetic value", fabsf(vertex.output[0][0] - expected_x[opcode]) < 0.00001f);
        }
        for (opcode = 1; opcode <= 7; opcode++) {
            program[0][1] = (opcode << 25);
            program[0][2] = 0x1Bu << 2;
            program[0][3] = (2u << 28) | (15u << 12) | 0x800u | 4u | 1u;
            CHECK("ILU arithmetic executes", nv_cpu_vertex_execute(program, valid, 0, attributes, constants, &vertex));
            CHECK("ILU arithmetic finite", isfinite(vertex.output[0][0]) && isfinite(vertex.output[0][3]));
        }
        program[0][1] = (1u << 21) | 0x100u | 0xE4u;
        program[0][2] = 2u << 26;
        program[0][3] = (10u << 12) | 0x800u | (3u << 3) | 1u;
        CHECK("vertex swizzle negate mask", nv_cpu_vertex_execute(program, valid, 0, attributes, constants, &vertex)
              && vertex.output[3][0] == -4 && vertex.output[3][1] == 0
              && vertex.output[3][2] == -2 && vertex.output[3][3] == 1);
        memset(program, 0, sizeof program);
        program[0][1] = (1u << 21) | 0x1Bu;
        program[0][2] = 2u << 26;
        program[0][3] = 15u << 24;
        program[1][1] = (1u << 21) | (1u << 25) | (1u << 13) | 0x1Bu;
        program[1][2] = (3u << 26) | (0x1Bu << 2);
        program[1][3] = (1u << 28) | (15u << 24) | (15u << 16) |
                          (15u << 12) | 0x800u | (3u << 3) | 4u | 1u;
        CHECK("paired units read pre-write register", nv_cpu_vertex_execute(program, valid, 0, attributes, constants, &vertex)
              && vertex.output[3][0] == 1 && vertex.output[3][1] == 2);
        program[0][1] = (13u << 21) | 0x1Bu;
        program[0][3] = 0;
        program[1][1] = (1u << 21) | (1u << 13) | 0x1Bu;
        program[1][2] = 3u << 26;
        program[1][3] = (15u << 12) | 0x800u | 2u | 1u;
        CHECK("relative constant address", nv_cpu_vertex_execute(program, valid, 0, attributes, constants, &vertex)
              && vertex.output[0][0] == 0.5f);
        program[1][1] = (1u << 21) | (191u << 13) | 0x1Bu;
        CHECK("reject out of range relative constant",
              !nv_cpu_vertex_execute(program, valid, 0, attributes, constants, &vertex));
    }
}

static void test_register_combiners(void)
{
    {
        const float coordinates[] = {-2147480000.0f, -128.75f, -1.0f, -0.25f, -0.0f, 0.25f, 128.75f, 2147480000.0f};
        float registers[16][4] = {{0}};
        uint32_t index;
        for (index = 0; index < sizeof coordinates / sizeof coordinates[0]; index++)
            CHECK("coordinate floor matches reference", nv_cpu_floor_coordinate(coordinates[index]) == (int32_t)floorf(coordinates[index]));
        CHECK("finite normal", nv_cpu_finite(1.0f));
        CHECK("finite zero", nv_cpu_finite(-0.0f));
        CHECK("reject infinity", !nv_cpu_finite(INFINITY));
        CHECK("reject NaN", !nv_cpu_finite(NAN));
        CHECK("clamp NaN to lower bound", nv_cpu_clamp(NAN, 0.0f, 1.0f) == 0.0f);
        CHECK("clamp infinity to upper bound", nv_cpu_clamp(INFINITY, 0.0f, 1.0f) == 1.0f);
        registers[4][0] = NAN;
        CHECK("unsigned combiner NaN maps to zero", nv_cpu_combiner_input(registers, 4, 0, 0) == 0.0f);
    }
    Nv2aCpuCombiners state = {0};
    const float diffuse[4] = {0.5f, 0.25f, 0.75f, 0.5f}, specular[4] = {0};
    const float textures[4][4] = {{0.8f, 0.4f, 0.2f, 0.6f}, {0.25f, 0.5f, 1.0f, 0.5f}};
    float result[4];
    printf("test_register_combiners\n");
    state.control = 1;
    state.rgb_input[0] = 0x04080000u;
    state.alpha_input[0] = 0x14180000u;
    state.rgb_output[0] = state.alpha_output[0] = 0xC0u;
    state.final_input[0] = 0x0000000Cu;
    state.final_input[1] = 0x00001C00u;
    CHECK_INT("recognize exact modulation shader", nv_cpu_combiner_fast_mode(&state), 2);
    CHECK("combiner diffuse modulation", nv_cpu_combiners_execute(&state, diffuse, specular, textures, 1, result)
        && fabsf(result[0] - 0.4f) < 0.00001f && fabsf(result[3] - 0.3f) < 0.00001f);
    state.rgb_input[0] = 0x08200000u;
    state.alpha_input[0] = 0x18200000u;
    CHECK_INT("recognize exact replacement shader", nv_cpu_combiner_fast_mode(&state), 1);
    CHECK("combiner texture replacement", nv_cpu_combiners_execute(&state, diffuse, specular, textures, 1, result)
        && fabsf(result[0] - 0.8f) < 0.00001f && fabsf(result[3] - 0.6f) < 0.00001f);
    state.rgb_output[0] = state.alpha_output[0] = 0xC00u;
    state.alpha_input[0] = 0x14200000u;
    state.final_input[1] = 0x00001C80u;
    CHECK_INT("recognize real movie shader", nv_cpu_combiner_fast_mode(&state), 3);
    CHECK("movie shader uses diffuse alpha", nv_cpu_combiners_execute(&state, diffuse, specular, textures, 1, result)
          && fabsf(result[0] - 0.8f) < 0.00001f && fabsf(result[3] - 0.5f) < 0.00001f);
        state.rgb_input[0] = 0x08040000u;
        state.alpha_input[0] = 0x00002014u;
        CHECK_INT("recognize real menu shader", nv_cpu_combiner_fast_mode(&state), 4);
        CHECK("menu shader modulates RGB only", nv_cpu_combiners_execute(&state, diffuse, specular, textures, 1, result)
            && fabsf(result[0] - 0.4f) < 0.00001f && fabsf(result[3] - 0.5f) < 0.00001f);
    state.rgb_input[0] = 0x08090000u;
    state.alpha_input[0] = 0x18190000u;
    CHECK_INT("two texture shader requires general execution", nv_cpu_combiner_fast_mode(&state), 0);
    CHECK("combiner two texture product", nv_cpu_combiners_execute(&state, diffuse, specular, textures, 1, result)
        && fabsf(result[0] - 0.2f) < 0.00001f && fabsf(result[3] - 0.3f) < 0.00001f);
    state.rgb_input[0] = 0x04200000u;
    state.rgb_output[0] = 0x300C0u;
    CHECK("combiner shift-right output encoding", nv_cpu_combiners_execute(&state, diffuse, specular, textures, 1, result)
        && fabsf(result[0] - 0.25f) < 0.00001f);
    state.control = 9;
    CHECK("reject excessive combiner stages", !nv_cpu_combiners_execute(&state, diffuse, specular, textures, 1, result));
    CHECK("depth less", nv_cpu_compare(0x0201, 100, 200) && !nv_cpu_compare(0x0201, 200, 100));
    CHECK("depth equal", nv_cpu_compare(0x0202, 100, 100) && !nv_cpu_compare(0x0202, 100, 200));
    CHECK("depth less equal", nv_cpu_compare(0x0203, 100, 100) && nv_cpu_compare(0x0203, 100, 200));
    CHECK("depth greater", nv_cpu_compare(0x0204, 200, 100) && !nv_cpu_compare(0x0204, 100, 200));
    CHECK("depth not equal", nv_cpu_compare(0x0205, 100, 200) && !nv_cpu_compare(0x0205, 100, 100));
    CHECK("depth greater equal", nv_cpu_compare(0x0206, 100, 100) && nv_cpu_compare(0x0206, 200, 100));
    CHECK("depth never always", !nv_cpu_compare(0x0200, 100, 200) && nv_cpu_compare(0x0207, 100, 200));
    nv_cpu_unpack_normal(1023u | (1025u << 11) | (511u << 22), result);
    CHECK("packed signed normal", result[0] == 1.0f && result[1] == -1.0f && result[2] == 1.0f);
        {
          uint32_t blended;
          CHECK("additive lighting blend", nv_cpu_blend(0xFF201010u, 0xFF102030u, 1, 1, 0x8006, 0, &blended)
              && blended == 0xFF303040u);
          CHECK("source alpha blend", nv_cpu_blend(0x80FF0000u, 0xFF0000FFu, 0x0302, 0x0303, 0x8006, 0, &blended)
              && blended == 0xBF80007Fu);
          CHECK("destination color blend", nv_cpu_blend(0xFF808080u, 0xFF808080u, 0x0306, 0, 0x8006, 0, &blended)
              && blended == 0xFF404040u);
          CHECK("reject unsupported blend equation", !nv_cpu_blend(0, 0, 1, 1, 0xFFFF, 0, &blended));
        }
    CHECK("top-left shared edge owned once",
          nv_cpu_edge_inside(0, 0, -1, 1) && !nv_cpu_edge_inside(0, 0, 1, 1));
    CHECK("top-left rule supports reverse winding",
          nv_cpu_edge_inside(0, 0, 1, -1) && !nv_cpu_edge_inside(0, 0, -1, -1));
}

int main(void)
{
    int i;
    printf("d3d8_smoke: running\n");
    for (i = 0; i < 256; i++) g_palette[i] = 0xFF000000u | i;

    test_to_dxgi();
    test_bpp();
    test_predicates();
    test_swizzle_classification();
    test_unswizzle_roundtrip();
    test_swizzle_offset();
    test_dxt_decode();
    test_convert_linear();
    test_fvf_position();
    test_vertex_program();
    test_register_combiners();

    if (failures == 0) {
        printf("d3d8_smoke: ALL PASS\n");
        return 0;
    }
    printf("d3d8_smoke: %d FAILURE(S)\n", failures);
    return 1;
}
