/**
 * Execute the parts of the title's pushbuffer that produce visible pixels.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; without
 * something consuming them the framebuffer stays whatever it was, which is how
 * a fully booted title renders a black screen. This walks the same command
 * stream nv2a_pb_scan.c surveys and carries out the subset that decides what is
 * on screen: which surface is being drawn into, and clearing it.
 *
 * It also rasterises geometry, but only the part that can be drawn honestly:
 * batches whose attribute 0 is already in screen space, flat-shaded, straight
 * into the same guest framebuffer the clear writes. Titles draw their UI, HUD
 * and 2D overlays that way, so it is the first geometry to appear. Batches that
 * need a vertex program executed are counted and skipped rather than drawn
 * somewhere wrong -- see raster_batch(). Texturing, depth and vertex programs
 * are still a renderer, not a command decoder; the upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c.
 *
 * Everything this does not handle is counted and ranked by
 * nv2a_pb_exec_report(), so what remains is a list rather than a guess.
 *
 * Enabled with RECOMP_PB_EXEC. RECOMP_RASTER_TEST draws one known triangle
 * after every clear, which separates "the pixel path is broken" from "the title
 * has not given us any vertices". RECOMP_FB_DUMP=<prefix> writes the surface to
 * <prefix>NNN.bmp, so the result can be looked at without a display.
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernel.h"   /* XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE */
#include "xbox_memory_layout.h"   /* xbox_Nv2aFrameCounterFlip */
/* The swizzle decoder the D3D8 layer already uses -- one implementation of
 * Morton order, not a second one that can disagree with it. */
#include "../d3d/d3d8_swizzle.h"
#include "../d3d/nv2a_shader_cpu.h"
#ifdef _WIN32
#include "nv2a_gpu.h"
#endif

void nv2a_pb_exec_flush(void)
{
#ifdef _WIN32
    nv2a_gpu_flush();
#endif
}

/* Ordering events (WAIT_FOR_IDLE, semaphore release) only need the GPU to have
 * finished; they do not read surfaces back. They fire tens of times per frame,
 * so a full publication on each was most of the sync cost.
 *
 * Safe because the texture path no longer re-uploads a dirty surface from guest
 * RAM: refresh_surface is skipped while the surface holds unpublishsed GPU
 * content (nv2a_gpu_d3d11.cpp, the "direct" branch). Without that this stalled
 * the title at the video->menu transition; with it, the menu runs ~30-35 FPS
 * instead of ~12-18. Set RECOMP_SYNC_LIGHT=0 to restore full publication on
 * every ordering event. */
static int pb_sync_light(void)
{
    static int on = -1;
    if (on < 0) {
        const char *value = getenv("RECOMP_SYNC_LIGHT");
        on = (value && value[0] == '0') ? 0 : 1;
    }
    return on;
}

static void pb_flush_ordering(void)
{
#ifdef _WIN32
    if (pb_sync_light()) { nv2a_gpu_wait(); return; }
#endif
    nv2a_pb_exec_flush();
}

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowStart(void);
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;

/* Would writing this surface land on the title's own image?
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset within the colour DMA object,
 * not a guest virtual address, and this executor has always used it as one.
 * That is harmless while the two happen to agree and catastrophic when they do
 * not: the Xbox Dashboard names surface 0x00088000 at 1280x960x4, so clearing
 * it wrote 4.9 MB of opaque black from 0x00088000 to 0x00538000 -- straight
 * over its own code, its D3D context at 0x000BBFC0 and the register-block
 * pointer at 0x000BE2C4. The symptom was a title that submitted one perfect
 * frame and then spun forever in a pushbuffer-full loop, three layers away,
 * with every D3D global reading 0xFF000000: the clear colour.
 *
 * So refuse, and say so. Getting the address right needs the DMA object base
 * this ignores (NV097_SET_CONTEXT_DMA_COLOR); until that exists, writing
 * nothing is strictly better than writing over the guest, and a title that
 * cannot draw is easier to debug than one that has been overwritten.
 */
static int surface_hits_image(uint32_t base, uint32_t bytes)
{
    if (!g_xbox_image_hi || !bytes)
        return 0;
    return base < g_xbox_image_hi && base + bytes > g_xbox_image_lo;
}

/* Where a DMA-object offset actually lives.
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset inside the colour DMA object,
 * and for a framebuffer that object covers physical memory -- so the offset
 * is a physical address, not a guest VA. Those are the same number in this
 * runtime, which is why treating it as a VA works until it does not: on
 * hardware the image is mapped at VA 0x00010000 from arbitrary physical
 * pages, so a framebuffer at physical 0x84000 does not overlap it. Here it
 * would.
 *
 * The title tells us which it is by where it allocated. Half-Life 2's
 * framebuffer comes from MmAllocateContiguousMemory, which this runtime
 * serves from the window at XBOX_CONTIG_BASE, so physical P is visible at
 * XBOX_CONTIG_BASE + P -- clear of the image, and the same bytes the title's
 * own writes and the framebuffer window reach.
 *
 * So: use the offset as a VA when that is credible, and fall back to the
 * physical mirror exactly when it is not. Titles whose surfaces already sit
 * in ordinary RAM (Wreckless renders to the tiled alias of physical
 * 0x01954000) keep the first path and are unaffected.
 */
static uint32_t dma_resolve(uint32_t offset)
{
    /* Did this runtime hand the offset out as contiguous memory? Then the
     * bytes live in the window. Check live allocation ownership rather than
     * a high-water mark: freed pages and alignment gaps are not allocations. The
     * title's own writes go through the window, so the executor's must too.
     *
     * Checking this BEFORE the image test is the whole point. The image test
     * only catches an offset that would land on the title's code, and whether
     * it does is an accident of where the image happens to end: Half-Life 2's
     * colour surface is physical 0x00A6C000, which clears the image by 700 KB.
     * So it looked like an ordinary VA, and the executor cleared 1.2 MB of
     * black straight through the guest heap -- which faulted the title three
     * frames later on a pointer that had been overwritten, while the real
     * framebuffer at 0x80A6C000 stayed untouched and the screen stayed black. */
    if (offset < XBOX_CONTIG_SIZE
            && xbox_ContiguousBlockSize(XBOX_CONTIG_BASE + offset))
        return XBOX_CONTIG_BASE + offset;
    if (!surface_hits_image(offset, 1))
        return offset;
    if ((uint64_t)offset < XBOX_CONTIG_SIZE)
        return XBOX_CONTIG_BASE + offset;
    return offset;                         /* nothing better to offer */
}

static int surface_write_refused(uint32_t base, uint32_t bytes, const char *what)
{
    static int said;

    if (!surface_hits_image(base, bytes))
        return 0;
    if (!said) {
        said = 1;
        fprintf(stderr,
                "  [GPU] REFUSING to %s surface 0x%08X..0x%08X: that overlaps "
                "the loaded image (0x%08X..0x%08X).\n"
                "  [GPU]   SET_SURFACE_COLOR_OFFSET is a DMA-object offset, not "
                "a guest VA, and this executor treats it as one. Writing here "
                "would destroy the title's own code and globals.\n",
                what, base, base + bytes, g_xbox_image_lo, g_xbox_image_hi);
        fflush(stderr);
    }
    return 1;
}

/* NV097 methods this executor acts on. */
/* Blending. The pair this title programs, read from its own pushbuffer
 * rather than guessed: BLEND_ENABLE written 1168 times and left on,
 * SFACTOR 0x0302 (SRC_ALPHA) and DFACTOR 0x0303 (ONE_MINUS_SRC_ALPHA).
 * ALPHA_TEST_ENABLE is written 390 times and left at zero, so this is
 * blending and not an alpha test. */
#define NV097_WAIT_FOR_IDLE               0x0110
#define NV097_SET_BLEND_ENABLE            0x0304
#define NV097_SET_BLEND_FUNC_SFACTOR      0x0344
#define NV097_SET_BLEND_FUNC_DFACTOR      0x0348
#define NV_BLEND_SRC_ALPHA                0x0302
#define NV_BLEND_ONE_MINUS_SRC_ALPHA      0x0303

#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200
#define NV097_SET_SURFACE_CLIP_VERTICAL   0x0204
#define NV097_SET_SURFACE_FORMAT          0x0208
#define NV097_SET_SURFACE_PITCH           0x020C
#define NV097_SET_SURFACE_COLOR_OFFSET    0x0210
#define NV097_SET_COLOR_CLEAR_VALUE       0x1D90
#define NV097_CLEAR_SURFACE               0x1D94
#define NV097_SET_VERTEX_DATA_ARRAY_OFFSET 0x1720   /* +i*4, 16 attributes */
#define NV097_SET_VERTEX_DATA_ARRAY_FORMAT 0x1760   /* +i*4 */
#define NV097_SET_BEGIN_END               0x17FC
#define NV097_SET_TEXTURE_OFFSET          0x1B00   /* +i*0x40 */
#define NV097_SET_TEXTURE_FORMAT          0x1B04
#define NV097_SET_TEXTURE_ADDRESS         0x1B08
#define NV097_SET_TEXTURE_CONTROL1        0x1B10
#define NV097_SET_TEXTURE_IMAGE_RECT      0x1B1C
/* Benign render state the executor classifies but does not act on. */
#define NV097_SET_DITHER_ENABLE           0x0310
#define NV097_SET_POINT_PARAMS_ENABLE     0x0318
#define NV097_SET_POINT_SMOOTH_ENABLE     0x031C
#define NV097_SET_SWATH_WIDTH             0x09F8
#define NV097_SET_STIPPLE_ENABLE          0x147C
#define NV097_SET_ANTI_ALIASING_CONTROL   0x1D7C
#define NV097_SET_OCCLUDE_ZSTENCIL_EN     0x1D84
#define NV097_SET_CLEAR_RECT_HORIZONTAL   0x1D98
#define NV097_SET_CLEAR_RECT_VERTICAL     0x1D9C
#define NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN 0x1E98
/* The buffer flip. A title double-buffers by telling the GPU which buffer
 * the CRTC reads and which it draws into, advancing the write index and
 * then stalling until the flip has happened. Ignoring these means the
 * stall never clears: Half-Life 2's loader submits its initialisation,
 * asks for a flip, and waits for it in a loop that makes no kernel calls
 * and burns no dispatch, which reads as a hang with no cause.
 *
 * ponytail: the flip completes the moment it is asked for, because there is
 * no scanout to be in the middle of. That makes every frame land instantly
 * and a title that paces itself on the flip runs as fast as it can draw.
 * Pacing wants the vblank clock in the kernel, not a sleep in here. */
#define NV097_SET_FLIP_READ               0x0120
#define NV097_SET_FLIP_WRITE              0x0124
#define NV097_SET_FLIP_MODULO             0x0128
#define NV097_FLIP_INCREMENT_WRITE        0x012C
#define NV097_FLIP_STALL                  0x0130
#define NV097_ARRAY_ELEMENT16             0x1800
#define NV097_ARRAY_ELEMENT32             0x1808
/* Draw a run of vertices straight out of the arrays, with no index list:
 * bits 0..23 are the first vertex, bits 24..31 the count minus one. It may
 * appear several times inside one BEGIN_END to draw a longer run. */
#define NV097_DRAW_ARRAYS                 0x1810
#define NV097_INLINE_ARRAY                0x1818
/* Immediate-mode vertices. SET_VERTEX3F/4F carry the position, and writing
 * its last component completes a vertex using whatever the SET_VERTEX_DATA*
 * registers currently hold for the other attributes. This is how Half-Life
 * 2's Xbox loader and the game's own 2D drawing submit every quad -- neither
 * uses INLINE_ARRAY -- so without these the executor saw SET_BEGIN_END pairs
 * with nothing attached and reported `draws 0` while a million and a half
 * textured quads a minute went past it. */
#define NV097_SET_VERTEX3F                0x1500   /* +0..0x08, 3 floats */
#define NV097_SET_VERTEX4F                0x1518   /* +0..0x0C, 4 floats */
#define NV097_SET_VERTEX_DATA2F_M         0x1880   /* + attr*8,  2 floats */
#define NV097_SET_VERTEX_DATA4F_M         0x1A00   /* + attr*16, 4 floats */
#define NV097_SET_VERTEX_DATA4UB          0x1940

/* One immediate vertex, as this file packs it for the shared draw path:
 * position float4, diffuse D3DCOLOR, texcoord0 float2. */
#define IMM_VERTEX_DWORDS 7

#define NV097_CLEAR_COLOR_MASK            0xF0   /* R,G,B,A bits */

/* One vertex attribute stream, as the title describes it. Attribute 0 is
 * position; the rest are colours, texture coordinates and so on. */
typedef struct {
    uint32_t offset;      /* guest address of element 0 */
    uint32_t type;        /* NV097 data type nibble */
    uint32_t size;        /* components per element */
    uint32_t stride;      /* bytes between elements */
} VertexAttr;

#define NV_VERTEX_ATTRS 16
#define NV_VERTEX_CACHE_SIZE 4096
#define NV_MAX_INLINE   4096            /* dwords of INLINE_ARRAY per batch */

/* Texture stage 0, decoded from what the title programmed.
 *
 * Only stage 0: it is the only one the dashboard configures, and a stage
 * nothing writes to is a stage nothing can be sampled from. The rest arrive
 * as unhandled methods and are counted as such, which is how the next title
 * that needs them will say so. */
typedef struct {
    uint32_t offset;                    /* resolved guest address of texel (0,0) */
    uint32_t raw_offset;                /* address as written by the pushbuffer */
    uint32_t width, height;             /* from IMAGE_RECT               */
    uint32_t pitch;                     /* bytes per row, from CONTROL1  */
    uint32_t color;                     /* NV097 colour-format code      */
    uint32_t raw_format;
    uint32_t addr_u, addr_v;            /* wrap mode per axis            */
    uint32_t addr_w, filter, control0, control0_valid, border_color;
    uint32_t color_key;
    float bump_matrix[4], bump_scale, bump_offset;
    int      valid;
} Texture;

static struct {
    VertexAttr attr[NV_VERTEX_ATTRS];
    float uniform_attr[NV_VERTEX_ATTRS][4];
    uint32_t uniform_valid;
    uint32_t   prim;                    /* SET_BEGIN_END parameter, 0 = ended */
    uint32_t   *idx;
    uint32_t   idx_count, idx_capacity, max_index_count;
    uint32_t   batch_array32_count;
    uint64_t   array16_indices, array32_indices, draw_array_indices, wide_indices;
    uint64_t   native_array32_batches;
    /* INLINE_ARRAY payload: vertices written straight into the pushbuffer
     * instead of into a buffer the title points at. Same vertex format, a
     * different place to read them from. */
    uint32_t   inline_buf[NV_MAX_INLINE];
    uint32_t   inline_count;
    /* Current values of the immediate-mode attributes, and how many complete
     * vertices they have produced in this batch. */
    float      imm_pos[4];
    uint32_t   imm_diffuse;
    float      imm_tex[2];
    uint32_t   imm_count;
    float      imm_attributes[NV_MAX_INLINE / IMM_VERTEX_DWORDS][NV_VERTEX_ATTRS][4];
    int        immediate_active;
    int        inline_active;
    uint32_t   draws, verts, nonzero_draws;
    float      min_x, max_x, min_y, max_y;
    uint32_t color_offset, color_base, pitch, format;
    /* The surface the last batch actually drew into. A double-buffered title
     * has already pointed color_offset at the next buffer and cleared it by
     * the time the flip arrives, so dumping the current one dumps the frame
     * that has not been drawn yet -- which is how a correctly rendered
     * sequence came out as 12 black BMPs. */
    uint32_t drawn_offset;
    uint64_t pixels;
    uint32_t pixel_max;   /* brightest value any pixel write carried */
    uint32_t clip_x, clip_w, clip_y, clip_h;
    uint32_t window_clip_valid, window_clip_type;
    uint32_t window_clip_horizontal[8], window_clip_vertical[8];
    uint32_t clear_color;
    /* Clear rectangle (SET_CLEAR_RECT_*): the region CLEAR_SURFACE is meant to
     * touch. Recorded, not applied -- the software clear uses the surface clip,
     * which the title sets to the same region when it matters.
     * ponytail: honour clear_rect_* here if a title is ever seen clearing a
     * sub-region it expects the rest of the surface to survive. */
    uint32_t clear_rect_x, clear_rect_w, clear_rect_y, clear_rect_h;
    /* Benign render state captured so it is classified rather than counted.
     * The software rasteriser does not act on any of it. */
    uint32_t anti_alias_control, swath_width, stipple_enable, occlude_zstencil_en;
    uint32_t point_params_enable, point_smooth_enable, dither_enable;
    uint32_t transform_cxt_write_en;
    uint32_t clears, unhandled_total;
    /* A flush is a full GPU->CPU readback, so which guest event drives it says
     * where to make it cheaper. See nv2a_pb_exec_report. */
    uint32_t flush_flip, flush_semaphore, flush_wait_idle, flush_fence,
             flush_clear, flush_raster, flush_report, flush_present;
    uint32_t semaphore_context, semaphore_offset;
    uint32_t flip_read, flip_write, flip_modulo, flips;
    uint32_t tris_drawn, tris_skipped_offscreen, batches_untransformed;
    /* Why a batch came out flat. "Untextured" has two causes that look
     * identical on screen and want opposite fixes: the batch carried no
     * texture coordinates, or it did and the stage was not usable. */
    uint32_t batches_textured, batches_no_uv, batches_no_tex;
    uint32_t blend_enable, blend_sfactor, blend_dfactor;
    uint32_t blend_equation, blend_constant, color_mask;
    uint32_t combiner_control, shader_stage_program, transform_execution_mode;
    uint32_t shader_clip_mode, shader_other_stage_input;
    uint32_t shader_dot_mapping;
    float shader_eye_vector[3];
    uint32_t shader_eye_vector_valid;
    uint32_t vp_program[136][4], vp_valid[136];
    float vp_constants[192][4], viewport_offset[4], viewport_scale[4];
    uint32_t vp_load, vp_start, constant_load, vp_words, constant_words;
    Nv2aCpuCombiners combiners;
    uint32_t shade_mode, alpha_enable, alpha_func, alpha_ref;
    uint32_t depth_offset, depth_pitch, depth_clear, depth_enable, depth_func, depth_mask, control0;
    uint32_t zmin_max_control, depth_range_valid;
    float depth_clip_min, depth_clip_max;
    uint32_t fog_enable, fog_mode;
    float fog_parameters[3];
    uint32_t fog_gen_mode;
    float fog_plane[4];
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_read_mask, stencil_write_mask;
    uint32_t stencil_fail, stencil_depth_fail, stencil_pass;
    uint32_t cull_enable, cull_face, front_face, polygon_front, polygon_back, point_size;
    uint32_t polygon_offset_enable;
    float polygon_offset_scale, polygon_offset_bias;
    uint32_t depth_rejected, depth_written;
    /* Fixed-function lighting. Only consulted when transform_execution_mode
     * selects the fixed-function transform (not a vertex program); vertex
     * programs light via microcode instead. */
    uint32_t lighting_enable, specular_enable, light_enable_mask, color_material, light_control;
    uint32_t normalization_enable, skin_mode, texgen_view_model;
    uint32_t texgen[4][4], texture_matrix_enable[4];
    float material_alpha, specular_power;
    float specular_params[6];
    float scene_ambient[4], material_emission[4];
    float light_ambient[8][4], light_diffuse[8][4], light_specular[8][4];
    float light_local_position[8][4];    /* xyz = position, w = range */
    float light_local_attenuation[8][4]; /* xyz = constant/linear/quadratic coefficients */
    float light_infinite_direction[8][4];
    float light_infinite_half_vector[8][4];
    float light_spot_direction[8][4];    /* xyz = direction, w = spot cone parameter */
    uint32_t shader_vertices, shader_failures, shader_pixels;
    uint32_t composite_valid;
    float transform_data[4];
    uint32_t state_shader_runs, state_shader_failures;
    double raster_seconds;
    uint64_t gpu_attribute_fetches, gpu_attribute_skips, gpu_zero_bytes_saved;
    double gpu_prepare_seconds, gpu_submit_seconds;
    uint32_t gpu_batches, cpu_batches;
    int combiners_configured;
    Texture extra_tex[3];
    Texture  tex;
} s_gpu = {.cull_face = 0x405, .front_face = 0x900, .polygon_front = 0x1B02, .polygon_back = 0x1B02, .point_size = 8, .specular_power = 1.0f};

/* Unhandled methods, ranked. The interesting output is not that something was
 * skipped but which things dominate, because that is the order to implement
 * them in. */
#define PB_EXEC_MAX_UNHANDLED 2048
typedef struct { uint32_t subchannel, method, count, last_param; } PbUnhandled;
static PbUnhandled s_unhandled[PB_EXEC_MAX_UNHANDLED];
static int s_unhandled_count;

int xbox_Nv2aNativeFencesEnabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("RECOMP_NV2A_NATIVE_FENCES");
        enabled = value != NULL && strcmp(value, "0") != 0;
    }
    return enabled;
}

static int ramht_instance(uint32_t handle, uint32_t *instance)
{
    volatile uint32_t *fifo = xbox_Nv2aRegisterPointer(0x2000, 0x1208);
    volatile uint32_t *ramin = xbox_Nv2aRegisterPointer(0x700000, 0x100000);
    if (!fifo || !ramin)
        goto failure;
    uint32_t ramht = fifo[0x210 / 4];
    uint32_t first = ((ramht & 0x1F0u) << 8) / 4;
    uint32_t count = (4096u << ((ramht >> 16) & 3u)) / 4;
    uint32_t channel = fifo[0x1204 / 4] & 31u;
    for (uint32_t i = first; i < first + count; i += 2) {
        uint32_t candidate = ramin[i + 1];
        if (ramin[i] == handle &&
                (candidate & 0x80000000u) &&
                ((candidate >> 24) & 31u) == channel) {
            *instance = (candidate & 0xFFFFu) << 4;
            return 1;
        }
    }
failure:
    fprintf(stderr, "[GPU] RAMHT object unavailable: handle 0x%08X\n", handle);
    return 0;
}

static void semaphore_release(uint32_t value)
{
    uint32_t instance;
    if (!ramht_instance(s_gpu.semaphore_context, &instance))
        goto failure;
    volatile uint32_t *descriptor = xbox_Nv2aRegisterPointer(0x700000 + instance, 12);
    if (!descriptor)
        goto failure;
    uint32_t flags = descriptor[0];
    uint32_t limit = descriptor[1];
    uint32_t frame = descriptor[2];
    uint32_t dma_class = flags & 0xFFFu;
    uint32_t target = flags & 0x30000u;
    uint64_t address = (frame & 0xFFFFF000u) + (flags >> 20);
    address += s_gpu.semaphore_offset;
    if ((dma_class != 0x03u && dma_class != 0x3Du) ||
            (target != 0 && target != 0x20000u) || (address & 3u) ||
            (uint64_t)s_gpu.semaphore_offset + 4 > (uint64_t)limit + 1) {
        fprintf(stderr, "[GPU] invalid semaphore DMA descriptor: instance 0x%05X flags 0x%08X limit 0x%08X frame 0x%08X\n",
                instance, flags, limit, frame);
        goto failure;
    }
    uint8_t *destination = xbox_DmaPhysicalPointer(address, 4);
    if (!destination)
        goto failure;
    /* Retire the command's value, not a concurrently advanced CPU counter. */
    s_gpu.flush_semaphore++;
    pb_flush_ordering();
    *(volatile uint32_t *)destination = value;
    return;

failure:
    fprintf(stderr, "[GPU] semaphore release failed: handle 0x%08X offset 0x%08X value 0x%08X\n",
            s_gpu.semaphore_context, s_gpu.semaphore_offset, value);
    fflush(stderr);
    _Exit(EXIT_FAILURE);
}

/* Every texture-stage register, as the title last set it.
 * Texturing is not implemented yet; knowing which formats and sizes a title
 * actually programs is what decides which ones are worth implementing. */
#define NV_TEX_FIRST 0x1B00
#define NV_TEX_LAST  0x1BFC
static uint32_t s_tex_reg[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];
static uint8_t  s_tex_set[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];

/* Formats whose dimensions come from the format word and whose coordinates
 * arrive normalised, rather than from a pitch and SET_TEXTURE_IMAGE_RECT with
 * coordinates in texels. Swizzled and block-compressed are both in this group,
 * and every place that used to test only for swizzled needs the pair. */
static int tex_size_from_format(uint32_t fmt)
{
    return d3d8_format_is_swizzled(fmt) || d3d8_format_dxt_block_bytes(fmt);
}

static void record_tex_reg(uint32_t method, uint32_t param)
{
    s_tex_reg[(method - NV_TEX_FIRST) / 4] = param;
    s_tex_set[(method - NV_TEX_FIRST) / 4] = 1;
    /* A pitch is a linear texture's property. A swizzled one has no rows and
     * so no pitch, and requiring one here refused every swizzled texture --
     * which is nearly all of them, since swizzled is the Xbox default. That
     * left the title's own textures unsampled and every textured quad drawn in
     * flat vertex colour. */
    s_gpu.tex.valid = s_gpu.tex.offset && s_gpu.tex.width && s_gpu.tex.height
                   && (tex_size_from_format(s_gpu.tex.color)
                       || s_gpu.tex.pitch);
}

/* Every distinct texture a batch was drawn with, and how many batches used it.
 *
 * The per-draw verbose print shows the first few draws of the first frame,
 * which is enough to see that texturing works at all and not enough to answer
 * "is a font page ever bound". This is the same shape as the unhandled-method
 * table below it: a small set, ranked, printed with the rest of the report. */
#define PB_EXEC_MAX_TEXTURES 64
typedef struct {
    uint32_t offset, color, width, height, batches;
} PbTexUse;
static PbTexUse s_tex_use[PB_EXEC_MAX_TEXTURES];
static int s_tex_use_count;

/* Defined below, next to the sampler it goes through. */
static void dump_texture_bmp(uint32_t seq);

/* Repeat dumps are numbered from well past the first-use sequence, so a
 * listing sorts them after the textures they came from and no first-use file
 * is ever overwritten by one. */
#define TEX_DUMP_SEQ_BASE 1000u
#define TEX_DUMP_SEQ_MAX  40u

static void note_texture_use(void)
{
    int i;

    if (!s_gpu.tex.valid)
        return;
    for (i = 0; i < s_tex_use_count; i++) {
        if (s_tex_use[i].offset == s_gpu.tex.offset
         && s_tex_use[i].color  == s_gpu.tex.color) {
            s_tex_use[i].batches++;
            return;
        }
    }
    if (s_tex_use_count < PB_EXEC_MAX_TEXTURES) {
        s_tex_use[s_tex_use_count].offset  = s_gpu.tex.offset;
        s_tex_use[s_tex_use_count].color   = s_gpu.tex.color;
        s_tex_use[s_tex_use_count].width   = s_gpu.tex.width;
        s_tex_use[s_tex_use_count].height  = s_gpu.tex.height;
        s_tex_use[s_tex_use_count].batches = 1;
        s_tex_use_count++;
    }
}

static void note_unhandled(uint32_t subchannel, uint32_t method, uint32_t param)
{
    int i;

    s_gpu.unhandled_total++;
    for (i = 0; i < s_unhandled_count; i++) {
        if (s_unhandled[i].subchannel == subchannel && s_unhandled[i].method == method) {
            s_unhandled[i].count++;
            s_unhandled[i].last_param = param;
            return;
        }
    }
    if (s_unhandled_count < PB_EXEC_MAX_UNHANDLED) {
        s_unhandled[s_unhandled_count].subchannel = subchannel;
        s_unhandled[s_unhandled_count].method = method;
        s_unhandled[s_unhandled_count].count = 1;
        s_unhandled[s_unhandled_count].last_param = param;
        s_unhandled_count++;
    }
}

static uint32_t vertex_attribute_bytes(const VertexAttr *a)
{
    uint32_t components = a->size < 4 ? a->size : 4;
    switch (a->type) {
    case 0: case 6: return 4;
    case 1: case 5: return components * 2;
    case 2: return components * 4;
    case 4: return components;
    default: return 0;
    }
}

/* Read an array, inline or immediate attribute using the same format decoder. */
static int fetch_attr(const VertexAttr *a, uint32_t index, float out[4])
{
    const uint8_t *p;
    uint32_t i, bytes;
    uint64_t at, limit;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (s_gpu.immediate_active) {
        uintptr_t slot_bytes = (uintptr_t)a - (uintptr_t)s_gpu.attr;
        if (index < s_gpu.imm_count && slot_bytes < sizeof s_gpu.attr && slot_bytes % sizeof *a == 0) {
            memcpy(out, s_gpu.imm_attributes[index][slot_bytes / sizeof *a], sizeof(float) * 4);
            return 1;
        }
        return 0;
    }
    if (!a->size || !a->stride) {
        uintptr_t slot_bytes = (uintptr_t)a - (uintptr_t)s_gpu.attr;
        if (slot_bytes < sizeof s_gpu.attr && slot_bytes % sizeof *a == 0) {
            uint32_t attribute = (uint32_t)(slot_bytes / sizeof *a);
            if (s_gpu.uniform_valid & (1u << attribute)) {
                memcpy(out, s_gpu.uniform_attr[attribute], sizeof(float) * 4);
                return 1;
            }
        }
        return 0;
    }
    bytes = vertex_attribute_bytes(a);
    if (!bytes) {
        fprintf(stderr, "[GPU] unsupported vertex attribute type %u size %u\n", a->type, a->size);
        fflush(stderr);
        _Exit(EXIT_FAILURE);
    }
    at = (uint64_t)a->offset + (uint64_t)index * a->stride;
    if (s_gpu.inline_active) {
        /* The batch arrived as INLINE_ARRAY, so `offset` is a byte offset into
         * the buffered payload rather than a guest address -- and 0 is a legal
         * one there, which is why the offset test is on the other side of this
         * branch. */
        limit = (uint64_t)s_gpu.inline_count * 4;
    } else {
        if (!a->offset)
            return 0;
        if (a->offset >= XBOX_CONTIG_BASE && a->offset < XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
            limit = (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE;
        else
            limit = (uint64_t)(g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram) * (1 + XBOX_NUM_MIRRORS);
        if (limit > (uint64_t)UINT32_MAX + 1u) limit = (uint64_t)UINT32_MAX + 1u;
    }
    if (at + bytes > limit) {
        fprintf(stderr, "[GPU] vertex attribute extent rejected: index %u offset %08X stride %u bytes %u\n",
                index, a->offset, a->stride, bytes);
        fflush(stderr);
        _Exit(EXIT_FAILURE);
    }
    p = s_gpu.inline_active ? (const uint8_t *)s_gpu.inline_buf + (size_t)at :
                             (const uint8_t *)xbox_GetMemoryOffset() + (size_t)at;

    switch (a->type) {
    case 1:
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = fmaxf(-1.0f, (float)((const int16_t *)p)[i] / 32767.0f);
        return 1;
    case 5:
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i];
        return 1;
    case 6:
        nv_cpu_unpack_normal(*(const uint32_t *)p, out);
        return 1;
    case 0:                                  /* D3DCOLOR */
        /* A DWORD 0xAARRGGBB, so little-endian bytes are B,G,R,A -- not the
         * component order of every other format here. Returned as R,G,B,A so
         * callers need not know which format the title chose. */
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case 2:                                  /* float */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        return 1;
    case 4:                                  /* unsigned byte, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    default:
        return 0;
    }
}

static uint32_t surface_bpp(void)
{
    switch (s_gpu.format & 15u) {
    case 1: case 2: case 3: case 10: return 2;
    case 4: case 5: case 6: case 7: case 8: return 4;
    case 9: return 1;
    default: return 0;
    }
}


/* Write the current surface out as a 24-bit BMP.
 *
 * A framebuffer window needs someone watching it. A file does not, which makes
 * this the only way to check what a title actually rendered on a machine you
 * are not sitting at -- and the only way to put a picture in a bug report.
 *
 * ponytail: bottom-up 24bpp BMP, no palette, no compression. That is the one
 * format every viewer reads and it is 30 lines; PNG would need a dependency.
 */
static void dump_surface_bmp(void)
{
    const char *prefix = getenv("RECOMP_FB_DUMP");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    static int seq;
    char path[512];
    uint32_t w = s_gpu.clip_w, h = s_gpu.clip_h, y, x;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    FILE *f;

    if (!prefix || !w || !h || (bpp != 2 && bpp != 4) || !s_gpu.color_offset)
        return;

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%03d.bmp", prefix, seq++);
    f = fopen(path, "wb");
    if (!f)
        return;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    /* BMP rows run bottom-up. */
    for (y = h; y-- > 0; ) {
        const uint8_t *row = mem + dma_resolve(s_gpu.drawn_offset
                                                ? s_gpu.drawn_offset
                                                : s_gpu.color_offset)
                           + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        for (x = 0; x < w; x++) {
            uint8_t bgr[3];
            if (bpp == 4) {
                uint32_t v = ((const uint32_t *)row)[s_gpu.clip_x + x];
                bgr[0] = (uint8_t)(v);
                bgr[1] = (uint8_t)(v >> 8);
                bgr[2] = (uint8_t)(v >> 16);
            } else {
                uint16_t v = ((const uint16_t *)row)[s_gpu.clip_x + x];
                bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                bgr[1] = (uint8_t)(((v >>  5) & 0x3F) << 2);
                bgr[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            }
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    if (seq == 1)
        fprintf(stderr, "  [GPU] framebuffer dump: %s (%ux%u from 0x%08X %ubpp)\n",
                path, w, h, s_gpu.color_offset, bpp);
}

/* Defined below, next to the rest of the rasteriser; the clear path uses it
 * for RECOMP_RASTER_TEST. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2], const Nv2aCpuVertex varying[3]);

#ifdef _WIN32
static int gpu_clear_surface(uint32_t param)
{
    static int enabled = -1;
    Nv2aGpuDraw state = {0};
    uint8_t *memory = (uint8_t *)xbox_GetMemoryOffset();
    int result;
    if (enabled < 0) {
        const char *setting = getenv("RECOMP_NV2A_NATIVE_CLEARS");
        enabled = !setting || strcmp(setting, "0") != 0;
    }
    if (!enabled || s_gpu.clip_x || s_gpu.clip_y || !s_gpu.clip_w || !s_gpu.clip_h)
        return 0;
    state.width = state.clip_width = s_gpu.clip_w;
    state.height = state.clip_height = s_gpu.clip_h;
    state.bytes_per_pixel = surface_bpp();
    state.pitch = s_gpu.pitch;
    state.depth_pitch = s_gpu.depth_pitch;
    state.depth_format = (s_gpu.format >> 4) & 15u;
    state.control0 = s_gpu.control0;
    if (param & NV097_CLEAR_COLOR_MASK) {
        uint32_t base = dma_resolve(s_gpu.color_offset);
        if (!s_gpu.color_offset || !state.pitch || !state.bytes_per_pixel ||
            state.width > state.pitch / state.bytes_per_pixel ||
            surface_write_refused(base, state.height * state.pitch, "clear")) return 0;
        state.color = memory + base;
    }
    if (param & 3u) {
        uint32_t base = dma_resolve(s_gpu.depth_offset);
        uint32_t bytes = state.depth_format == 1 ? 2u : state.depth_format == 2 ? 4u : 0u;
        if (!s_gpu.depth_offset || !state.depth_pitch || !bytes || (state.control0 & 0x1000u) ||
            state.width > state.depth_pitch / bytes ||
            surface_write_refused(base, state.height * state.depth_pitch, "depth clear")) return 0;
        state.depth = memory + base;
    }
    result = nv2a_gpu_clear(&state, param, s_gpu.clear_color, s_gpu.depth_clear);
    if (result < 0) {
        fprintf(stderr, "  [GPU] native clear failed: flags 0x%08X, color 0x%08X, depth 0x%08X\n",
                param, s_gpu.color_offset, s_gpu.depth_offset);
        fflush(stderr);
        exit(EXIT_FAILURE);
    }
    return result;
}
#endif

static void clear_surface(uint32_t param)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    uint32_t y, x, width;
    int native_clear = 0;

#ifdef _WIN32
    native_clear = gpu_clear_surface(param);
#endif
    if (!native_clear) { s_gpu.flush_clear++; nv2a_pb_exec_flush(); }

    if (!native_clear && (param & 3u) && s_gpu.depth_offset && s_gpu.depth_pitch) {
        uint32_t format = (s_gpu.format >> 4) & 15u;
        uint32_t depth_bpp = format == 1 ? 2u : format == 2 ? 4u : 0u;
        uint32_t base = dma_resolve(s_gpu.depth_offset);
        if (depth_bpp && !(s_gpu.control0 & 0x1000u) &&
            s_gpu.depth_pitch >= (s_gpu.clip_x + s_gpu.clip_w) * depth_bpp &&
            !surface_write_refused(base, (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.depth_pitch, "depth clear")) {
            for (y = s_gpu.clip_y; y < s_gpu.clip_y + s_gpu.clip_h; y++) {
                uint8_t *row = mem + base + (size_t)y * s_gpu.depth_pitch;
                for (x = s_gpu.clip_x; x < s_gpu.clip_x + s_gpu.clip_w; x++) {
                    if (depth_bpp == 2) {
                        if (param & 1u) ((uint16_t *)row)[x] = (uint16_t)(s_gpu.depth_clear >> 8);
                    } else {
                        uint32_t previous = ((uint32_t *)row)[x];
                        uint32_t mask = ((param & 1u) ? 0xFFFFFF00u : 0u) | ((param & 2u) ? 255u : 0u);
                        ((uint32_t *)row)[x] = (previous & ~mask) | (s_gpu.depth_clear & mask);
                    }
                }
            }
        }
    }
    if (!(param & NV097_CLEAR_COLOR_MASK))
        return;                            /* depth/stencil only */
    if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0 || s_gpu.clip_x >= s_gpu.pitch / bpp)
        return;
    width = s_gpu.clip_w;
    if (width > s_gpu.pitch / bpp - s_gpu.clip_x) width = s_gpu.pitch / bpp - s_gpu.clip_x;
    {
        uint32_t base = dma_resolve(s_gpu.color_offset);
        if (surface_write_refused(base,
                                  (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch,
                                  "clear"))
            return;
        s_gpu.color_base = base;
    }

    for (y = 0; !native_clear && y < s_gpu.clip_h; y++) {
        uint8_t *row = mem + s_gpu.color_base
                     + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        if (bpp == 4) {
            uint32_t *p = (uint32_t *)row + s_gpu.clip_x;
            for (x = 0; x < width; x++)
                p[x] = s_gpu.clear_color;
        } else if (bpp == 2) {
            /* The clear value is always given as A8R8G8B8; a 16-bit surface
             * takes the same colour reduced to 5:6:5. */
            uint16_t v = (uint16_t)(((s_gpu.clear_color >> 8) & 0xF800)
                                  | ((s_gpu.clear_color >> 5) & 0x07E0)
                                  | ((s_gpu.clear_color >> 3) & 0x001F));
            uint16_t *p = (uint16_t *)row + s_gpu.clip_x;
            for (x = 0; x < width; x++)
                p[x] = v;
        }
    }
    s_gpu.clears++;
    /* Progress markers, interleaved with everything else in the log. The
     * summary says drawing stopped; only a marker next to the surrounding
     * activity says what the title was doing when it stopped. */
    if ((s_gpu.clears % 100) == 0)
        fprintf(stderr, "  [GPU] clear #%u\n", s_gpu.clears);
    /* Distinct clear colours actually used. "Cleared to black" and "the clear
     * never ran" look identical in the framebuffer, and only one of them is a
     * bug -- so record what was asked for, not just how often. */
    {
        static uint32_t seen[8];
        static int n;
        int i;
        for (i = 0; i < n; i++)
            if (seen[i] == s_gpu.clear_color) break;
        if (i == n && n < 8) {
            seen[n++] = s_gpu.clear_color;
            fprintf(stderr, "  [GPU] clear colour 0x%08X -> surface 0x%08X"
                            " (%ubpp)\n",
                    s_gpu.clear_color, s_gpu.color_offset, surface_bpp());
        }
    }

    /* Prove the pixel path end to end, independent of whether the title has
     * given us any geometry yet.
     *
     * "Nothing on screen" has three very different causes -- the surface
     * address or pitch is wrong, the rasteriser is broken, or the title's
     * vertex buffers are empty -- and they are indistinguishable from a black
     * window. RECOMP_RASTER_TEST draws one known triangle into the surface
     * just cleared, so a visible triangle rules out the first two and leaves
     * only the third. On Wreckless it is the third: attribute 0 decodes
     * correctly (float, size 2, stride 16) and the buffer it points at stays
     * zero.
     *
     * ponytail: bring-up aid, not a feature. It costs one branch per clear. */
#ifndef _WIN32
    if (getenv("RECOMP_RASTER_TEST")) {
        static int announced;
        /* Every clear, not once: the title clears each frame and double-buffers,
         * so a triangle drawn a single time is erased before anyone sees it. */
        if (s_gpu.clip_w && s_gpu.clip_h) {
            float a[2], b[2], c[2];
            a[0] = s_gpu.clip_w * 0.5f; a[1] = s_gpu.clip_h * 0.15f;
            b[0] = s_gpu.clip_w * 0.85f; b[1] = s_gpu.clip_h * 0.85f;
            c[0] = s_gpu.clip_w * 0.15f; c[1] = s_gpu.clip_h * 0.85f;
            raster_triangle(a, b, c, 0xFFFF00FFu, NULL, NULL);  /* magenta: never a clear colour */
            if (announced++ == 0)
            fprintf(stderr, "  [GPU] raster self-test: triangle (%.0f,%.0f)"
                            " (%.0f,%.0f) (%.0f,%.0f) into 0x%08X %ubpp\n",
                    a[0], a[1], b[0], b[1], c[0], c[1],
                    s_gpu.color_offset, surface_bpp());
        }
    }
#endif

    /* Show the surface actually being drawn into. A title that double-buffers
     * renders into the back buffer, so following AvSetDisplayMode's address
     * would show the one nothing is writing. */
    /* The window has to read where the pixels actually are, which is the
     * resolved address rather than the DMA-object offset. */
    /* Only until the title flips. Following the draw surface on every scan
     * shows the buffer being written right now, half a frame at a time; past
     * the first flip the window is repointed at the finished one instead. */
    if (s_gpu.flips == 0)
        xbox_FramebufferWindowSet(dma_resolve(s_gpu.color_offset), s_gpu.pitch);

    /* And open the window, rather than waiting for AvSetDisplayMode to do it.
     *
     * That was the only caller, so a title which draws before setting a display
     * mode -- or never sets one at all -- got no window however much it
     * rendered. The Xbox Dashboard clears a 1280x960 surface at 0x00088000 on
     * its first frame and had not called AvSetDisplayMode by then, so
     * RECOMP_FB_WINDOW=1 was set, the executor knew the address and the pitch,
     * and nothing appeared.
     *
     * Here is the better trigger anyway: this runs when a surface address is
     * known to be real, because a clear just used it. Idempotent and gated on
     * RECOMP_FB_WINDOW, so the cost is one interlocked compare per clear. */
    xbox_FramebufferWindowStart();
}


/* ── Rasteriser ──────────────────────────────────────────────────────────
 *
 * Fills triangles straight into the guest framebuffer, the same memory
 * clear_surface() writes and the framebuffer window already shows. That is the
 * whole reason it is done on the CPU rather than through D3D: nothing new has
 * to be plumbed for the result to be visible.
 *
 * ponytail: flat-shaded, no depth buffer, no texturing, no perspective
 * correction, and only batches whose attribute 0 is already in screen space.
 * A title running a vertex program hands over object-space positions that mean
 * nothing without executing the program, so those batches are counted and
 * skipped rather than drawn somewhere wrong. Upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c once vertex programs are
 * translated; this exists to get the first geometry on screen for every title,
 * which in practice is UI, HUD and 2D overlays -- all pre-transformed.
 */

/* One texel, in the title's own format.
 *
 * The codes are the NV097 colour field, which is the Xbox D3DFMT_ enum --
 * src/d3d/d3d8_xbox.h is the table, and it is the table to check against
 * rather than recollection: 0x1E is LIN_X8R8G8B8 and not, as this first read
 * it, a byte-reversed BGRA. Getting that one wrong turned an opaque black
 * render target into a screen of pure blue, which is the kind of wrong that
 * looks like content.
 *
 * Only the linear (LIN_) formats are read. A swizzled texture stores its
 * texels in Morton order rather than in rows, so reading one as if it had a
 * pitch does not give a slightly wrong colour, it gives a different image --
 * and inventing that image is exactly what this is not for. An unsupported
 * format samples nothing and the caller keeps the vertex colour, which is
 * visibly wrong rather than quietly wrong.
 *
 * ponytail: nearest texel, no filtering, whatever SET_TEXTURE_FILTER asked
 * for. Bilinear when a title's output actually depends on it.
 */
/* Off the edge of the texture, the way the title asked for.
 *
 * Refusing to sample instead is not neutral: it hands the caller back the
 * vertex colour, so a pass whose coordinates reach the last texel by half a
 * texel gets a bright line down the edge of the screen. The dashboard's
 * resolve does exactly that -- its last column and last row, 1119 pixels of
 * white on a black frame, from a rounding step at the boundary.
 */
static uint32_t wrap_coord(int32_t c, uint32_t size, uint32_t mode)
{
    if (!size)
        return 0;
    if (mode == 1 || mode == 2) {
        int32_t period = (int32_t)(mode == 2 ? size * 2u : size);
        int32_t wrapped = c % period;
        if (wrapped < 0) wrapped += period;
        if (mode == 2 && wrapped >= (int32_t)size)
            wrapped = period - 1 - wrapped;
        return (uint32_t)wrapped;
    }
    return c < 0 ? 0u : (uint32_t)c >= size ? size - 1 : (uint32_t)c;
}

static uint32_t expand(uint32_t v, uint32_t bits)
{
    return d3d8_expand_channel(v, bits);
}

/* The linear format that decodes the same texels as a swizzled one.
 *
 * Swizzling changes where a texel lives, not what it says: A8R8G8B8 (0x06) and
 * LIN_A8R8G8B8 (0x12) are the same four bytes in the same order. So the whole
 * difference is the address calculation, and one of those lets every format
 * below serve both. Pairs read off the table in d3d8_xbox.h rather than
 * recalled -- the comment above this one is about getting exactly that wrong. */
static uint32_t linear_twin(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;                /* L8        -> LIN_L8        */
    case 0x01: return 0x1B;                /* AL8       -> LIN_AL8       */
    case 0x02: return 0x10;                /* A1R5G5B5  -> LIN_A1R5G5B5  */
    case 0x03: return 0x1C;                /* X1R5G5B5  -> LIN_X1R5G5B5  */
    case 0x04: return 0x1D;                /* A4R4G4B4  -> LIN_A4R4G4B4  */
    case 0x05: return 0x11;                /* R5G6B5    -> LIN_R5G6B5    */
    case 0x06: return 0x12;                /* A8R8G8B8  -> LIN_A8R8G8B8  */
    case 0x07: return 0x1E;                /* X8R8G8B8  -> LIN_X8R8G8B8  */
    case 0x19: return 0x1F;                /* A8        -> LIN_A8        */
    case 0x1A: return 0x20;                /* A8L8      -> LIN_A8L8      */
    case 0x28: return 0x17;                /* G8B8      -> LIN_G8B8      */
    case 0x29: return 0x16;                /* R8B8      -> LIN_R8B8      */
    case 0x3A: return 0x3F;
    case 0x3B: return 0x40;
    case 0x3C: return 0x41;
    default:   return fmt;                 /* already linear, or unhandled */
    }
}

static uint32_t texture_bytes_per_pixel(uint32_t fmt)
{
    switch (linear_twin(fmt)) {
    case 0x12: case 0x1E: case 0x3F: case 0x40: case 0x41: return 4;
    case 0x10: case 0x11: case 0x16: case 0x17: case 0x1C: case 0x1D: case 0x20:
    case 0x24: case 0x25: return 2;
    case 0x13: case 0x1B: case 0x1F: return 1;
    default: return 0;
    }
}

static uint32_t physical_alias(uint32_t address)
{
    /* The Xbox exposes the same video-memory image through the 0x80000000
     * mirror and the low physical aperture. */
    return address & 0x0FFFFFFFu;
}

static int sample_texture_bound(const Texture *texture, uint32_t u, uint32_t v, uint32_t *argb)
{
    static d3d8_dxt_texel_cache dxt_cache;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t fmt;

    if (!texture->valid)
        return 0;
    u = wrap_coord((int32_t)u, texture->width, texture->addr_u);
    v = wrap_coord((int32_t)v, texture->height, texture->addr_v);

    fmt = texture->color;
    if (d3d8_format_dxt_block_bytes(fmt))
        return d3d8_dxt_decode_texel_cached(&dxt_cache,
                                           mem + texture->offset, fmt, u, v,
                                           texture->width, argb);
    if (d3d8_format_is_swizzled(fmt)) {
        /* Morton order: a texel's index is interleaved from x and y instead of
         * v*pitch + u, so index from the base of the image. The switch below
         * casts to each format's own width, which makes that index a texel
         * index for every one of them. */
        fmt = linear_twin(fmt);
        p = mem + texture->offset;
        u = swizzle_offset(u, v, texture->width, texture->height);
    } else {
        p = mem + texture->offset + (size_t)v * texture->pitch;
    }

    switch (fmt) {

    /* 32-bit, alpha-red-green-blue in the dword. */
    case 0x12:                                      /* LIN_A8R8G8B8 */
        *argb = ((const uint32_t *)p)[u];
        return 1;
    case 0x1E:                                      /* LIN_X8R8G8B8 */
        *argb = ((const uint32_t *)p)[u] | 0xFF000000u;
        return 1;

    /* 32-bit, other channel orders. The name gives the byte order from the
     * top of the dword down, so each is a permutation of the same four. */
    case 0x3F: {                                    /* LIN_A8B8G8R8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
        return 1;
    }
    case 0x40: {                                    /* LIN_B8G8R8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24)                 /* A, from the bottom */
              | (((t >>  8) & 0xFFu) << 16)         /* R */
              | (((t >> 16) & 0xFFu) <<  8)         /* G */
              |  ((t >> 24) & 0xFFu);               /* B, from the top */
        return 1;
    }
    case 0x41: {                                    /* LIN_R8G8B8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (t >> 8);
        return 1;
    }

    /* 16-bit. */
    case 0x10: {                                    /* LIN_A1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = ((t & 0x8000u) ? 0xFF000000u : 0u)
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1C: {                                    /* LIN_X1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x11: {                                    /* LIN_R5G6B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 11) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x3F, 6) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1D: {                                    /* LIN_A4R4G4B4 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = (expand((t >> 12) & 0x0F, 4) << 24)
              | (expand((t >>  8) & 0x0F, 4) << 16)
              | (expand((t >>  4) & 0x0F, 4) <<  8)
              |  expand( t        & 0x0F, 4);
        return 1;
    }

    case 0x16: {                                    /* LIN_R8B8: A=R, G=B */
        uint32_t t = ((const uint16_t *)p)[u];
        uint32_t red = t >> 8, blue = t & 255u;
        *argb = (red << 24) | (red << 16) | (blue << 8) | blue;
        return 1;
    }
    case 0x17: {                                    /* LIN_G8B8: A=G, R=B */
        uint32_t t = ((const uint16_t *)p)[u];
        uint32_t green = t >> 8, blue = t & 255u;
        *argb = (green << 24) | (blue << 16) | (green << 8) | blue;
        return 1;
    }
    case 0x20: {                                    /* LIN_A8L8 */
        uint32_t t = ((const uint16_t *)p)[u];
        uint32_t luminance = t & 255u;
        *argb = ((t >> 8) << 24) | (luminance << 16) | (luminance << 8) | luminance;
        return 1;
    }

    /* 8-bit. */
    case 0x13: {                                    /* LIN_L8 */
        uint32_t t = p[u];
        *argb = 0xFF000000u | (t << 16) | (t << 8) | t;
        return 1;
    }
    case 0x1F:                                      /* LIN_A8 */
        *argb = ((uint32_t)p[u] << 24) | 0x00FFFFFFu;
        return 1;
    case 0x1B: {                                    /* LIN_AL8: shared alpha/luminance */
        uint32_t t = p[u];
        *argb = t * 0x01010101u;
        return 1;
    }

    /* 4:2:2 packed YUV, two texels per four bytes.
     *
     * This is how a title hands over a decoded video frame, and without it
     * the frame falls through to `default` -- which returns 0, so the caller
     * paints the quad's vertex colour and the movie is a flat rectangle.
     *
     * The chroma pair is shared between an even texel and the one after it,
     * so the group is found by masking the bottom bit of the index. BT.601,
     * the same coefficients the D3D8 upload path converts with, so the two
     * paths agree rather than each having its own idea of the colour. */
    case 0x24:                                      /* LC_CR8YB8CB8YA8, YUY2 */
    case 0x25: {                                    /* LC_YB8CR8YA8CB8, UYVY */
        uint32_t yoff = (fmt == 0x24) ? 0u : 1u;
        const uint8_t *g = p + (size_t)(u & ~1u) * 2;
        int c  = (int)g[(u & 1u) ? 2 + yoff : yoff] - 16;
        int cu = (int)g[1 - yoff] - 128;
        int cv = (int)g[3 - yoff] - 128;
        int r = (298 * c + 409 * cv + 128) >> 8;
        int gg = (298 * c - 100 * cu - 208 * cv + 128) >> 8;
        int b = (298 * c + 516 * cu + 128) >> 8;
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        if (gg < 0) gg = 0;
        if (gg > 255) gg = 255;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        *argb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8)
              | (uint32_t)b;
        return 1;
    }

    default:
        return 0;
    }
}

static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb)
{
    return sample_texture_bound(&s_gpu.tex, u, v, argb);
}

/* Write a bound texture out as a BMP, through the sampler rather than around it.
 *
 * "Which texture is this" is not answerable from an address and a format, and
 * it is the question behind most of the ones that matter -- is that a font
 * page or an icon atlas, did the swizzle decode, is the alpha inverted. Going
 * through sample_texture means the file shows exactly what the rasteriser
 * sees, so a decode bug appears here rather than only as a wrong-looking
 * triangle.
 *
 * ponytail: RGB only, alpha dropped. A glyph page is alpha and would come out
 * black, so alpha is composited onto mid-grey to stay legible; that is a
 * viewing choice, not a decode. One file per distinct texture, first use only.
 */
static void dump_texture_bmp(uint32_t seq)
{
    const char *prefix = getenv("RECOMP_TEX_DUMP");
    uint32_t w = s_gpu.tex.width, h = s_gpu.tex.height, x, y;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    char path[512];
    FILE *f;

    if (!prefix || !w || !h || w > 4096 || h > 4096)
        return;
    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%02u_%08X_fmt%02X.bmp",
             prefix, seq, s_gpu.tex.offset, s_gpu.tex.color);
    f = fopen(path, "wb");
    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t argb = 0, a;
            uint8_t px[3];
            if (!sample_texture(x, h - 1 - y, &argb))
                argb = 0;
            a = (argb >> 24) & 0xFFu;
            /* over mid-grey, so an alpha-only page is visible either way */
            px[0] = (uint8_t)(((argb & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[1] = (uint8_t)((((argb >> 8) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[2] = (uint8_t)((((argb >> 16) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            fwrite(px, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    fprintf(stderr, "  [TEXDUMP] %s (%ux%u fmt 0x%02X)\n",
            path, w, h, s_gpu.tex.color);
    fflush(stderr);
}

/* The surface, resolved once per batch.
 *
 * dma_resolve consults the contiguous arena's high-water mark and
 * surface_hits_image walks the image range; both were being done per pixel
 * -- dma_resolve twice -- which cost more than the rasterisation they
 * guarded. Neither answer can change inside a batch, because the colour
 * offset arrives as a method and a method cannot arrive mid-triangle.
 *
 * This is not a micro-optimisation for its own sake: the loader's video
 * paces on frames actually presented, so the rasteriser's throughput is the
 * playback rate. */
static uint8_t *s_surface;          /* host address of surface row 0 */
static uint8_t *s_depth_surface;

static int surface_begin_batch(const uint8_t *mem)
{
    uint32_t base = dma_resolve(s_gpu.color_offset);

    if (surface_hits_image(base, (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch))
        return 0;
    s_surface = (uint8_t *)mem + base;
    return 1;
}

static void put_pixel(uint8_t *mem, uint32_t bpp, int x, int y, uint32_t argb)
{
    uint8_t *row;

    (void)mem;
    if (x < (int)s_gpu.clip_x || x >= (int)(s_gpu.clip_x + s_gpu.clip_w))
        return;
    if (y < (int)s_gpu.clip_y || y >= (int)(s_gpu.clip_y + s_gpu.clip_h))
        return;
    s_gpu.pixels++;
    if ((argb & 0x00FFFFFFu) > (s_gpu.pixel_max & 0x00FFFFFFu))
        s_gpu.pixel_max = argb;
    row = s_surface + (size_t)y * s_gpu.pitch;

    if (s_gpu.blend_enable || s_gpu.color_mask != 0x01010101u) {
        uint32_t dst = 0;
        if (bpp == 4) {
            dst = ((const uint32_t *)row)[x];
        } else if (bpp == 2) {
            uint32_t t = ((const uint16_t *)row)[x];
            dst = 0xFF000000u | (d3d8_expand_channel(t >> 11, 5) << 16) |
                  (d3d8_expand_channel((t >> 5) & 63u, 6) << 8) | d3d8_expand_channel(t & 31u, 5);
        }
        if (s_gpu.blend_enable &&
            !(s_gpu.blend_equation == 0x8006 && s_gpu.blend_sfactor == NV_BLEND_SRC_ALPHA &&
              s_gpu.blend_dfactor == NV_BLEND_ONE_MINUS_SRC_ALPHA && (argb >> 24) == 255)) {
            uint32_t blended;
            if (!nv_cpu_blend(argb, dst, s_gpu.blend_sfactor, s_gpu.blend_dfactor,
                              s_gpu.blend_equation, s_gpu.blend_constant, &blended)) {
                s_gpu.shader_failures++;
                return;
            }
            argb = blended;
        }
        if (s_gpu.color_mask != 0x01010101u) {
            uint32_t shift, mask = 0;
            for (shift = 0; shift < 32; shift += 8)
                if (s_gpu.color_mask & (1u << shift)) mask |= 255u << shift;
            argb = (argb & mask) | (dst & ~mask);
        }
    }

    if (bpp == 4) {
        ((uint32_t *)row)[x] = argb;
    } else if (bpp == 2) {
        ((uint16_t *)row)[x] = (uint16_t)(((argb >> 8) & 0xF800)
                                        | ((argb >> 5) & 0x07E0)
                                        | ((argb >> 3) & 0x001F));
    }
}

static void present_pvideo_overlay(uint32_t destination)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const volatile uint32_t *regs = (const volatile uint32_t *)(mem + 0xFD008000u);
    uint32_t source, pitch, width, height, out_x, out_y, out_width, out_height;
    uint32_t buffer = regs[0x700 / 4];
    uint32_t slot = (buffer & 0x10u) ? 1u : 0u;
    uint32_t format = regs[0x958 / 4 + slot];
    uint32_t size_in = regs[0x928 / 4 + slot];
    uint32_t point_in = regs[0x930 / 4 + slot];
    uint32_t size_out = regs[0x950 / 4 + slot];
    uint32_t point_out = regs[0x948 / 4 + slot];
    uint32_t source_x = (point_in & 0x7FFFu) >> 4;
    uint32_t source_y = (point_in >> 16) >> 4;
    uint32_t bpp = surface_bpp();
    uint32_t y;

    if (!(buffer & 0x11u) || (regs[0x704 / 4] & 1u)
            || ((format >> 16) & 3u) != 1u || (bpp != 2 && bpp != 4))
        return;
    pitch = format & 0x1FFFu;
    width = size_in & 0x7FFu;
    height = (size_in >> 16) & 0x7FFu;
    out_x = point_out & 0xFFFu;
    out_y = (point_out >> 16) & 0xFFFu;
    out_width = size_out & 0xFFFu;
    out_height = (size_out >> 16) & 0xFFFu;
    source = regs[0x900 / 4 + slot] + regs[0x920 / 4 + slot];
    if (!pitch || source_x >= width || source_y >= height
            || !out_width || !out_height || pitch < width * 2u
            || (uint64_t)physical_alias(source) + (uint64_t)pitch * height > XBOX_CONTIG_SIZE
            || out_x >= s_gpu.clip_w || out_y >= s_gpu.clip_h)
        return;
    source = dma_resolve(source) + source_y * pitch + source_x * 2u;
    width -= source_x;
    height -= source_y;

    /* PVIDEO format 1 is packed YUYV: two luma samples share U and V. */
    for (y = 0; y < out_height && out_y + y < s_gpu.clip_h; y++) {
        uint32_t x;
        uint32_t sy = height == out_height ? y : (uint32_t)((uint64_t)y * height / out_height);
        const uint8_t *row = mem + source + (size_t)sy * pitch;
        uint8_t *dst = (uint8_t *)mem + dma_resolve(destination)
                     + (size_t)(out_y + y) * s_gpu.pitch;
        for (x = 0; x < out_width && out_x + x < s_gpu.clip_w; x++) {
            uint32_t sx = width == out_width ? x : (uint32_t)((uint64_t)x * width / out_width);
            const uint8_t *pair = row + (sx & ~1u) * 2u;
            int yv = (int)pair[(sx & 1u) ? 2u : 0u] - 16;
            int u = (int)pair[1] - 128;
            int v = (int)pair[3] - 128;
            int r = (298 * yv + 409 * v + 128) >> 8;
            int g = (298 * yv - 100 * u - 208 * v + 128) >> 8;
            int b = (298 * yv + 516 * u + 128) >> 8;
            uint32_t argb;
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            argb = 0xFF000000u | ((uint32_t)r << 16)
                 | ((uint32_t)g << 8) | (uint32_t)b;
            if (bpp == 4)
                ((uint32_t *)dst)[out_x + x] = argb;
            else
                ((uint16_t *)dst)[out_x + x] = (uint16_t)(((argb >> 8) & 0xF800)
                    | ((argb >> 5) & 0x07E0) | ((argb >> 3) & 0x001F));
        }
    }
    if (getenv("RECOMP_PB_SURF_TRACE"))
        fprintf(stderr, "  [GPU] PVIDEO overlay 0x%08X %ux%u -> %ux%u at %u,%u\n",
                source, width, height, out_width, out_height, out_x, out_y);
}

/* Half-space fill. Barycentric edge functions rather than scanline slopes:
 * the same test decides both windings, so a title that emits clockwise
 * triangles does not silently render nothing. */
static int shader_fragment(const Nv2aCpuVertex vertices[3], const float weights[3], int fast_mode, uint32_t *argb)
{
    float diffuse[4] = {0}, specular[4] = {0}, textures[4][4] = {{0}}, result[4];
    uint32_t component, vertex, stage;
    if (fast_mode != 1) {
        for (component = 0; component < 4; component++) {
            for (vertex = 0; vertex < 3; vertex++) {
                diffuse[component] += vertices[vertex].output[3][component] * weights[vertex];
                if (!fast_mode)
                    specular[component] += vertices[vertex].output[4][component] * weights[vertex];
            }
        }
        if (s_gpu.shade_mode == 0x1D00) {
            memcpy(diffuse, vertices[2].output[3], sizeof diffuse);
            memcpy(specular, vertices[2].output[4], sizeof specular);
        }
    }
    for (stage = 0; stage < 4; stage++) {
        uint32_t mode = (s_gpu.shader_stage_program >> (stage * 5u)) & 31u;
        const Texture *texture = stage ? &s_gpu.extra_tex[stage - 1] : &s_gpu.tex;
        float coordinate[4] = {0};
        uint32_t texel;
        textures[stage][3] = 1.0f;
        if (!mode) continue;
        for (component = 0; component < 4; component++)
            for (vertex = 0; vertex < 3; vertex++)
                coordinate[component] += vertices[vertex].output[9 + stage][component] * weights[vertex];
        if (mode == 4) {
            memcpy(textures[stage], coordinate, sizeof coordinate);
            continue;
        }
        if (mode != 1 || coordinate[3] == 0.0f) return 0;
        coordinate[0] /= coordinate[3]; coordinate[1] /= coordinate[3];
        if (tex_size_from_format(texture->color)) {
            coordinate[0] *= texture->width;
            coordinate[1] *= texture->height;
        }
        if (!nv_cpu_finite(coordinate[0]) || !nv_cpu_finite(coordinate[1]) ||
            fabsf(coordinate[0]) > 2147480000.0f || fabsf(coordinate[1]) > 2147480000.0f) return 0;
        if (!sample_texture_bound(texture, (uint32_t)nv_cpu_floor_coordinate(coordinate[0]),
                                   (uint32_t)nv_cpu_floor_coordinate(coordinate[1]), &texel)) return 0;
        if (fast_mode == 1) { *argb = texel; return 1; }
        if (fast_mode == 3) {
            *argb = (texel & 0x00FFFFFFu) |
                ((uint32_t)(nv_cpu_clamp(diffuse[3], 0.0f, 1.0f) * 255.0f + 0.5f) << 24);
            return 1;
        }
        nv_cpu_unpack_argb(texel, textures[stage]);
    }
    if (fast_mode == 2 || fast_mode == 4 || fast_mode == 5) {
        for (component = 0; component < 4; component++)
            result[component] = diffuse[component] * textures[0][component];
        if (fast_mode == 4) result[3] = diffuse[3];
        if (fast_mode == 5) result[3] = textures[0][3];
        *argb = nv_cpu_pack_argb(result);
        return 1;
    }
    if (!nv_cpu_combiners_execute(&s_gpu.combiners, diffuse, specular, textures, 1.0f, result)) return 0;
    *argb = nv_cpu_pack_argb(result);
    return 1;
}

static int alpha_pass(uint32_t alpha)
{
    if (!s_gpu.alpha_enable) return 1;
    return nv_cpu_compare(s_gpu.alpha_func, alpha, s_gpu.alpha_ref);
}

static int depth_pass(uint8_t *mem, int x, int y, const Nv2aCpuVertex vertices[3],
                      float opposite_a, float opposite_b, float opposite_c, float area, int write_depth)
{
    uint32_t format = (s_gpu.format >> 4) & 15u;
    uint32_t stored, incoming, maximum = format == 1 ? 65535u : 16777215u;
    uint8_t *pixel;
    float depth;
    if (!s_gpu.depth_enable || !vertices) return 1;
    if (!s_gpu.depth_offset || !s_gpu.depth_pitch || (format != 1 && format != 2) ||
        (s_gpu.control0 & 0x1000u)) { s_gpu.shader_failures++; return 0; }
    depth = (opposite_a * vertices[0].output[0][2] + opposite_b * vertices[1].output[0][2] +
             opposite_c * vertices[2].output[0][2]) / area;
    if (s_gpu.control0 & 0x10000u) {
        float reciprocal = opposite_a / vertices[0].output[0][3] +
                           opposite_b / vertices[1].output[0][3] + opposite_c / vertices[2].output[0][3];
        depth = area / reciprocal;
    }
    if (!nv_cpu_finite(depth)) return 0;
    incoming = (uint32_t)nv_cpu_clamp(depth, 0.0f, (float)maximum);
    (void)mem;
    pixel = s_depth_surface + (size_t)y * s_gpu.depth_pitch + (size_t)x * (format == 1 ? 2 : 4);
    stored = format == 1 ? *(uint16_t *)pixel : *(uint32_t *)pixel >> 8;
    if (!nv_cpu_compare(s_gpu.depth_func, incoming, stored)) {
        s_gpu.depth_rejected++;
        return 0;
    }
    if (s_gpu.depth_mask && write_depth) {
        if (format == 1) *(uint16_t *)pixel = (uint16_t)incoming;
        else *(uint32_t *)pixel = (incoming << 8) | (*(uint32_t *)pixel & 255u);
        s_gpu.depth_written++;
    }
    return 1;
}

static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2], const Nv2aCpuVertex varying[3])
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area;
    int minx, maxx, miny, maxy, x, y;
    int textured = uv && s_gpu.tex.valid;
    float reciprocal_w[3] = {1.0f, 1.0f, 1.0f};
    int shaded = varying && s_gpu.combiners_configured;
    int fast_mode = s_gpu.shader_stage_program == 1 ? nv_cpu_combiner_fast_mode(&s_gpu.combiners) : 0;
    int fast_shaded = 0;
    uint32_t fast_alpha = 0;
    float fast_uv[3][2];
    uint8_t fast_modulate[4][256];

    s_gpu.flush_raster++;
    nv2a_pb_exec_flush();

    if (bpp != 4 && bpp != 2)
        return;

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f || !isfinite(area))
        return;                            /* degenerate */
    if (shaded) {
        uint32_t vertex;
        const float *constant_color = varying[s_gpu.shade_mode == 0x1D00 ? 2 : 0].output[3];
        int constant_diffuse = s_gpu.shade_mode == 0x1D00 ||
            (!memcmp(varying[0].output[3], varying[1].output[3], sizeof varying[0].output[3]) &&
             !memcmp(varying[1].output[3], varying[2].output[3], sizeof varying[0].output[3]));
        for (vertex = 0; vertex < 3; vertex++) {
            float position_w = varying[vertex].output[0][3];
            if (!isfinite(position_w) || position_w <= 0.0f) return;
            reciprocal_w[vertex] = 1.0f / position_w;
        }
        if ((fast_mode == 1 || ((fast_mode == 2 || fast_mode == 4 || fast_mode == 5) && constant_diffuse && fabsf(area) >= 2048.0f) ||
               (fast_mode == 3 &&
               varying[0].output[3][3] == varying[1].output[3][3] &&
               varying[1].output[3][3] == varying[2].output[3][3])) && s_gpu.tex.valid &&
            reciprocal_w[0] == reciprocal_w[1] && reciprocal_w[1] == reciprocal_w[2] &&
            varying[0].output[9][3] > 0.0f && isfinite(varying[0].output[9][3]) &&
            varying[0].output[9][3] == varying[1].output[9][3] &&
            varying[1].output[9][3] == varying[2].output[9][3]) {
            float scale_u = tex_size_from_format(s_gpu.tex.color) ? (float)s_gpu.tex.width : 1.0f;
            float scale_v = tex_size_from_format(s_gpu.tex.color) ? (float)s_gpu.tex.height : 1.0f;
            for (vertex = 0; vertex < 3; vertex++) {
                fast_uv[vertex][0] = varying[vertex].output[9][0] * scale_u / varying[vertex].output[9][3];
                fast_uv[vertex][1] = varying[vertex].output[9][1] * scale_v / varying[vertex].output[9][3];
                if (!isfinite(fast_uv[vertex][0]) || !isfinite(fast_uv[vertex][1])) return;
            }
            uv = (const float (*)[2])fast_uv;
            textured = 1;
            shaded = 0;
            fast_shaded = fast_mode;
            fast_alpha = nv_cpu_pack_argb(constant_color) & 0xFF000000u;
        }
    }
    if (!isfinite(a[0]) || !isfinite(a[1]) || !isfinite(b[0]) || !isfinite(b[1]) ||
        !isfinite(c[0]) || !isfinite(c[1])) return;

    /* Where this batch writes. The same check the per-pixel path made, made
     * once: a surface address landing on the title's own image is no safer
     * one pixel at a time than 4.9 MB at once. */
    if (!surface_begin_batch(mem))
        return;
    if (s_gpu.depth_enable && varying) {
        uint32_t format = (s_gpu.format >> 4) & 15u;
        uint32_t bytes = format == 1 ? 2u : format == 2 ? 4u : 0u;
        uint32_t base = dma_resolve(s_gpu.depth_offset);
        if (!bytes || s_gpu.depth_pitch < (s_gpu.clip_x + s_gpu.clip_w) * bytes ||
            surface_write_refused(base,
                                  (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.depth_pitch, "depth draw")) {
            s_gpu.shader_failures++;
            return;
        }
        s_depth_surface = mem + base;
    }

    minx = (int)floorf(nv_cpu_clamp(fminf(a[0], fminf(b[0], c[0])),
                      (float)s_gpu.clip_x, (float)(s_gpu.clip_x + s_gpu.clip_w)));
    maxx = (int)ceilf(nv_cpu_clamp(fmaxf(a[0], fmaxf(b[0], c[0])),
                     (float)s_gpu.clip_x, (float)(s_gpu.clip_x + s_gpu.clip_w)));
    miny = (int)floorf(nv_cpu_clamp(fminf(a[1], fminf(b[1], c[1])),
                      (float)s_gpu.clip_y, (float)(s_gpu.clip_y + s_gpu.clip_h)));
    maxy = (int)ceilf(nv_cpu_clamp(fmaxf(a[1], fmaxf(b[1], c[1])),
                     (float)s_gpu.clip_y, (float)(s_gpu.clip_y + s_gpu.clip_h)));

    if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
    if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
    if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        return;
    }
    if (fast_shaded == 2 || fast_shaded == 4 || fast_shaded == 5) {
        const float *constant_color = varying[s_gpu.shade_mode == 0x1D00 ? 2 : 0].output[3];
        uint32_t component, texel;
        for (component = 0; component < 4; component++)
            for (texel = 0; texel < 256; texel++)
                fast_modulate[component][texel] = (uint8_t)(nv_cpu_clamp(
                    ((float)texel / 255.0f) * constant_color[component], 0.0f, 1.0f) * 255.0f + 0.5f);
    }

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            if (!nv_cpu_edge_inside(w0, b[0] - a[0], b[1] - a[1], area) ||
                !nv_cpu_edge_inside(w1, c[0] - b[0], c[1] - b[1], area) ||
                !nv_cpu_edge_inside(w2, a[0] - c[0], a[1] - c[1], area))
                continue;
            if (varying && s_gpu.depth_enable && !s_gpu.alpha_enable &&
                !depth_pass(mem, x, y, varying, w1, w2, w0, area, 0)) continue;
            if (shaded) {
                float weights[3] = {w1 * reciprocal_w[0], w2 * reciprocal_w[1], w0 * reciprocal_w[2]};
                float sum = weights[0] + weights[1] + weights[2];
                uint32_t fragment;
                if (!nv_cpu_finite(sum) || sum == 0.0f) continue;
                weights[0] /= sum; weights[1] /= sum; weights[2] /= sum;
                if (!shader_fragment(varying, weights, fast_mode, &fragment)) {
                    s_gpu.shader_failures++;
                    continue;
                }
                if (alpha_pass(fragment >> 24) && depth_pass(mem, x, y, varying, w1, w2, w0, area, 1)) {
                    put_pixel(mem, bpp, x, y, fragment);
                    s_gpu.shader_pixels++;
                }
                continue;
            }
            if (textured) {
                /* Barycentric, straight from the edge functions already
                 * computed: w1 is the area opposite a, w2 opposite b, w0
                 * opposite c, and the three sum to the whole triangle.
                 *
                 * No perspective divide. These are screen-space vertices with
                 * no w to divide by -- which is exactly the case a full-screen
                 * pass is, and the only case that reaches here. */
                uint32_t texel;
                float su = (w1 * uv[0][0] + w2 * uv[1][0] + w0 * uv[2][0]) / area;
                float sv = (w1 * uv[0][1] + w2 * uv[1][1] + w0 * uv[2][1]) / area;
                if (!nv_cpu_finite(su) || !nv_cpu_finite(sv) || fabsf(su) > 2147480000.0f || fabsf(sv) > 2147480000.0f)
                    continue;
                if (sample_texture((uint32_t)nv_cpu_floor_coordinate(su), (uint32_t)nv_cpu_floor_coordinate(sv), &texel)) {
                    if (fast_shaded == 3) texel = (texel & 0x00FFFFFFu) | fast_alpha;
                    if (fast_shaded == 2 || fast_shaded == 4 || fast_shaded == 5) {
                        uint32_t alpha = fast_shaded == 2 ? (uint32_t)fast_modulate[3][texel >> 24] << 24 :
                            fast_shaded == 4 ? fast_alpha : texel & 0xFF000000u;
                        texel = alpha | ((uint32_t)fast_modulate[0][(texel >> 16) & 255u] << 16) |
                            ((uint32_t)fast_modulate[1][(texel >> 8) & 255u] << 8) | fast_modulate[2][texel & 255u];
                    }
                    if ((!fast_shaded || alpha_pass(texel >> 24)) &&
                        depth_pass(mem, x, y, varying, w1, w2, w0, area, 1)) {
                        put_pixel(mem, bpp, x, y, texel);
                        if (fast_shaded) s_gpu.shader_pixels++;
                    }
                    continue;
                }
            }
            if (depth_pass(mem, x, y, varying, w1, w2, w0, area, 1))
                put_pixel(mem, bpp, x, y, argb);
        }
    }
    s_gpu.tris_drawn++;
    s_gpu.drawn_offset = s_gpu.color_offset;
}

/* Attribute 3 is diffuse colour in every NV2A layout that sets one. Absent it,
 * white -- a visible wrong colour beats an invisible correct one during
 * bring-up. */
/* Which attribute carries the colour.
 *
 * Slot 3 is diffuse by convention and titles that follow it are read straight
 * from there. Half-Life 2 does not: its vertex is position, colour, texcoord
 * at stride 24, and the colour arrives in slot 5. So fall back to the format
 * rather than the slot number -- D3DCOLOR is the one attribute type that is
 * only ever a colour, which makes it a stronger signal than the convention. */
static const VertexAttr *color_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[3].offset && s_gpu.attr[3].stride)
        return &s_gpu.attr[3];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 0 && s_gpu.attr[a].size == 4
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[3];
}

/* Attribute 9 is texture coordinate 0 in the NV2A vertex layout, the same way
 * 0 is position and 3 is diffuse -- for a title that follows the convention.
 *
 * Half-Life 2 does not, in either place. Its menu and HUD vertex is position,
 * colour, texcoord at stride 24, with the colour in slot 5 and the texcoords
 * in slot 7, so reading slot 9 found nothing and every batch drew untextured.
 * That is invisible rather than wrong-looking: the menu paints a full-screen
 * quad and then draws its text over it, and with no sampling both come out
 * white, so the screen is blank white and nothing suggests the text was ever
 * drawn.
 *
 * Falling back to the format works because the three attributes of such a
 * vertex are distinguishable: position is float3, colour is D3DCOLOR, and a
 * float2 is a texture coordinate and nothing else.
 *
 * ponytail: takes the first float2 it finds, so a title with two texcoord sets
 * gets stage 0's -- which is what this single-texture rasteriser samples
 * anyway. Multi-texture wants the D3D11 translator, not another heuristic. */
static const VertexAttr *texcoord_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[9].offset && s_gpu.attr[9].stride)
        return &s_gpu.attr[9];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 2 && s_gpu.attr[a].size == 2
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[9];
}

/* Texel coordinates, whichever convention the title used.
 *
 * The two are not interchangeable and the format decides which is in force: a
 * swizzled texture is addressed in [0,1], a linear one in texels. Both are
 * scaled to texels here so that everything downstream -- the barycentric
 * interpolation and the sampler -- works in one unit.
 *
 * This mattered the moment swizzled formats became samplable. Normalised
 * coordinates truncated to a texel index land on texel 0 for any coordinate
 * below 1.0, so a whole quad sampled a single texel and came out flat: the
 * background painted one near-black colour, which looks like a texture that
 * decoded wrong rather than one that was never indexed. */
static int fetch_texcoord(uint32_t index, float out[2])
{
    float t[4];

    if (!fetch_attr(texcoord_attr(), index, t))
        return 0;
    out[0] = t[0];
    out[1] = t[1];
    if (tex_size_from_format(s_gpu.tex.color)) {
        out[0] *= (float)s_gpu.tex.width;
        out[1] *= (float)s_gpu.tex.height;
    }
    return 1;
}

static uint32_t vertex_color(uint32_t index)
{
    float c[4];

    if (!fetch_attr(color_attr(), index, c))
        return 0xFFFFFFFFu;
    return ((uint32_t)(c[3] * 255.0f) << 24)
         | ((uint32_t)(c[0] * 255.0f) << 16)
         | ((uint32_t)(c[1] * 255.0f) <<  8)
         |  (uint32_t)(c[2] * 255.0f);
}

/* An untransformed batch drawn as if it were screen space smears a few pixels
 * into the corner, so the batch has to be classified before it is rasterised.
 *
 * This used to demand that every vertex land inside the surface, which is a
 * different question and the wrong one: geometry that extends past the
 * viewport is ordinary, and clipping it is raster_triangle's job (it clamps
 * its span to the clip rect). The dashboard is exactly the case that exposed
 * it -- a full-screen pass drawn as one oversized triangle, vertices at
 * (-0.5,-0.5), (2*w,-0.5), (-0.5,2*h), all correct and all rejected.
 *
 * What actually separates the two is scale. Object-space positions are model
 * units, a handful either side of the origin; screen-space ones are measured
 * in pixels of a surface hundreds of pixels wide. So: the batch has to be able
 * to touch the surface at all, and it has to be bigger than object space.
 *
 * ponytail: a genuinely tiny screen-space sprite reads as object space and is
 * skipped. It is counted as skipped rather than silently dropped, and the
 * unambiguous answer needs the vertex-program state, which is not tracked yet.
 */
#define OBJECT_SPACE_SPAN 8.0f

static int batch_is_screen_space(void)
{
    float p[4], lo_x, hi_x, lo_y, hi_y;
    uint32_t i;

    if (!s_gpu.clip_w || !s_gpu.clip_h || !s_gpu.idx_count)
        return 0;
    if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p))
        return 0;
    lo_x = hi_x = p[0];
    lo_y = hi_y = p[1];
    for (i = 1; i < s_gpu.idx_count; i++) {
        if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], p))
            return 0;
        if (p[0] < lo_x) lo_x = p[0];
        if (p[0] > hi_x) hi_x = p[0];
        if (p[1] < lo_y) lo_y = p[1];
        if (p[1] > hi_y) hi_y = p[1];
    }

    /* Entirely off the surface: nothing to draw under either reading. */
    if (hi_x < (float)s_gpu.clip_x
     || lo_x > (float)(s_gpu.clip_x + s_gpu.clip_w)
     || hi_y < (float)s_gpu.clip_y
     || lo_y > (float)(s_gpu.clip_y + s_gpu.clip_h))
        return 0;

    /* Small enough to be model units rather than pixels. */
    if (hi_x - lo_x < OBJECT_SPACE_SPAN && hi_y - lo_y < OBJECT_SPACE_SPAN)
        return 0;

    return 1;
}

/* NV097 primitive types.
 *
 * These are the operand of SET_BEGIN_END, where 0 is END and the list starts
 * at 1. They were each one too low, so every title's geometry was decomposed
 * as the primitive below the one it asked for -- a strip as a fan, a fan as
 * quads, and TRIANGLES, the one case whose vertex count must be a multiple
 * of three, as a strip.
 *
 * The vertex order says which numbering is right without taking a table on
 * trust: a strip arrives in Z order and a fan in cyclic order, and they only
 * line up with the primitive under this one. */
#define NV_PRIM_POINTS         1
#define NV_PRIM_LINES          2
#define NV_PRIM_LINE_LOOP      3
#define NV_PRIM_LINE_STRIP     4
#define NV_PRIM_TRIANGLES      5
#define NV_PRIM_TRIANGLE_STRIP 6
#define NV_PRIM_TRIANGLE_FAN   7
#define NV_PRIM_QUADS          8
#define NV_PRIM_QUAD_STRIP     9
#define NV_PRIM_POLYGON        10

/* How many post-draw captures to keep: enough to see whether the geometry
 * is stable from frame to frame, few enough not to fill a directory. */
#define FB_DUMP_AFTER_DRAW 8
static int s_drawn_dumps;

static void dump_surface_bmp(void);

/* One triangle by vertex index: gather position and, if the batch has one,
 * texture coordinate 0. A vertex whose position cannot be read is not drawn;
 * a batch whose texcoords cannot be read is drawn untextured rather than not
 * at all, so a missing coordinate stream costs the colour and not the shape.
 */
static int fetch_shader_vertex(uint32_t index, Nv2aCpuVertex *vertex)
{
    static struct {
        uint32_t draw, index;
        Nv2aCpuVertex vertex;
    } cache[NV_VERTEX_CACHE_SIZE];
    uint32_t cache_slot = index % NV_VERTEX_CACHE_SIZE, attribute;
    if (cache[cache_slot].draw == s_gpu.draws && cache[cache_slot].index == index) {
        *vertex = cache[cache_slot].vertex;
        return 1;
    }
    if ((s_gpu.transform_execution_mode & 3u) == 2u) {
        float attributes[16][4];
        for (attribute = 0; attribute < 16; attribute++)
            fetch_attr(&s_gpu.attr[attribute], index, attributes[attribute]);
        if (!nv_cpu_vertex_execute(s_gpu.vp_program, s_gpu.vp_valid, s_gpu.vp_start,
                                   attributes, s_gpu.vp_constants, vertex)) {
            static uint32_t shown;
            if (shown++ < 8 && vertex->failed_slot < 136) {
                const uint32_t *words = s_gpu.vp_program[vertex->failed_slot];
                fprintf(stderr, "[GPU] rejected vertex shader: start %u slot %u valid %X constant %d words %08X %08X %08X %08X\n",
                        s_gpu.vp_start, vertex->failed_slot, s_gpu.vp_valid[vertex->failed_slot],
                        vertex->failed_constant, words[0], words[1], words[2], words[3]);
            }
            s_gpu.shader_failures++;
            return 0;
        }
        s_gpu.shader_vertices++;
    } else {
        memset(vertex, 0, sizeof(*vertex));
        for (attribute = 0; attribute < 16; attribute++) vertex->output[attribute][3] = 1.0f;
        if (!fetch_attr(&s_gpu.attr[0], index, vertex->output[0])) return 0;
        if ((s_gpu.transform_execution_mode & 3u) == 0u && s_gpu.composite_valid == 0xFFFFu) {
            float position[4];
            uint32_t component, source;
            memcpy(position, vertex->output[0], sizeof position);
            for (component = 0; component < 4; component++) {
                vertex->output[0][component] = 0.0f;
                for (source = 0; source < 4; source++)
                    vertex->output[0][component] += nv_cpu_multiply(position[source], s_gpu.vp_constants[component][source]);
            }
            if (!isfinite(vertex->output[0][3]) || vertex->output[0][3] == 0.0f) return 0;
            for (component = 0; component < 3; component++)
                vertex->output[0][component] /= vertex->output[0][3];
            vertex->output[0][0] += s_gpu.vp_constants[59][0];
            vertex->output[0][1] += s_gpu.vp_constants[59][1];
        }
        if (!fetch_attr(color_attr(), index, vertex->output[3]))
            nv_cpu_unpack_argb(0xFFFFFFFFu, vertex->output[3]);
        fetch_attr(&s_gpu.attr[4], index, vertex->output[4]);
        for (attribute = 0; attribute < 4; attribute++)
            fetch_attr(attribute ? &s_gpu.attr[9 + attribute] : texcoord_attr(), index, vertex->output[9 + attribute]);
    }
    for (attribute = 0; attribute < 4; attribute++) {
        vertex->output[3][attribute] = isnan(vertex->output[3][attribute]) ? 1.0f :
            nv_cpu_clamp(vertex->output[3][attribute], 0.0f, 1.0f);
        vertex->output[4][attribute] = isnan(vertex->output[4][attribute]) ? 1.0f :
            nv_cpu_clamp(vertex->output[4][attribute], 0.0f, 1.0f);
    }
    cache[cache_slot].draw = s_gpu.draws;
    cache[cache_slot].index = index;
    cache[cache_slot].vertex = *vertex;
    return 1;
}

static void raster_indexed(uint32_t i0, uint32_t i1, uint32_t i2, uint32_t argb)
{
    float p[3][4], uv[3][2];
    Nv2aCpuVertex vertices[3];
    int textured;

    if (!fetch_shader_vertex(i0, &vertices[0])
     || !fetch_shader_vertex(i1, &vertices[1])
     || !fetch_shader_vertex(i2, &vertices[2]))
        return;
    memcpy(p[0], vertices[0].output[0], sizeof p[0]);
    memcpy(p[1], vertices[1].output[0], sizeof p[1]);
    memcpy(p[2], vertices[2].output[0], sizeof p[2]);
    {
        static uint32_t last_draw, shown;
        float input[4];
        fetch_attr(&s_gpu.attr[0], i0, input);
        if (last_draw != s_gpu.draws && shown < 16 && fabsf(input[0]) < 8 && fabsf(input[1]) < 8 &&
            s_gpu.idx_count > 12) {
            last_draw = s_gpu.draws;
            shown++;
            fprintf(stderr, "[GPU] scene vertex: mode %X count %u normal type %u input %.4f %.4f %.4f %.4f output %.4f %.4f %.4f %.4f diffuse %.4f %.4f %.4f %.4f\n",
                    s_gpu.transform_execution_mode, s_gpu.idx_count, s_gpu.attr[2].type,
                    input[0], input[1], input[2], input[3], p[0][0], p[0][1], p[0][2], p[0][3],
                    vertices[0].output[3][0], vertices[0].output[3][1], vertices[0].output[3][2], vertices[0].output[3][3]);
            if (shown == 1) {
                uint32_t slot;
                for (slot = s_gpu.vp_start; slot < 136; slot++) {
                    const uint32_t *words = s_gpu.vp_program[slot];
                    uint32_t constant = (words[1] >> 13) & 255u;
                    fprintf(stderr, "[GPU] scene instruction %u: %08X %08X %08X %08X\n", slot,
                            words[0], words[1], words[2], words[3]);
                    if (constant < 192) {
                        const float *value = s_gpu.vp_constants[constant];
                        fprintf(stderr, "[GPU] scene constant %u: %.6f %.6f %.6f %.6f\n", constant,
                                value[0], value[1], value[2], value[3]);
                    }
                    if (words[3] & 1u) break;
                }
                for (slot = 0; slot < 16; slot++) {
                    fetch_attr(&s_gpu.attr[slot], i0, input);
                    fprintf(stderr, "[GPU] scene input %u type %u size %u stride %u: %.6f %.6f %.6f %.6f\n",
                            slot, s_gpu.attr[slot].type, s_gpu.attr[slot].size, s_gpu.attr[slot].stride,
                            input[0], input[1], input[2], input[3]);
                }
            }
        }
    }

    textured = fetch_texcoord(i0, uv[0])
            && fetch_texcoord(i1, uv[1])
            && fetch_texcoord(i2, uv[2]);

    raster_triangle(p[0], p[1], p[2], argb,
                    textured ? (const float (*)[2])uv : NULL, vertices);
}

#ifdef _WIN32
static int gpu_decode_texture(void *context, uint32_t horizontal, uint32_t vertical, uint32_t *color)
{
    return sample_texture_bound((const Texture *)context, horizontal, vertical, color);
}

typedef struct {
    Texture texture;
    uint32_t face_stride;
    uint32_t bytes_per_pixel, mip_levels;
    uint32_t depth;
} GpuCubeTexture;

static int gpu_decode_cube_texture(void *context, uint32_t face, uint32_t horizontal, uint32_t vertical, uint32_t *color)
{
    const GpuCubeTexture *binding = (const GpuCubeTexture *)context;
    Texture texture = binding->texture;
    if (face >= 6) return 0;
    texture.offset += face * binding->face_stride;
    return sample_texture_bound(&texture, horizontal, vertical, color);
}

static uint32_t *gpu_indices, gpu_index_count, gpu_index_capacity;
static Nv2aGpuVertex *gpu_vertices;
static uint32_t gpu_vertex_capacity;
static int gpu_decode_mip_texture(void *context, uint32_t face, uint32_t level,
                                  uint32_t horizontal, uint32_t vertical, uint32_t *color)
{
    const GpuCubeTexture *binding = (const GpuCubeTexture *)context;
    Texture texture = binding->texture;
    uint64_t offset = texture.offset + (uint64_t)face * binding->face_stride;
    uint32_t block_bytes = d3d8_format_dxt_block_bytes(texture.color);
    if (level >= binding->mip_levels || face >= (binding->face_stride ? 6u : 1u)) return 0;
    for (uint32_t previous = 0; previous < level; previous++) {
        if (block_bytes) offset += (uint64_t)((texture.width + 3) / 4) * ((texture.height + 3) / 4) * block_bytes;
        else offset += (uint64_t)texture.width * texture.height * binding->bytes_per_pixel;
        if (texture.width > 1) texture.width >>= 1;
        if (texture.height > 1) texture.height >>= 1;
    }
    if (offset > UINT32_MAX) return 0;
    texture.offset = (uint32_t)offset;
    return sample_texture_bound(&texture, horizontal, vertical, color);
}

static int gpu_decode_volume_texture(void *context, uint32_t level, uint32_t horizontal,
                                     uint32_t vertical, uint32_t slice, uint32_t *color)
{
    const GpuCubeTexture *binding = (const GpuCubeTexture *)context;
    Texture texture = binding->texture;
    uint32_t depth = binding->depth;
    uint32_t block_bytes = d3d8_format_dxt_block_bytes(texture.color);
    uint64_t offset = texture.offset, slice_bytes;
    if (level >= binding->mip_levels) return 0;
    for (uint32_t previous = 0; ; previous++) {
        slice_bytes = block_bytes ?
            (uint64_t)((texture.width + 3) / 4) * ((texture.height + 3) / 4) * block_bytes :
            tex_size_from_format(texture.color) ? (uint64_t)texture.width * texture.height * binding->bytes_per_pixel :
            (uint64_t)texture.pitch * texture.height;
        if (previous == level) break;
        offset += slice_bytes * depth;
        if (texture.width > 1) texture.width >>= 1;
        if (texture.height > 1) texture.height >>= 1;
        if (depth > 1) depth >>= 1;
    }
    if (horizontal >= texture.width || vertical >= texture.height || slice >= depth) return 0;
    if (!block_bytes && d3d8_format_is_swizzled(texture.color)) {
        offset += (uint64_t)swizzle_volume_offset(horizontal, vertical, slice,
            texture.width, texture.height, depth) * binding->bytes_per_pixel;
        texture.color = linear_twin(texture.color);
        texture.width = texture.height = 1;
        texture.pitch = binding->bytes_per_pixel;
        horizontal = vertical = 0;
    } else {
        offset += slice_bytes * slice;
    }
    if (offset > UINT32_MAX) return 0;
    texture.offset = (uint32_t)offset;
    return sample_texture_bound(&texture, horizontal, vertical, color);
}

static uint32_t gpu_vertex_tags[65536], gpu_vertex_offsets[65536], gpu_vertex_keys[65536], gpu_vertex_serial;
static uint32_t gpu_attribute_mask;

static void gpu_begin_chunk(void)
{
    gpu_index_count = 0;
    if (++gpu_vertex_serial == 0) {
        memset(gpu_vertex_tags, 0, sizeof gpu_vertex_tags);
        gpu_vertex_serial = 1;
    }
}

static int gpu_submit_chunk(Nv2aGpuDraw *state, uint32_t *count);

static double gpu_clock_seconds(void)
{
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if ((!frequency.QuadPart && (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)) ||
        !QueryPerformanceCounter(&counter)) {
        fprintf(stderr, "[GPU-D3D11] native performance clock failed (win32 %lu)\n", GetLastError());
        fflush(stderr);
        _Exit(EXIT_FAILURE);
    }
    return (double)counter.QuadPart / (double)frequency.QuadPart;
}

static int gpu_append_primitive(Nv2aGpuDraw *state, uint32_t *count, const uint32_t *indices, uint32_t vertex_count)
{
    Nv2aCpuVertex vertices[3];
    uint32_t vertex, stage;
    /* Submit only complete primitives at chunk boundaries. */
    if (*count > gpu_vertex_capacity - vertex_count || gpu_index_count > gpu_index_capacity - vertex_count) {
        if (!gpu_submit_chunk(state, count)) return 0;
        gpu_begin_chunk();
    }
    if ((s_gpu.transform_execution_mode & 3u) == 2u ||
        ((s_gpu.transform_execution_mode & 3u) == 0u && s_gpu.composite_valid == 0xFFFFu)) {
        for (vertex = 0; vertex < vertex_count; vertex++) {
            uint32_t index = indices[vertex];
            uint32_t slot = index & 65535u;
            Nv2aGpuVertex *destination;
            if (gpu_vertex_tags[slot] == gpu_vertex_serial && gpu_vertex_keys[slot] == index) {
                gpu_indices[gpu_index_count++] = gpu_vertex_offsets[slot];
                continue;
            }
            gpu_vertex_tags[slot] = gpu_vertex_serial; gpu_vertex_keys[slot] = index;
            gpu_vertex_offsets[slot] = *count;
            gpu_indices[gpu_index_count++] = *count;
            destination = &gpu_vertices[(*count)++];
            if ((s_gpu.transform_execution_mode & 3u) == 2u) {
                memset(destination, 0, sizeof *destination);
                for (stage = 0; stage < 16; stage++) {
                    if (gpu_attribute_mask & (1u << stage)) {
                        fetch_attr(&s_gpu.attr[stage], indices[vertex], destination->attributes[stage]);
                        s_gpu.gpu_attribute_fetches++;
                    } else s_gpu.gpu_attribute_skips++;
                }
                memcpy(destination->position, destination->attributes[0], sizeof destination->position);
            } else {
                memset(destination, 0, offsetof(Nv2aGpuVertex, attributes));
                s_gpu.gpu_zero_bytes_saved += sizeof destination->attributes;
                fetch_attr(&s_gpu.attr[0], indices[vertex], destination->position);
                if (!fetch_attr(color_attr(), indices[vertex], destination->diffuse))
                    nv_cpu_unpack_argb(0xFFFFFFFFu, destination->diffuse);
                fetch_attr(&s_gpu.attr[4], indices[vertex], destination->specular);
                float fog_attribute[4] = {0,0,0,1};
                fetch_attr(&s_gpu.attr[5], indices[vertex], fog_attribute);
                destination->fog_coordinate = fog_attribute[0];
                fetch_attr(&s_gpu.attr[2], indices[vertex], destination->normal);
                fetch_attr(&s_gpu.attr[1], indices[vertex], destination->weights);
                for (stage = 0; stage < 4; stage++)
                    fetch_attr(stage ? &s_gpu.attr[9 + stage] : texcoord_attr(), indices[vertex], destination->texture[stage]);
            }
        }
        s_gpu.shader_vertices += vertex_count;
        return 1;
    }
    for (vertex = 0; vertex < vertex_count; vertex++)
        if (!fetch_shader_vertex(indices[vertex], &vertices[vertex])) return 1;
    for (vertex = 0; vertex < vertex_count; vertex++) {
        for (stage = 0; stage < 4; stage++)
            if (!nv_cpu_finite(vertices[vertex].output[0][stage])) return 1;
    }
    for (vertex = 0; vertex < vertex_count; vertex++) {
        gpu_indices[gpu_index_count++] = *count;
        Nv2aGpuVertex *destination = &gpu_vertices[(*count)++];
        memset(destination, 0, offsetof(Nv2aGpuVertex, attributes));
        s_gpu.gpu_zero_bytes_saved += sizeof destination->attributes;
        memcpy(destination->position, vertices[vertex].output[0], sizeof destination->position);
        memcpy(destination->diffuse, vertices[s_gpu.shade_mode == 0x1D00 ? vertex_count - 1 : vertex].output[3], sizeof destination->diffuse);
        memcpy(destination->specular, vertices[s_gpu.shade_mode == 0x1D00 ? vertex_count - 1 : vertex].output[4], sizeof destination->specular);
        destination->fog_coordinate = vertices[vertex].output[5][0];
        for (stage = 0; stage < 4; stage++) memcpy(destination->texture[stage], vertices[vertex].output[9 + stage], sizeof destination->texture[stage]);
    }
    return 1;
}

static int gpu_append_triangle(Nv2aGpuDraw *state, uint32_t *count, uint32_t first, uint32_t second, uint32_t third)
{
    const uint32_t indices[3] = {first, second, third};
    return gpu_append_primitive(state, count, indices, 3);
}

static int gpu_batch_rejected(const char *reason)
{
    fprintf(stderr, "[GPU-D3D11] rejected batch: %s; format %X stages %X surface %X pitch %u clip %u,%u %ux%u\n",
            reason, s_gpu.format, s_gpu.shader_stage_program, s_gpu.color_offset, s_gpu.pitch,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.clip_w, s_gpu.clip_h);
    return 0;
}

static int gpu_raster_batch(void)
{
    uint8_t *memory = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t count = 0, index, stage;
    uint32_t prior_batches = s_gpu.gpu_batches;
    Nv2aGpuDraw state = {0};
    state.topology = s_gpu.prim == NV_PRIM_POINTS ? NV2A_GPU_TOPOLOGY_POINTS :
        s_gpu.prim <= NV_PRIM_LINE_STRIP ? NV2A_GPU_TOPOLOGY_LINES : NV2A_GPU_TOPOLOGY_TRIANGLES;
    state.point_size = s_gpu.point_size;
    GpuCubeTexture cube_textures[4];
    uint64_t expanded_count = (uint64_t)s_gpu.idx_count * 3;
    uint32_t vertex_capacity = expanded_count < NV2A_GPU_MAX_VERTICES ? (uint32_t)expanded_count : NV2A_GPU_MAX_VERTICES;
    uint32_t index_capacity = expanded_count < NV2A_GPU_MAX_INDICES ? (uint32_t)expanded_count : NV2A_GPU_MAX_INDICES;
    if (vertex_capacity > gpu_vertex_capacity) {
        Nv2aGpuVertex *grown = (Nv2aGpuVertex *)realloc(gpu_vertices, (size_t)vertex_capacity * sizeof *gpu_vertices);
        if (!grown) return gpu_batch_rejected("native vertex staging allocation");
        gpu_vertices = grown; gpu_vertex_capacity = vertex_capacity;
    }
    if (index_capacity > gpu_index_capacity) {
        uint32_t *grown = (uint32_t *)realloc(gpu_indices, (size_t)index_capacity * sizeof *gpu_indices);
        if (!grown) return gpu_batch_rejected("native index staging allocation");
        gpu_indices = grown; gpu_index_capacity = index_capacity;
    }
    gpu_begin_chunk();
    if (!s_gpu.combiners_configured) return gpu_batch_rejected("unconfigured combiners");
    if (surface_bpp() != 4) return gpu_batch_rejected("surface pixel format");
    if (!s_gpu.clip_w || !s_gpu.clip_h || !surface_begin_batch(memory)) return gpu_batch_rejected("surface mapping or clip extent");
    state.color = s_surface; state.width = s_gpu.clip_x + s_gpu.clip_w; state.height = s_gpu.clip_y + s_gpu.clip_h;
    if (state.width > s_gpu.pitch / 4) state.width = s_gpu.pitch / 4;
    state.pitch = s_gpu.pitch; state.bytes_per_pixel = 4;
    state.clip_x = s_gpu.clip_x; state.clip_y = s_gpu.clip_y; state.clip_width = s_gpu.clip_w; state.clip_height = s_gpu.clip_h;
    state.window_clip_valid = s_gpu.window_clip_valid; state.window_clip_type = s_gpu.window_clip_type;
    memcpy(state.window_clip_horizontal, s_gpu.window_clip_horizontal, sizeof state.window_clip_horizontal);
    memcpy(state.window_clip_vertical, s_gpu.window_clip_vertical, sizeof state.window_clip_vertical);
    state.stage_program = s_gpu.shader_stage_program; state.combiners = s_gpu.combiners;
    state.shader_clip_mode = s_gpu.shader_clip_mode;
    state.shader_other_stage_input = s_gpu.shader_other_stage_input;
    state.shader_dot_mapping = s_gpu.shader_dot_mapping;
    memcpy(state.shader_eye_vector, s_gpu.shader_eye_vector, sizeof state.shader_eye_vector);
    state.shader_eye_vector_valid = s_gpu.shader_eye_vector_valid;
    if ((s_gpu.transform_execution_mode & 3u) == 2u) {
        state.vertex_program = s_gpu.vp_program; state.vertex_valid = s_gpu.vp_valid;
        state.vertex_constants = s_gpu.vp_constants; state.vertex_start = s_gpu.vp_start;
        static int fetch_all = -1;
        if (fetch_all < 0) {
            const char *value = getenv("RECOMP_NV2A_VERTEX_FETCH_ALL");
            fetch_all = value && *value && strcmp(value, "0") != 0;
        }
        /* Position is retained for the native vertex snapshot even if the program does not read it. */
        gpu_attribute_mask = fetch_all ? 0xFFFFu :
            nv_cpu_vertex_input_mask(s_gpu.vp_program, s_gpu.vp_valid, s_gpu.vp_start) | 1u;
    } else if ((s_gpu.transform_execution_mode & 3u) == 0u && s_gpu.composite_valid == 0xFFFFu) {
        state.fixed_transform = 1; state.vertex_constants = s_gpu.vp_constants;
        state.lighting_enable = s_gpu.lighting_enable; state.specular_enable = s_gpu.specular_enable;
        state.light_enable_mask = s_gpu.light_enable_mask; state.color_material = s_gpu.color_material;
        state.light_control = s_gpu.light_control;
        state.normalization_enable = s_gpu.normalization_enable; state.skin_mode = s_gpu.skin_mode;
        state.texgen_view_model = s_gpu.texgen_view_model;
        memcpy(state.texgen, s_gpu.texgen, sizeof state.texgen);
        memcpy(state.texture_matrix_enable, s_gpu.texture_matrix_enable, sizeof state.texture_matrix_enable);
        state.material_alpha = s_gpu.material_alpha; state.specular_power = s_gpu.specular_power;
        memcpy(state.scene_ambient, s_gpu.scene_ambient, sizeof state.scene_ambient);
        memcpy(state.material_emission, s_gpu.material_emission, sizeof state.material_emission);
        memcpy(state.light_ambient, s_gpu.light_ambient, sizeof state.light_ambient);
        memcpy(state.light_diffuse, s_gpu.light_diffuse, sizeof state.light_diffuse);
        memcpy(state.light_specular, s_gpu.light_specular, sizeof state.light_specular);
        memcpy(state.light_local_position, s_gpu.light_local_position, sizeof state.light_local_position);
        memcpy(state.light_local_attenuation, s_gpu.light_local_attenuation, sizeof state.light_local_attenuation);
        memcpy(state.light_infinite_direction, s_gpu.light_infinite_direction, sizeof state.light_infinite_direction);
        memcpy(state.light_infinite_half_vector, s_gpu.light_infinite_half_vector, sizeof state.light_infinite_half_vector);
        memcpy(state.light_spot_direction, s_gpu.light_spot_direction, sizeof state.light_spot_direction);
    }
    state.flat_shading = s_gpu.shade_mode == 0x1D00;
    state.alpha_enable = s_gpu.alpha_enable; state.alpha_function = s_gpu.alpha_func; state.alpha_reference = s_gpu.alpha_ref;
    state.control0 = s_gpu.control0; state.depth_enable = s_gpu.depth_enable; state.depth_function = s_gpu.depth_func; state.depth_mask = s_gpu.depth_mask;
    state.zmin_max_control = s_gpu.zmin_max_control; state.depth_range_valid = s_gpu.depth_range_valid;
    state.depth_clip_min = s_gpu.depth_clip_min; state.depth_clip_max = s_gpu.depth_clip_max;
    state.fog_enable = s_gpu.fog_enable; state.fog_mode = s_gpu.fog_mode;
    memcpy(state.fog_parameters, s_gpu.fog_parameters, sizeof state.fog_parameters);
    state.fog_gen_mode = s_gpu.fog_gen_mode;
    memcpy(state.fog_plane, s_gpu.fog_plane, sizeof state.fog_plane);
    state.stencil_enable = s_gpu.stencil_enable; state.stencil_function = s_gpu.stencil_func;
    state.stencil_reference = s_gpu.stencil_ref; state.stencil_read_mask = s_gpu.stencil_read_mask;
    state.stencil_write_mask = s_gpu.stencil_write_mask;
    state.stencil_fail = s_gpu.stencil_fail; state.stencil_depth_fail = s_gpu.stencil_depth_fail; state.stencil_pass = s_gpu.stencil_pass;
    state.cull_enable = s_gpu.cull_enable; state.cull_face = s_gpu.cull_face; state.front_face = s_gpu.front_face;
    state.polygon_front = s_gpu.polygon_front; state.polygon_back = s_gpu.polygon_back;
    state.polygon_offset_enable = s_gpu.polygon_offset_enable;
    state.polygon_offset_scale = s_gpu.polygon_offset_scale; state.polygon_offset_bias = s_gpu.polygon_offset_bias;
    state.depth_format = (s_gpu.format >> 4) & 15u; state.depth_pitch = s_gpu.depth_pitch;
    if (state.depth_enable || state.stencil_enable) {
        uint32_t base = dma_resolve(s_gpu.depth_offset);
        if (!s_gpu.depth_offset) return gpu_batch_rejected("missing depth surface");
        if (surface_write_refused(base, state.height * state.depth_pitch, "GPU depth")) return gpu_batch_rejected("depth surface memory extent");
        state.depth = memory + base;
    }
    state.blend_enable = s_gpu.blend_enable; state.blend_source = s_gpu.blend_sfactor; state.blend_destination = s_gpu.blend_dfactor;
    state.blend_equation = s_gpu.blend_equation; state.blend_constant = s_gpu.blend_constant; state.color_mask = s_gpu.color_mask;
    for (stage = 0; stage < 4; stage++) {
        const Texture *texture = stage ? &s_gpu.extra_tex[stage - 1] : &s_gpu.tex;
        Nv2aGpuTexture *binding = &state.textures[stage];
        uint64_t bytes;
        uint32_t mode = (state.stage_program >> (stage * 5)) & 31;
        uint32_t block_bytes = d3d8_format_dxt_block_bytes(texture->color);
        uint32_t bytes_per_pixel = 0;
        binding->control0 = texture->control0; binding->control0_valid = texture->control0_valid;
        binding->filter = texture->filter;
        if (!nv2a_gpu_texture_mode_samples(mode)) continue;
        if (!nv2a_gpu_texture_enabled(binding)) return gpu_batch_rejected("disabled texture used by sampling shader mode");
        if (!texture->valid) return gpu_batch_rejected("invalid texture binding");
        if (((texture->raw_format >> 4) & 15u) == 3u)
            binding->depth = 1u << (texture->raw_format >> 28);
        if ((mode == 2) != (binding->depth != 0) || (binding->depth && (texture->raw_format & 4u)))
            return gpu_batch_rejected("projective volume texture dimensionality");
        binding->source = memory + texture->offset; binding->width = texture->width; binding->height = texture->height;
        binding->pitch = texture->pitch; binding->format = texture->color; binding->linear = !tex_size_from_format(texture->color);
        binding->address_u = texture->addr_u; binding->address_v = texture->addr_v;
        binding->address_w = texture->addr_w;
        binding->border_color = texture->border_color;
        binding->color_key = texture->color_key;
        memcpy(binding->bump_matrix, texture->bump_matrix, sizeof binding->bump_matrix);
        binding->bump_scale = texture->bump_scale; binding->bump_offset = texture->bump_offset;
        binding->mip_levels = (texture->raw_format >> 16) & 15u;
        if (!binding->mip_levels) binding->mip_levels = 1;
        uint32_t maximum_levels = 1, maximum_dimension = texture->width > texture->height ? texture->width : texture->height;
        if (binding->depth > maximum_dimension) maximum_dimension = binding->depth;
        while (maximum_dimension > 1) { maximum_dimension >>= 1; maximum_levels++; }
        if (binding->mip_levels > maximum_levels || (binding->linear && binding->mip_levels != 1))
            return gpu_batch_rejected("texture mip count or linear mip layout");
        binding->decode = gpu_decode_texture; binding->decode_context = (void *)texture;
        if (block_bytes) bytes = (uint64_t)((texture->width + 3) / 4) * ((texture->height + 3) / 4) * block_bytes;
        else {
            bytes_per_pixel = texture_bytes_per_pixel(texture->color);
            if (!bytes_per_pixel) return gpu_batch_rejected("texture pixel format");
            if (binding->linear && (uint64_t)texture->pitch < (uint64_t)texture->width * bytes_per_pixel)
                return gpu_batch_rejected("linear texture pitch");
            if ((texture->color == 0x24 || texture->color == 0x25) && (texture->width & 1u))
                return gpu_batch_rejected("packed YUV texture width");
            bytes = binding->linear ? (uint64_t)texture->pitch * texture->height :
                (uint64_t)texture->width * texture->height * bytes_per_pixel;
        }
        uint64_t face_bytes = bytes * (binding->depth ? binding->depth : 1u);
        for (uint32_t level = 1; level < binding->mip_levels; level++) {
            uint32_t width = texture->width >> level, height = texture->height >> level;
            if (!width) width = 1;
            if (!height) height = 1;
            uint32_t depth = binding->depth >> level;
            if (!depth) depth = 1;
            if (block_bytes) face_bytes += (uint64_t)((width + 3) / 4) * ((height + 3) / 4) * block_bytes * depth;
            else face_bytes += (uint64_t)width * height * bytes_per_pixel * depth;
        }
        cube_textures[stage].texture = *texture;
        cube_textures[stage].bytes_per_pixel = bytes_per_pixel;
        cube_textures[stage].mip_levels = binding->mip_levels;
        cube_textures[stage].face_stride = 0;
        cube_textures[stage].depth = binding->depth;
        bytes = face_bytes;
        if (texture->raw_format & 4u) {
            if (texture->width != texture->height || binding->linear) {
                fprintf(stderr, "[GPU-D3D11] cube layout: stage %u raw %08X color %X %ux%u pitch %u source %X\n",
                        stage, texture->raw_format, texture->color, texture->width, texture->height, texture->pitch, texture->offset);
                return gpu_batch_rejected("cube texture format or dimensions");
            }
            face_bytes = (face_bytes + 127u) & ~127ull;
            if (face_bytes > UINT32_MAX / 6u) return gpu_batch_rejected("cube texture memory extent");
            binding->cube = 1; binding->face_stride = (uint32_t)face_bytes;
            cube_textures[stage].texture = *texture; cube_textures[stage].face_stride = binding->face_stride;
            binding->decode_context = &cube_textures[stage]; binding->decode_face = gpu_decode_cube_texture;
            bytes = face_bytes * 6;
        }
        if (binding->mip_levels > 1) {
            binding->decode_context = &cube_textures[stage]; binding->decode_level = gpu_decode_mip_texture;
        }
        if (binding->depth) {
            binding->decode_context = &cube_textures[stage];
            binding->decode_volume = gpu_decode_volume_texture;
        }
        if (!bytes || (uint64_t)physical_alias(texture->offset) + bytes > XBOX_CONTIG_SIZE) return gpu_batch_rejected("texture memory extent");
        binding->source_bytes = (uint32_t)bytes;
    }
    switch (s_gpu.prim) {
    case NV_PRIM_POINTS:
        for (index = 0; index < s_gpu.idx_count; index++)
            if (!gpu_append_primitive(&state, &count, &s_gpu.idx[index], 1)) return 0;
        break;
    case NV_PRIM_LINES:
        for (index = 0; index + 1 < s_gpu.idx_count; index += 2)
            if (!gpu_append_primitive(&state, &count, &s_gpu.idx[index], 2)) return 0;
        break;
    case NV_PRIM_LINE_STRIP: case NV_PRIM_LINE_LOOP:
        for (index = 0; index + 1 < s_gpu.idx_count; index++)
            if (!gpu_append_primitive(&state, &count, &s_gpu.idx[index], 2)) return 0;
        if (s_gpu.prim == NV_PRIM_LINE_LOOP) {
            const uint32_t closing[2] = {s_gpu.idx[s_gpu.idx_count - 1], s_gpu.idx[0]};
            if (!gpu_append_primitive(&state, &count, closing, 2)) return 0;
        }
        break;
    case NV_PRIM_TRIANGLES:
        for (index = 0; index + 2 < s_gpu.idx_count; index += 3)
            if (!gpu_append_triangle(&state, &count, s_gpu.idx[index], s_gpu.idx[index+1], s_gpu.idx[index+2])) return 0;
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (index = 0; index + 2 < s_gpu.idx_count; index++)
            if (!gpu_append_triangle(&state, &count, s_gpu.idx[index + (index & 1u)],
                                     s_gpu.idx[index + ((index & 1u) ? 0 : 1)], s_gpu.idx[index+2])) return 0;
        break;
    case NV_PRIM_TRIANGLE_FAN: case NV_PRIM_POLYGON:
        for (index = 1; index + 1 < s_gpu.idx_count; index++)
            if (!gpu_append_triangle(&state, &count, s_gpu.idx[0], s_gpu.idx[index], s_gpu.idx[index+1])) return 0;
        break;
    case NV_PRIM_QUADS:
        for (index = 0; index + 3 < s_gpu.idx_count; index += 4)
            if (!gpu_append_triangle(&state, &count, s_gpu.idx[index], s_gpu.idx[index+1], s_gpu.idx[index+2]) ||
                !gpu_append_triangle(&state, &count, s_gpu.idx[index], s_gpu.idx[index+2], s_gpu.idx[index+3])) return 0;
        break;
    case NV_PRIM_QUAD_STRIP:
        for (index = 0; index + 3 < s_gpu.idx_count; index += 2)
            if (!gpu_append_triangle(&state, &count, s_gpu.idx[index], s_gpu.idx[index+1], s_gpu.idx[index+3]) ||
                !gpu_append_triangle(&state, &count, s_gpu.idx[index], s_gpu.idx[index+3], s_gpu.idx[index+2])) return 0;
        break;
    default: return 0;
    }
    if (!gpu_submit_chunk(&state, &count)) return 0;
    if (s_gpu.batch_array32_count && s_gpu.gpu_batches != prior_batches) s_gpu.native_array32_batches++;
    return 1;
}

static int gpu_submit_chunk(Nv2aGpuDraw *state, uint32_t *count)
{
    uint32_t index;
    if (!*count) return 1;
    state->indices = gpu_indices; state->index_count = gpu_index_count;
    double started = gpu_clock_seconds();
    int drawn = nv2a_gpu_draw(state, gpu_vertices, *count);
    s_gpu.gpu_submit_seconds += gpu_clock_seconds() - started;
    if (!drawn) {
        static uint32_t seen[16], shown;
        uint32_t key = state->stage_program ^ state->control0 ^ (state->depth_enable << 24) ^ (state->blend_source << 16) ^ state->blend_destination;
        for (index = 0; index < shown; index++) if (seen[index] == key) return 0;
        if (shown < 16) {
            seen[shown++] = key;
            fprintf(stderr, "[GPU-D3D11] unsupported: stages %X combiner %X depth %u/%X control %X blend %u/%X/%X/%X texture %X/%X position %.2f %.2f %.2f %.2f\n",
                state->stage_program, state->combiners.control, state->depth_enable, state->depth_function, state->control0,
                state->blend_enable, state->blend_source, state->blend_destination, state->blend_equation,
                state->textures[0].format, state->textures[1].format,
                gpu_vertices[0].position[0], gpu_vertices[0].position[1], gpu_vertices[0].position[2], gpu_vertices[0].position[3]);
        }
        return 0;
    }
    s_gpu.gpu_batches++;
    if (state->topology == NV2A_GPU_TOPOLOGY_TRIANGLES) s_gpu.tris_drawn += gpu_index_count / 3;
    s_gpu.drawn_offset = s_gpu.color_offset;
    *count = 0;
    return 1;
}
#endif

static void raster_batch(void)
{
#ifndef _WIN32
    uint32_t i;
#endif

    if (s_gpu.idx_count < (s_gpu.prim == NV_PRIM_POINTS ? 1u : s_gpu.prim <= NV_PRIM_LINE_STRIP ? 2u : 3u))
        return;
    if ((s_gpu.transform_execution_mode & 3u) != 2u && s_gpu.composite_valid != 0xFFFFu && !batch_is_screen_space()) {
        static uint32_t shown;
        if (shown++ < 8) {
            float input[4];
            fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], input);
            fprintf(stderr, "[GPU] skipped fixed draw: mode %X count %u input %.4f %.4f %.4f %.4f\n",
                    s_gpu.transform_execution_mode, s_gpu.idx_count, input[0], input[1], input[2], input[3]);
        }
        s_gpu.batches_untransformed++;
        return;
    }

    /* Count why, once per batch: the texture stage cannot change inside one. */
    {
        const VertexAttr *tc = texcoord_attr();

        if (!(tc->offset && tc->stride))
            s_gpu.batches_no_uv++;
        else if (!s_gpu.tex.valid)
            s_gpu.batches_no_tex++;
        else {
            s_gpu.batches_textured++;
            note_texture_use();
        }
    }

#ifdef _WIN32
    double started = gpu_clock_seconds();
    double prior_submit = s_gpu.gpu_submit_seconds;
    int rendered = gpu_raster_batch();
    s_gpu.gpu_prepare_seconds += gpu_clock_seconds() - started - (s_gpu.gpu_submit_seconds - prior_submit);
    if (rendered) return;
    fprintf(stderr, "[GPU-D3D11] unsupported batch: primitive %u count %u; hardware rendering required\n", s_gpu.prim, s_gpu.idx_count);
    for (uint32_t i = 0; i < 4; i++) {
        const Texture *texture = i ? &s_gpu.extra_tex[i - 1] : &s_gpu.tex;
        fprintf(stderr, "[GPU-D3D11] texture %u: mode %u format %08X control %08X offset %08X size %ux%u pitch %u valid %u\n",
            i, (s_gpu.shader_stage_program >> (i * 5)) & 31u, texture->raw_format,
            texture->control0, texture->offset, texture->width, texture->height, texture->pitch, texture->valid);
    }
    fflush(stderr);
    _Exit(EXIT_FAILURE);
#else
    s_gpu.cpu_batches++;
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < s_gpu.idx_count; i += 3)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[0], s_gpu.idx[i], s_gpu.idx[i+1],
                           vertex_color(s_gpu.idx[0]));
        break;
    case NV_PRIM_QUADS:
        /* Independent quads, four vertices each. A batch of eight is two
         * quads, not one six-triangle fan around the first vertex; with
         * exactly four the two agreed, which is why sharing the fan arm
         * looked right. */
        for (i = 0; i + 3 < s_gpu.idx_count; i += 4) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+2], s_gpu.idx[i+3],
                           vertex_color(s_gpu.idx[i]));
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        /* Each vertex pair past the first closes another quad against the
         * pair before it. */
        for (i = 0; i + 3 < s_gpu.idx_count; i += 2) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+3],
                           vertex_color(s_gpu.idx[i]));
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+3], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        }
        break;
    default:
        break;                             /* points and lines: not yet */
    }

    if (s_gpu.tris_drawn && (s_gpu.tris_drawn % 500) == 0)
        fprintf(stderr, "  [GPU] %u triangles rasterised\n", s_gpu.tris_drawn);
#endif
}

/* What a batch actually contains. Before anything can be rasterised, the
 * question is what space attribute 0 arrives in: a title running a vertex
 * program hands over object-space positions that mean nothing without running
 * it, while pre-transformed screen-space coordinates can be drawn directly. */
static void draw_primitive(void)
{
    float v[4];
    uint32_t i;

    if (!s_gpu.prim || !s_gpu.idx_count)
        return;
    s_gpu.draws++;
    if ((s_gpu.draws % 200) == 0 && getenv("RECOMP_PB_EXEC_VERBOSE"))
        fprintf(stderr, "  [GPU] draw #%u\n", s_gpu.draws);
    s_gpu.verts += s_gpu.idx_count;

    /* How many batches carry coordinates at all, and what range they span.
     * A pipeline that decodes perfectly and draws nothing is indistinguishable
     * from one that never ran, unless the vertices themselves are measured. */
    {
        float p[4];
        if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p)) {
            if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) {
                s_gpu.nonzero_draws++;
                if (p[0] < s_gpu.min_x) s_gpu.min_x = p[0];
                if (p[0] > s_gpu.max_x) s_gpu.max_x = p[0];
                if (p[1] < s_gpu.min_y) s_gpu.min_y = p[1];
                if (p[1] > s_gpu.max_y) s_gpu.max_y = p[1];
            }
        }
    }

    {
        clock_t started = clock();
        raster_batch();
        s_gpu.raster_seconds += (double)(clock() - started) / CLOCKS_PER_SEC;
    }

    if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
        static int shown;
        if (shown++ < 6) {
            fprintf(stderr, "  [GPU] prim %u, %u indices, pos attr:"
                            " off 0x%08X type %u size %u stride %u\n",
                    s_gpu.prim, s_gpu.idx_count, s_gpu.attr[0].offset,
                    s_gpu.attr[0].type, s_gpu.attr[0].size, s_gpu.attr[0].stride);
            /* The texture stage, for either kind of batch. This used to print
             * only for inline batches, which meant a title drawing through
             * vertex arrays -- Half-Life 2's menu, for one -- showed no
             * texture state at all, and the reason a quad sampled flat was
             * invisible. */
            fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                            " colour 0x%02X swizzled %d valid %d\n",
                    s_gpu.tex.offset, s_gpu.tex.width, s_gpu.tex.height,
                    s_gpu.tex.pitch, s_gpu.tex.color,
                    d3d8_format_is_swizzled(s_gpu.tex.color), s_gpu.tex.valid);
            {
                uint32_t k;
                for (k = 0; k < s_gpu.idx_count && k < 3; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
            }
            /* An inline batch has no guest buffer to go and look at -- the
             * vertices are the payload -- so print the payload too. */
            if (s_gpu.inline_active) {
                uint32_t k;
                fprintf(stderr, "  [GPU]   inline %u dwords:", s_gpu.inline_count);
                for (k = 0; k < s_gpu.inline_count && k < 16; k++)
                    fprintf(stderr, " %08X", s_gpu.inline_buf[k]);
                fprintf(stderr, "\n");
                for (k = 0; k < s_gpu.idx_count && k < 4; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
                fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                                " colour 0x%02X valid %d\n",
                        s_gpu.tex.offset, s_gpu.tex.width, s_gpu.tex.height,
                        s_gpu.tex.pitch, s_gpu.tex.color, s_gpu.tex.valid);
            }
            {
                /* Every attribute the batch has, not just position. If the
                 * other streams carry data and position does not, the problem
                 * is one buffer rather than the whole vertex path. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t a, k;
                for (a = 0; a < NV_VERTEX_ATTRS; a++) {
                    const VertexAttr *at = &s_gpu.attr[a];
                    uint32_t nz = 0;
                    if (!at->offset || !at->size)
                        continue;
                    for (k = 0; k < 64; k++)
                        if (mem[at->offset + k]) nz++;
                    fprintf(stderr, "  [GPU]   attr%-2u off 0x%08X type %u"
                                    " size %u stride %-3u  %u/64 bytes set\n",
                            a, at->offset, at->type, at->size, at->stride, nz);
                }
            }
            {
                /* Raw bytes at the array, in case the values read as zero:
                 * that looks the same whether the offset is wrong or the
                 * buffer genuinely has not been filled yet. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t k;
                fprintf(stderr, "  [GPU]   bytes @0x%08X:", s_gpu.attr[0].offset);
                for (k = 0; k < 32; k++)
                    fprintf(stderr, " %02X", mem[s_gpu.attr[0].offset + k]);
                fprintf(stderr, "\n");
                fprintf(stderr, "  [GPU]   indices:");
                for (k = 0; k < s_gpu.idx_count && k < 8; k++)
                    fprintf(stderr, " %u", s_gpu.idx[k]);
                fprintf(stderr, "\n");
            }
            for (i = 0; i < s_gpu.idx_count && i < 3; i++) {
                if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], v))
                    fprintf(stderr, "  [GPU]   v[%u] = %.3f %.3f %.3f %.3f\n",
                            s_gpu.idx[i], v[0], v[1], v[2], v[3]);
            }
        }
    }
}

/* Draw the vertices the title wrote straight into the pushbuffer.
 *
 * INLINE_ARRAY carries no offsets and no indices: the dwords between BEGIN and
 * END *are* the vertex buffer, packed in attribute order using the same
 * SET_VERTEX_DATA_ARRAY_FORMAT registers an ordinary array would use. So the
 * whole batch is describable as a vertex array whose base happens to be that
 * payload, which means synthesising the layout and handing it to the existing
 * path -- rather than a second copy of the topology and rasterisation code.
 *
 * The title's own attribute table is saved and put back: these offsets and
 * strides are ours, and it has not stopped using its.
 *
 * ponytail: each attribute is padded to a whole dword. That is exact for the
 * float and D3DCOLOR formats every inline batch actually uses; a packed
 * sub-dword attribute would need the unpadded layout.
 */
static void require_index_capacity(uint32_t count, uint32_t method);

static void draw_inline_array(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t off = 0, a, i, vsize, count;

    memcpy(saved, s_gpu.attr, sizeof saved);

    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        uint32_t bytes;
        if (!s_gpu.attr[a].size)
            continue;
        switch (s_gpu.attr[a].type) {
        case 0:  bytes = 4;                        break;  /* D3DCOLOR   */
        case 1: case 5: bytes = 2 * s_gpu.attr[a].size; break;
        case 6: bytes = 4; break;
        case 2:  bytes = 4 * s_gpu.attr[a].size;   break;  /* float      */
        case 4:  bytes = s_gpu.attr[a].size;       break;  /* ubyte norm */
        default: bytes = 4 * s_gpu.attr[a].size;   break;
        }
        s_gpu.attr[a].offset = off;
        off += (bytes + 3u) & ~3u;
    }
    vsize = off;
    if (!vsize)
        goto out;

    count = (s_gpu.inline_count * 4) / vsize;
    if (count < (s_gpu.prim == NV_PRIM_POINTS ? 1u : s_gpu.prim <= NV_PRIM_LINE_STRIP ? 2u : 3u))
        goto out;
    require_index_capacity(count, NV097_INLINE_ARRAY);
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].size)
            s_gpu.attr[a].stride = vsize;

    for (i = 0; i < count; i++)
        s_gpu.idx[i] = i;
    s_gpu.idx_count = count;

    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

out:
    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
}

/* Draw the vertices SET_VERTEX3F/4F completed.
 *
 * Same trick as draw_inline_array: rather than a second copy of the topology
 * and rasterisation code, describe what was accumulated as an ordinary vertex
 * array and hand it to the existing path. The layout is ours and fixed, so
 * the attribute table is written out here rather than derived from the
 * title's format registers.
 *
 * The title's own table is saved and put back -- it has not stopped using it.
 *
 * The compact position/color/texcoord payload keeps the existing batching
 * capacity; fetch_attr reads the full attribute snapshot for each vertex.
 */
static void draw_immediate(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t i;

    if (s_gpu.imm_count < (s_gpu.prim == NV_PRIM_POINTS ? 1u : s_gpu.prim <= NV_PRIM_LINE_STRIP ? 2u : 3u))
        return;

    memcpy(saved, s_gpu.attr, sizeof saved);
    memset(s_gpu.attr, 0, sizeof s_gpu.attr);
    /* Offsets are byte offsets into inline_buf here, not guest addresses --
     * fetch_attr reads them that way while inline_active is set, which is
     * also why 0 is a legal offset for position. */
    s_gpu.attr[0].type = 2; s_gpu.attr[0].size = 4;   /* position float4  */
    s_gpu.attr[0].offset = 0;
    s_gpu.attr[3].type = 0; s_gpu.attr[3].size = 4;   /* diffuse D3DCOLOR */
    s_gpu.attr[3].offset = 16;
    s_gpu.attr[9].type = 2; s_gpu.attr[9].size = 2;   /* texcoord0 float2 */
    s_gpu.attr[9].offset = 20;
    s_gpu.attr[0].stride = s_gpu.attr[3].stride = s_gpu.attr[9].stride =
        IMM_VERTEX_DWORDS * 4;

    require_index_capacity(s_gpu.imm_count, NV097_SET_VERTEX4F);
    for (i = 0; i < s_gpu.imm_count; i++)
        s_gpu.idx[i] = i;
    s_gpu.idx_count = i;

    /* fetch_attr bounds-checks against inline_count dwords. */
    s_gpu.inline_count = s_gpu.imm_count * IMM_VERTEX_DWORDS;
    s_gpu.inline_active = 1;
    s_gpu.immediate_active = 1;
    draw_primitive();
    s_gpu.immediate_active = 0;
    s_gpu.inline_active = 0;

    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
    s_gpu.inline_count = 0;
}

/* A vertex is complete: append it in the layout draw_immediate describes. */
static void imm_emit_vertex(void)
{
    uint32_t at = s_gpu.imm_count * IMM_VERTEX_DWORDS;

    if (!s_gpu.prim || at + IMM_VERTEX_DWORDS > NV_MAX_INLINE)
        return;
    for (uint32_t attribute = 0; attribute < NV_VERTEX_ATTRS; attribute++) {
        float *value = s_gpu.imm_attributes[s_gpu.imm_count][attribute];
        if (s_gpu.uniform_valid & (1u << attribute))
            memcpy(value, s_gpu.uniform_attr[attribute], sizeof(float) * 4);
        else {
            value[0] = value[1] = value[2] = 0.0f;
            value[3] = 1.0f;
        }
    }
    memcpy(s_gpu.imm_attributes[s_gpu.imm_count][0], s_gpu.imm_pos, sizeof s_gpu.imm_pos);
    nv_cpu_unpack_argb(s_gpu.imm_diffuse, s_gpu.imm_attributes[s_gpu.imm_count][3]);
    memcpy(s_gpu.imm_attributes[s_gpu.imm_count][9], s_gpu.imm_tex, sizeof s_gpu.imm_tex);
    memcpy(&s_gpu.inline_buf[at],     s_gpu.imm_pos, 4 * sizeof(float));
    memcpy(&s_gpu.inline_buf[at + 4], &s_gpu.imm_diffuse, sizeof(uint32_t));
    memcpy(&s_gpu.inline_buf[at + 5], s_gpu.imm_tex, 2 * sizeof(float));
    s_gpu.imm_count++;
}

/* The immediate-mode writes. Returns 1 if `method` was one of them.
 *
 * Split out because it is a range test against five separate bases, and that
 * reads better than five more cases in an already long switch.
 */
static int imm_vertex_method(uint32_t method, uint32_t param)
{
    union { uint32_t u; float f; } v;
    v.u = param;
    if (method >= 0x1530 && method < 0x153C) {
        s_gpu.uniform_attr[2][(method - 0x1530) >> 2] = v.f;
        s_gpu.uniform_valid |= 1u << 2;
        return 1;
    }
    if (method == 0x1540 || method == 0x1544) {
        uint32_t component = (method - 0x1540) >> 1;
        s_gpu.uniform_attr[2][component] = fmaxf(-1.0f, (float)(int16_t)(param & 0xFFFFu) / 32767.0f);
        s_gpu.uniform_attr[2][component + 1] = fmaxf(-1.0f, (float)(int16_t)(param >> 16) / 32767.0f);
        s_gpu.uniform_valid |= 1u << 2;
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2F_M && method < NV097_SET_VERTEX_DATA2F_M + NV_VERTEX_ATTRS * 8) {
        uint32_t attribute = (method - NV097_SET_VERTEX_DATA2F_M) / 8;
        uint32_t component = ((method - NV097_SET_VERTEX_DATA2F_M) & 7u) / 4;
        if (!(s_gpu.uniform_valid & (1u << attribute))) s_gpu.uniform_attr[attribute][3] = 1.0f;
        s_gpu.uniform_attr[attribute][component] = v.f;
        s_gpu.uniform_valid |= 1u << attribute;
    }
    if (method >= NV097_SET_VERTEX_DATA4F_M && method < NV097_SET_VERTEX_DATA4F_M + NV_VERTEX_ATTRS * 16) {
        uint32_t attribute = (method - NV097_SET_VERTEX_DATA4F_M) / 16;
        uint32_t component = ((method - NV097_SET_VERTEX_DATA4F_M) & 15u) / 4;
        if (!(s_gpu.uniform_valid & (1u << attribute))) s_gpu.uniform_attr[attribute][3] = 1.0f;
        s_gpu.uniform_attr[attribute][component] = v.f;
        s_gpu.uniform_valid |= 1u << attribute;
        if (attribute == 3) s_gpu.imm_diffuse = nv_cpu_pack_argb(s_gpu.uniform_attr[attribute]);
    }
    if (method >= NV097_SET_VERTEX_DATA4UB && method < NV097_SET_VERTEX_DATA4UB + NV_VERTEX_ATTRS * 4) {
        uint32_t attribute = (method - NV097_SET_VERTEX_DATA4UB) / 4;
        uint32_t argb = (param & 0xFF00FF00u) | ((param & 255u) << 16) | ((param >> 16) & 255u);
        nv_cpu_unpack_argb(argb, s_gpu.uniform_attr[attribute]);
        s_gpu.uniform_valid |= 1u << attribute;
    }

    if (method >= NV097_SET_VERTEX4F && method < NV097_SET_VERTEX4F + 16) {
        uint32_t c = (method - NV097_SET_VERTEX4F) / 4;
        s_gpu.imm_pos[c] = v.f;
        if (c == 3)                      /* w completes the vertex */
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX3F && method < NV097_SET_VERTEX3F + 12) {
        uint32_t c = (method - NV097_SET_VERTEX3F) / 4;
        s_gpu.imm_pos[c] = v.f;
        if (c == 2) {                    /* z completes it, w is implicitly 1 */
            s_gpu.imm_pos[3] = 1.0f;
            imm_emit_vertex();
        }
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2F_M
            && method < NV097_SET_VERTEX_DATA2F_M + NV_VERTEX_ATTRS * 8) {
        uint32_t off = method - NV097_SET_VERTEX_DATA2F_M;
        if (off / 8 == 0) {
            uint32_t component = (off % 8) / 4;
            s_gpu.imm_pos[component] = v.f;
            if (component == 1) {
                s_gpu.imm_pos[2] = 0.0f;
                s_gpu.imm_pos[3] = 1.0f;
                imm_emit_vertex();
            }
        } else if (off / 8 == 9) {       /* attribute 9 is texture coord 0 */
            s_gpu.imm_tex[(off % 8) / 4] = v.f;
        }
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4F_M
            && method < NV097_SET_VERTEX_DATA4F_M + NV_VERTEX_ATTRS * 16) {
        uint32_t off = method - NV097_SET_VERTEX_DATA4F_M;
        uint32_t attr = off / 16, c = (off % 16) / 4;
        if (attr == 0) {
            s_gpu.imm_pos[c] = v.f;
            if (c == 3)
                imm_emit_vertex();
        } else if (attr == 9 && c < 2) {
            s_gpu.imm_tex[c] = v.f;
        }
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4UB
            && method < NV097_SET_VERTEX_DATA4UB + NV_VERTEX_ATTRS * 4) {
        if ((method - NV097_SET_VERTEX_DATA4UB) / 4 == 3)   /* diffuse */
            s_gpu.imm_diffuse = (param & 0xFF00FF00u) | ((param & 255u) << 16) | ((param >> 16) & 255u);
        return 1;
    }
    return 0;
}

static void require_index_capacity(uint32_t count, uint32_t method)
{
    uint64_t required = (uint64_t)s_gpu.idx_count + count;
    uint32_t capacity;
    uint32_t *grown;
    if (!s_gpu.prim || required > UINT32_MAX - 3u) {
        fprintf(stderr, "[GPU] index command rejected: method %04X primitive %u count %u + %u\n",
                method, s_gpu.prim, s_gpu.idx_count, count);
        fflush(stderr);
        _Exit(EXIT_FAILURE);
    }
    if (required <= s_gpu.idx_capacity) return;
    capacity = s_gpu.idx_capacity ? s_gpu.idx_capacity : 4096;
    while (capacity < required) {
        if (capacity > (UINT32_MAX - 3u) / 2) { capacity = (uint32_t)required; break; }
        capacity *= 2;
    }
    if ((uint64_t)capacity * sizeof *s_gpu.idx > SIZE_MAX) {
        fprintf(stderr, "[GPU] index storage extent rejected: %u elements\n", capacity);
        fflush(stderr);
        _Exit(EXIT_FAILURE);
    }
    grown = (uint32_t *)realloc(s_gpu.idx, (size_t)capacity * sizeof *s_gpu.idx);
    if (!grown) {
        fprintf(stderr, "[GPU] index storage allocation failed: %u elements\n", capacity);
        fflush(stderr);
        _Exit(EXIT_FAILURE);
    }
    s_gpu.idx = grown; s_gpu.idx_capacity = capacity;
}

static void append_vertex_index(uint32_t index)
{
    s_gpu.idx[s_gpu.idx_count++] = index;
    if (s_gpu.idx_count > s_gpu.max_index_count) s_gpu.max_index_count = s_gpu.idx_count;
    if (index > 65535u) s_gpu.wide_indices++;
}

void nv2a_pb_exec_indices(uint32_t method, const uint32_t *parameters, uint32_t count)
{
    if (!parameters || !count || count > 2047u ||
        (method != NV097_ARRAY_ELEMENT16 && method != NV097_ARRAY_ELEMENT32)) {
        fprintf(stderr, "[GPU] invalid bulk index packet: method %04X count %u\n", method, count);
        fflush(stderr);
        _Exit(EXIT_FAILURE);
    }
    require_index_capacity(count * (method == NV097_ARRAY_ELEMENT16 ? 2u : 1u), method);
    for (uint32_t word = 0; word < count; word++) {
        uint32_t parameter = parameters[word];
        if (method == NV097_ARRAY_ELEMENT16) {
            append_vertex_index(parameter & 0xFFFFu);
            append_vertex_index(parameter >> 16);
        } else append_vertex_index(parameter);
    }
    if (method == NV097_ARRAY_ELEMENT16) s_gpu.array16_indices += (uint64_t)count * 2;
    else {
        s_gpu.array32_indices += count;
        s_gpu.batch_array32_count += count;
    }
}

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    static int inited;
    static int verbose;
    if (!inited) {
        inited = 1;
        verbose = getenv("RECOMP_PB_EXEC_VERBOSE") != NULL;
        s_gpu.blend_equation = 0x8006;
        s_gpu.color_mask = 0x01010101u;
        s_gpu.min_x = s_gpu.min_y = 1e30f;
        s_gpu.max_x = s_gpu.max_y = -1e30f;
    }
    /* Bring-up: the first parameters each surface method carries. A wrong
     * pitch or clip is indistinguishable from a method never arriving unless
     * the values are visible. */
    if (verbose) {
        static int shown[8];
        static unsigned draw_shown;
        static unsigned other_subch_shown;
        static unsigned batch_number;
        static unsigned batch_methods;
        static int batch_active;
        int slot = -1;
        if (subch == 0 && method == NV097_SET_BEGIN_END && param) {
            batch_number++;
            batch_methods = 0;
            batch_active = 1;
        }
        if (batch_active && batch_number <= 2 && subch == 0 && batch_methods++ < 80)
            fprintf(stderr, "  [GPU] batch %u method 0x%04X param 0x%08X\n",
                    batch_number, method, param);
        if (subch == 0 && method == NV097_SET_BEGIN_END && !param)
            batch_active = 0;
        if ((method == NV097_SET_BEGIN_END || method == NV097_DRAW_ARRAYS)
            && draw_shown++ < 16)
            fprintf(stderr, "  [GPU] draw method subch %u method 0x%04X param 0x%08X\n",
                subch, method, param);
        if (subch != 0 && other_subch_shown++ < 16)
            fprintf(stderr, "  [GPU] other subch %u method 0x%04X param 0x%08X\n",
                subch, method, param);
        switch (method) {
        case NV097_SET_SURFACE_CLIP_HORIZONTAL: slot = 0; break;
        case NV097_SET_SURFACE_CLIP_VERTICAL:   slot = 1; break;
        case NV097_SET_SURFACE_FORMAT:          slot = 2; break;
        case NV097_SET_SURFACE_PITCH:           slot = 3; break;
        case NV097_SET_SURFACE_COLOR_OFFSET:    slot = 4; break;
        case NV097_SET_COLOR_CLEAR_VALUE:       slot = 5; break;
        case NV097_CLEAR_SURFACE:               slot = 6; break;
        default: break;
        }
        if (slot >= 0 && shown[slot]++ < 4)
            fprintf(stderr, "  [GPU] subch %u method 0x%04X param 0x%08X\n",
                    subch, method, param);
    }

    if (xbox_Nv2aNativeFencesEnabled() && subch < 8) {
        static uint32_t classes[8];
        if (method == 0) {
            uint32_t instance;
            if (!ramht_instance(param, &instance)) {
                fflush(stderr);
                _Exit(EXIT_FAILURE);
            }
            volatile uint32_t *object = xbox_Nv2aRegisterPointer(0x700000 + instance, 4);
            if (!object) {
                fflush(stderr);
                _Exit(EXIT_FAILURE);
            }
            classes[subch] = *object & 0xFFFu;
            return;
        }
        if (classes[subch] == 0x44u && method == 0x0310) {
            volatile uint32_t *color = xbox_Nv2aRegisterPointer(0x400B10, 4);
            if (!color) {
                fflush(stderr);
                _Exit(EXIT_FAILURE);
            }
            /* Xbox D3D uses pattern COLOR0 as its consumed-buffer checkpoint. */
            *color = param;
            return;
        }
    }
    if (subch != 0) {                      /* 3D class lives on subchannel 0 */
        note_unhandled(subch, method, param);
        return;
    }
    if (method == NV097_WAIT_FOR_IDLE) {
        s_gpu.flush_wait_idle++;
        pb_flush_ordering();
        return;
    }
    if (method == 0x0100) {
        if (!param)
            return;
        if (xbox_Nv2aNativeFencesEnabled()) {
            s_gpu.flush_fence++;
            nv2a_pb_exec_flush();
            if (xbox_Nv2aSoftwareMethod(param, s_gpu.depth_clear,
                                       s_gpu.clear_color))
                return;
        }
        note_unhandled(subch, method, param);
        return;
    }
    if (method == 0x01A4 || method == 0x1D6C || method == 0x1D70) {
        if (method == 0x01A4)
            s_gpu.semaphore_context = param;
        else if (method == 0x1D6C)
            s_gpu.semaphore_offset = param;
        if (!xbox_Nv2aNativeFencesEnabled())
            note_unhandled(subch, method, param);
        else if (method == 0x1D70)
            semaphore_release(param);
        return;
    }
    if (method == 0x1E60) {
        s_gpu.combiner_control = param;
        s_gpu.combiners.control = param;
        s_gpu.combiners_configured = 1;
        return;
    } else if (method == 0x1E70) {
        s_gpu.shader_stage_program = param;
        return;
    } else if (method == 0x17F8) {
        s_gpu.shader_clip_mode = param;
        return;
    } else if (method == 0x1E78) {
        s_gpu.shader_other_stage_input = param;
        return;
    } else if (method == 0x1E74) {
        s_gpu.shader_dot_mapping = param;
        return;
    } else if (method == 0x1E94) {
        s_gpu.transform_execution_mode = param;
        return;
    }
    if (method == 0x0288 || method == 0x028C) {
        s_gpu.combiners.final_input[(method - 0x0288) >> 2] = param;
        s_gpu.combiners_configured = 1;
        return;
    }
    if (method >= 0x0260 && method < 0x0280) {
        s_gpu.combiners.alpha_input[(method - 0x0260) >> 2] = param;
        return;
    }
    if (method >= 0x0A60 && method < 0x0AA0) {
        uint32_t factor = method >= 0x0A80 ? 1u : 0u;
        uint32_t stage = ((method - 0x0A60) >> 2) & 7u;
        nv_cpu_unpack_argb(param, s_gpu.combiners.factors[stage][factor]);
        return;
    }
    if (method >= 0x0AA0 && method < 0x0AC0) {
        s_gpu.combiners.alpha_output[(method - 0x0AA0) >> 2] = param;
        return;
    }
    if (method >= 0x0AC0 && method < 0x0AE0) {
        s_gpu.combiners.rgb_input[(method - 0x0AC0) >> 2] = param;
        return;
    }
    if (method >= 0x1E40 && method < 0x1E60) {
        s_gpu.combiners.rgb_output[(method - 0x1E40) >> 2] = param;
        return;
    }
    if (method == 0x1E20 || method == 0x1E24) {
        nv_cpu_unpack_argb(param, s_gpu.combiners.factors[8][(method - 0x1E20) >> 2]);
        return;
    }
    if (method == 0x02A8) {
        nv_cpu_unpack_argb((param & 0xFF00FF00u) | ((param & 255u) << 16) | ((param >> 16) & 255u),
                           s_gpu.combiners.fog_color);
        return;
    }
    if (method == 0x02B4) {
        s_gpu.window_clip_type = param & 1u; s_gpu.window_clip_valid = 1;
        return;
    }
    if (method >= 0x02C0 && method < 0x02E0) {
        s_gpu.window_clip_horizontal[(method - 0x02C0) / 4] = param;
        s_gpu.window_clip_valid = 1;
        return;
    }
    if (method >= 0x02E0 && method < 0x0300) {
        s_gpu.window_clip_vertical[(method - 0x02E0) / 4] = param;
        s_gpu.window_clip_valid = 1;
        return;
    }
    if (method == 0x037C) { s_gpu.shade_mode = param; return; }
    if (method == 0x0300) { s_gpu.alpha_enable = param; return; }
    if (method == 0x033C) { s_gpu.alpha_func = param; return; }
    if (method == 0x0340) { s_gpu.alpha_ref = param & 255u; return; }
    if (method == 0x034C) { s_gpu.blend_constant = param; return; }
    if (method == 0x0350) { s_gpu.blend_equation = param; return; }
    if (method == 0x0358) { s_gpu.color_mask = param; return; }
    if (method == 0x0214) { s_gpu.depth_offset = param; return; }
    if (method == 0x1D8C) { s_gpu.depth_clear = param; return; }
    if (method == 0x030C) { s_gpu.depth_enable = param; return; }
    if (method == 0x043C) { s_gpu.point_size = param; return; }
    if (method == 0x0354) { s_gpu.depth_func = param; return; }
    if (method == 0x035C) { s_gpu.depth_mask = param; return; }
    if (method == 0x0394) { memcpy(&s_gpu.depth_clip_min, &param, sizeof param); s_gpu.depth_range_valid |= 1u; return; }
    if (method == 0x0398) { memcpy(&s_gpu.depth_clip_max, &param, sizeof param); s_gpu.depth_range_valid |= 2u; return; }
    if (method == 0x1D78) { s_gpu.zmin_max_control = param; return; }
    if (method == 0x032C) { s_gpu.stencil_enable = param; return; }
    if (method == 0x0360) { s_gpu.stencil_write_mask = param & 255u; return; }
    if (method == 0x0364) { s_gpu.stencil_func = param; return; }
    if (method == 0x0368) { s_gpu.stencil_ref = param & 255u; return; }
    if (method == 0x036C) { s_gpu.stencil_read_mask = param & 255u; return; }
    if (method == 0x0370) { s_gpu.stencil_fail = param; return; }
    if (method == 0x0374) { s_gpu.stencil_depth_fail = param; return; }
    if (method == 0x0378) { s_gpu.stencil_pass = param; return; }
    if (method == 0x0308) { s_gpu.cull_enable = param; return; }
    if (method >= 0x0330 && method <= 0x0338) {
        uint32_t flag = 1u << ((method - 0x0330) >> 2);
        if (param) s_gpu.polygon_offset_enable |= flag;
        else s_gpu.polygon_offset_enable &= ~flag;
        return;
    }
    if (method == 0x0384) { memcpy(&s_gpu.polygon_offset_scale, &param, sizeof param); return; }
    if (method == 0x0388) { memcpy(&s_gpu.polygon_offset_bias, &param, sizeof param); return; }
    if (method == 0x038C) { s_gpu.polygon_front = param; return; }
    if (method == 0x0390) { s_gpu.polygon_back = param; return; }
    if (method == 0x039C) { s_gpu.cull_face = param; return; }
    if (method == 0x03A0) { s_gpu.front_face = param; return; }
    if (method == 0x0290) { s_gpu.control0 = param; return; }
    if (method == 0x029C) { s_gpu.fog_mode = param; return; }
    if (method == 0x02A0) { s_gpu.fog_gen_mode = param; return; }
    if (method == 0x02A4) { s_gpu.fog_enable = param; return; }
    /* Fixed-function lighting: scalar/flag registers. */
    if (method == 0x0294) { s_gpu.light_control = param; return; }
    if (method == 0x0298) { s_gpu.color_material = param; return; }
    if (method == 0x0314) { s_gpu.lighting_enable = param; return; }
    if (method == 0x0328) { s_gpu.skin_mode = param; return; }
    if (method == 0x03A4) { s_gpu.normalization_enable = param; return; }
    if (method >= 0x03C0 && method < 0x0400) {
        uint32_t word = (method - 0x03C0) >> 2;
        s_gpu.texgen[word / 4][word % 4] = param;
        return;
    }
    if (method >= 0x0420 && method < 0x0430) {
        s_gpu.texture_matrix_enable[(method - 0x0420) >> 2] = param;
        return;
    }
    if (method == 0x09CC) { s_gpu.texgen_view_model = param; return; }
    if (method >= 0x03A8 && method < 0x03B4) {
        memcpy(&s_gpu.material_emission[(method - 0x03A8) >> 2], &param, sizeof param);
        return;
    }
    if (method == 0x03B4) { memcpy(&s_gpu.material_alpha, &param, sizeof param); return; }
    if (method == 0x03B8) { s_gpu.specular_enable = param; return; }
    if (method == 0x03BC) { s_gpu.light_enable_mask = param; return; }
    if (method >= 0x09E0 && method < 0x09F8) {
        uint32_t word = (method - 0x09E0) >> 2;
        memcpy(&s_gpu.specular_params[word], &param, sizeof param);
        if (word == 5) {
            /* The other coefficient ranges still need a hardware curve reconstruction. */
            if (s_gpu.specular_params[1] > 0.0f && s_gpu.specular_params[2] < 1.0f) {
                s_gpu.specular_power = s_gpu.specular_params[2] - s_gpu.specular_params[0] * 2.0f;
            } else {
                static unsigned warned;
                s_gpu.specular_power = 1.0f;
                if (!warned++)
                    fprintf(stderr, "[GPU-D3D11] limitation: specular coefficient curve not reconstructed; exponent approximated as 1\n");
            }
        }
        return;
    }
    if (method >= 0x0A10 && method < 0x0A1C) {
        memcpy(&s_gpu.scene_ambient[(method - 0x0A10) >> 2], &param, sizeof param);
        return;
    }
    if (method >= 0x1000 && method < 0x1400) {
        /* All 8 lights' ambient/diffuse/specular/local/infinite/spot state is
         * one incrementing method block, 32 dwords (0x80 bytes) per light. */
        uint32_t slot = (method - 0x1000) >> 2;
        uint32_t light_index = slot >> 5;
        uint32_t field_word = (slot & 31u) << 2;
        float value;
        memcpy(&value, &param, sizeof value);
        if (field_word < 0x74) {
            if (field_word < 0x0C) s_gpu.light_ambient[light_index][field_word >> 2] = value;
            else if (field_word < 0x18) s_gpu.light_diffuse[light_index][(field_word - 0x0C) >> 2] = value;
            else if (field_word < 0x24) s_gpu.light_specular[light_index][(field_word - 0x18) >> 2] = value;
            else if (field_word == 0x24) s_gpu.light_local_position[light_index][3] = value; /* range */
            else if (field_word < 0x34) s_gpu.light_infinite_half_vector[light_index][(field_word - 0x28) >> 2] = value;
            else if (field_word < 0x40) s_gpu.light_infinite_direction[light_index][(field_word - 0x34) >> 2] = value;
            else if (field_word < 0x4C) { /* spot falloff, not used -- see spot direction handling below */ }
            else if (field_word < 0x5C) s_gpu.light_spot_direction[light_index][(field_word - 0x4C) >> 2] = value;
            else if (field_word < 0x68) s_gpu.light_local_position[light_index][(field_word - 0x5C) >> 2] = value;
            else if (field_word < 0x74) s_gpu.light_local_attenuation[light_index][(field_word - 0x68) >> 2] = value;
        } else note_unhandled(subch, method, param);
        return;
    }
    if (method >= 0x181C && method < 0x1828) {
        uint32_t component = (method - 0x181C) >> 2;
        memcpy(&s_gpu.shader_eye_vector[component], &param, sizeof param);
        s_gpu.shader_eye_vector_valid |= 1u << component;
        return;
    }
    if (method >= 0x0AE0 && method < 0x0AF0) {
        uint32_t stage = (method - 0x0AE0) >> 2;
        Texture *texture = stage ? &s_gpu.extra_tex[stage - 1] : &s_gpu.tex;
        texture->color_key = param;
        return;
    }
    if (method >= 0x09C0 && method < 0x09CC) {
        memcpy(&s_gpu.fog_parameters[(method - 0x09C0) >> 2], &param, sizeof param);
        return;
    }
    if (method >= 0x09D0 && method < 0x09E0) {
        memcpy(&s_gpu.fog_plane[(method - 0x09D0) >> 2], &param, sizeof param);
        return;
    }
    if (method >= 0x1B00 && method < 0x1C00 && (method & 63u) >= 0x28u) {
        uint32_t stage = (method - 0x1B00) >> 6;
        uint32_t offset = method & 63u;
        Texture *texture = stage ? &s_gpu.extra_tex[stage - 1] : &s_gpu.tex;
        if (offset <= 0x34u) memcpy(&texture->bump_matrix[(offset - 0x28u) >> 2], &param, sizeof param);
        else if (offset == 0x38u) memcpy(&texture->bump_scale, &param, sizeof param);
        else if (offset == 0x3Cu) memcpy(&texture->bump_offset, &param, sizeof param);
        else { note_unhandled(subch, method, param); return; }
        return;
    }
    if (method >= 0x1B40 && method < 0x1C00) {
        Texture *texture = &s_gpu.extra_tex[((method - 0x1B40) >> 6)];
        switch ((method - 0x1B00) & 63u) {
        case 0x00: texture->raw_offset = param; texture->offset = dma_resolve(param); break;
        case 0x04:
            texture->raw_format = param;
            texture->color = (param >> 8) & 255u;
            if (tex_size_from_format(texture->color)) {
                texture->width = 1u << ((param >> 20) & 15u);
                texture->height = 1u << ((param >> 24) & 15u);
            }
            break;
        case 0x08: texture->addr_u = param & 15u; texture->addr_v = (param >> 8) & 15u; texture->addr_w = (param >> 16) & 15u; break;
        case 0x0C: texture->control0 = param; texture->control0_valid = 1; break;
        case 0x10: texture->pitch = param >> 16; break;
        case 0x14: texture->filter = param; break;
        case 0x1C: texture->width = param >> 16; texture->height = param & 65535u; break;
        case 0x24: texture->border_color = param; break;
        default: note_unhandled(subch, method, param); return;
        }
        texture->valid = texture->offset && texture->width && texture->height &&
                         (tex_size_from_format(texture->color) || texture->pitch);
        return;
    }
    if (method == 0x1E9C) {
        s_gpu.vp_load = param;
        return;
    }
    if (method == 0x1EA0) {
        s_gpu.vp_start = param;
        return;
    }
    if (method == 0x1EA4) {
        s_gpu.constant_load = param;
        return;
    }
    if (method >= 0x1E80 && method < 0x1E90) {
        memcpy(&s_gpu.transform_data[(method - 0x1E80) >> 2], &param, 4);
        return;
    }
    if (method == 0x1E90) {
        float attributes[16][4] = {{0}};
        memcpy(attributes[0], s_gpu.transform_data, sizeof s_gpu.transform_data);
        s_gpu.state_shader_runs++;
#ifdef _WIN32
        if (!nv2a_gpu_execute_state(s_gpu.vp_program, s_gpu.vp_valid, param, attributes, s_gpu.vp_constants)) {
            s_gpu.state_shader_failures++;
            fprintf(stderr, "[GPU-D3D11] unsupported state program: start %u; hardware rendering required\n", param);
            fflush(stderr);
            _Exit(EXIT_FAILURE);
        }
#else
        Nv2aCpuVertex vertex;
        if (!nv_cpu_vertex_execute(s_gpu.vp_program, s_gpu.vp_valid, param,
                                   attributes, s_gpu.vp_constants, &vertex))
            s_gpu.state_shader_failures++;
        if (s_gpu.state_shader_runs <= 4)
            fprintf(stderr, "[GPU] state shader: start %u runs %u failures %u\n",
                    param, s_gpu.state_shader_runs, s_gpu.state_shader_failures);
    #endif
        return;
    }
    if (method >= 0x0B00 && method < 0x0B80) {
        uint32_t component = ((method - 0x0B00) >> 2) & 3u;
        if (s_gpu.vp_load < 136) {
            s_gpu.vp_program[s_gpu.vp_load][component] = param;
            s_gpu.vp_valid[s_gpu.vp_load] |= 1u << component;
        }
        s_gpu.vp_words++;
        if (component == 3)
            s_gpu.vp_load++;
        return;
    }
    if (method >= 0x0B80 && method < 0x0C00) {
        uint32_t component = ((method - 0x0B80) >> 2) & 3u;
        if (s_gpu.constant_load < 192)
            memcpy(&s_gpu.vp_constants[s_gpu.constant_load][component], &param, 4);
        if (s_gpu.constant_load < 4)
            s_gpu.composite_valid |= 1u << (s_gpu.constant_load * 4 + component);
        s_gpu.constant_words++;
        if (component == 3)
            s_gpu.constant_load++;
        return;
    }
    if (method >= 0x0A20 && method < 0x0A30) {
        uint32_t component = (method - 0x0A20) >> 2;
        memcpy(&s_gpu.viewport_offset[component], &param, 4);
        memcpy(&s_gpu.vp_constants[59][component], &param, 4);
        return;
    }
    if (method >= 0x0A50 && method < 0x0A60) {
        memcpy(&s_gpu.vp_constants[56][(method - 0x0A50) >> 2], &param, sizeof param);
        return;
    }
    if ((method >= 0x06C0 && method < 0x07C0) || (method >= 0x0840 && method < 0x0940)) {
        uint32_t base = method < 0x07C0 ? 0x06C0u : 0x0840u;
        uint32_t constant_base = method < 0x07C0 ? 68u : 64u;
        uint32_t word = (method - base) >> 2;
        uint32_t row = constant_base + (word / 16) * 8 + (word % 16) / 4;
        memcpy(&s_gpu.vp_constants[row][word % 4], &param, sizeof param);
        return;
    }
    if (method >= 0x0AF0 && method < 0x0B00) {
        uint32_t component = (method - 0x0AF0) >> 2;
        memcpy(&s_gpu.viewport_scale[component], &param, 4);
        memcpy(&s_gpu.vp_constants[58][component], &param, 4);
        return;
    }
    if (method >= 0x0440 && method < 0x06C0) {
        uint32_t entry, row;
        if (method < 0x0480) {
            entry = (method - 0x0440) >> 2;
            row = 4 + entry / 4;
        } else if (method < 0x0580) {
            entry = (method - 0x0480) >> 2;
            row = 8 + (entry / 16) * 8 + (entry % 16) / 4;
        } else if (method < 0x0680) {
            entry = (method - 0x0580) >> 2;
            row = 12 + (entry / 16) * 8 + (entry % 16) / 4;
        } else {
            entry = (method - 0x0680) >> 2;
            row = entry / 4;
        }
        memcpy(&s_gpu.vp_constants[row][entry % 4], &param, 4);
        if (row < 4) s_gpu.composite_valid |= 1u << entry;
        return;
    }
    if (method >= 0x0180 && method <= 0x01A8
            && (method & 3) == 0) {
        static unsigned context_shown;
        if (context_shown++ < 32)
            fprintf(stderr, "  [GPU] 3D context method 0x%04X handle 0x%08X\n",
                    method, param);
    }
    switch (method) {
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        s_gpu.clip_x = param & 0xFFFF;
        s_gpu.clip_w = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_CLIP_VERTICAL:
        s_gpu.clip_y = param & 0xFFFF;
        s_gpu.clip_h = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_FORMAT:
        s_gpu.format = param;
        break;
    case NV097_SET_SURFACE_PITCH:
        s_gpu.pitch = param & 0xFFFF;      /* colour pitch; zeta is the top half */
        s_gpu.depth_pitch = param >> 16;
        break;
    case NV097_SET_SURFACE_COLOR_OFFSET:
        if (getenv("RECOMP_PB_SURF_TRACE")
                && s_gpu.color_offset != param) {
            fprintf(stderr, "  [GPU] surface transition 0x%08X -> 0x%08X "
                            "after %u draws/%u clears\n",
                    s_gpu.color_offset, param, s_gpu.draws, s_gpu.clears);
            fflush(stderr);
        }
        s_gpu.color_offset = param;
        break;
    case NV097_SET_COLOR_CLEAR_VALUE:
        s_gpu.clear_color = param;
        break;
    case NV097_SET_BLEND_ENABLE:
        s_gpu.blend_enable = param;
        break;
    case NV097_SET_BLEND_FUNC_SFACTOR:
        s_gpu.blend_sfactor = param;
        break;
    case NV097_SET_BLEND_FUNC_DFACTOR:
        s_gpu.blend_dfactor = param;
        break;
    case NV097_CLEAR_SURFACE:
        clear_surface(param);
        break;

    case NV097_SET_BEGIN_END:
        if (param) {
            s_gpu.prim = param;
            s_gpu.idx_count = 0;
            s_gpu.batch_array32_count = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        } else {
            /* Three ways a batch can have arrived, and only one is in use at
             * a time: vertices completed by SET_VERTEX4F, a payload written
             * with INLINE_ARRAY, or indices into the title's own arrays. */
            if (s_gpu.imm_count)
                draw_immediate();
            else if (s_gpu.inline_count)
                draw_inline_array();
            else
                draw_primitive();
            s_gpu.prim = 0;
            s_gpu.batch_array32_count = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        }
        break;

    case NV097_INLINE_ARRAY:
        /* Vertex data, not a pointer to it. Buffered rather than decoded here
         * because the format is only fully known at END. */
        if (s_gpu.prim && s_gpu.inline_count < NV_MAX_INLINE)
            s_gpu.inline_buf[s_gpu.inline_count++] = param;
        break;

    case NV097_DRAW_ARRAYS: {
        /* A run description contributes full-width start..start+count-1
         * indices to the same topology stream as explicit array elements. */
        uint32_t start = param & 0x00FFFFFFu;
        uint32_t count = ((param >> 24) & 0xFFu) + 1u;
        uint32_t i;

        require_index_capacity(count, method);
        for (i = 0; i < count; i++)
            append_vertex_index(start + i);
        s_gpu.draw_array_indices += count;
        break;
    }

    case NV097_ARRAY_ELEMENT16:
        /* Two 16-bit indices per parameter word. */
        require_index_capacity(2, method);
        append_vertex_index(param & 0xFFFFu);
        append_vertex_index(param >> 16);
        s_gpu.array16_indices += 2;
        break;

    case NV097_ARRAY_ELEMENT32:
        require_index_capacity(1, method);
        append_vertex_index(param);
        s_gpu.array32_indices++;
        s_gpu.batch_array32_count++;
        break;
    case NV097_SET_TEXTURE_OFFSET:
        /* A texture offset is a DMA-object offset, exactly like a surface or a
         * vertex array offset -- physical, and reachable only through the
         * contiguous window when it names contiguous memory. */
        if (getenv("RECOMP_PB_SURF_TRACE")
            && s_gpu.tex.raw_offset != param) {
            fprintf(stderr, "  [GPU] texture transition 0x%08X -> 0x%08X "
                    "while surface 0x%08X after %u draws/%u clears\n",
                s_gpu.tex.raw_offset, param, s_gpu.color_offset,
                s_gpu.draws, s_gpu.clears);
            fflush(stderr);
        }
        s_gpu.tex.raw_offset = param;
        s_gpu.tex.offset = dma_resolve(param);
        record_tex_reg(method, param);
        break;

    case NV097_SET_FLIP_READ:
        s_gpu.flip_read = param;
        return;

    case NV097_SET_FLIP_WRITE:
        s_gpu.flip_write = param;
        return;

    case NV097_SET_FLIP_MODULO:
        s_gpu.flip_modulo = param;
        return;

    case NV097_FLIP_INCREMENT_WRITE:
        s_gpu.flip_write = s_gpu.flip_modulo
                         ? (s_gpu.flip_write + 1) % s_gpu.flip_modulo
                         : s_gpu.flip_write + 1;
        s_gpu.flips++;
        return;

    case NV097_FLIP_STALL:
        s_gpu.flush_flip++;
        nv2a_pb_exec_flush();
        /* The stall ends when the buffer being read is the one just finished.
         * There is no scanout here to wait for, so that is now. */
        s_gpu.flip_read = s_gpu.flip_write;
        /* And this is a completed swap, which is what a title's own swap
         * counter counts -- see xbox_Nv2aFrameCounterFlip. */
        xbox_Nv2aFrameCounterFlip();
        /* Hand the window a copy of the frame just finished.
         *
         * The buffer the title has finished is the one the last batch drew
         * into, which is what drawn_offset holds and why it exists: by the
         * flip, color_offset has already moved to the next buffer. Copying
         * here, rather than letting the window read guest memory on its own
         * clock, is what stops it showing a surface the rasteriser is still
         * writing. */
        if (s_gpu.pitch) {
            extern void xbox_FramebufferWindowPresent(uint32_t, uint32_t);
            uint32_t done = s_gpu.drawn_offset ? s_gpu.drawn_offset
                                               : s_gpu.color_offset;
            if (done) {
                present_pvideo_overlay(done);
                xbox_FramebufferWindowSet(dma_resolve(done), s_gpu.pitch);
                xbox_FramebufferWindowPresent(dma_resolve(done), s_gpu.pitch);
            }
        }
        if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
            static unsigned n;
            if (n++ < 8) {
                fprintf(stderr, "  [GPU] flip %u: read=%u write=%u\n",
                        s_gpu.flips, s_gpu.flip_read, s_gpu.flip_write);
                fflush(stderr);
            }
        }
        return;

    case NV097_SET_TEXTURE_FORMAT:
        s_gpu.tex.raw_format = param;
        s_gpu.tex.color = (param >> 8) & 0xFF;
        /* A swizzled texture carries its own dimensions here, as log2 in
         * BASE_SIZE_U/V (nv2a_regs.h: 0x00F00000 / 0x0F000000). It has to:
         * SET_TEXTURE_IMAGE_RECT describes a linear image, and a title that
         * only uses swizzled textures never sends one -- this title sends it
         * once and sets a format 3,176 times. Without this the width and
         * height stayed zero and nothing was ever sampled. */
        if (tex_size_from_format((param >> 8) & 0xFF)) {
            s_gpu.tex.width  = 1u << ((param >> 20) & 0xF);
            s_gpu.tex.height = 1u << ((param >> 24) & 0xF);
        }
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_ADDRESS:
        s_gpu.tex.addr_u =  param        & 0xF;
        s_gpu.tex.addr_v = (param >>  8) & 0xF;
        s_gpu.tex.addr_w = (param >> 16) & 0xF;
        record_tex_reg(method, param);
        break;

    case 0x1B0C:
        s_gpu.tex.control0 = param; s_gpu.tex.control0_valid = 1;
        record_tex_reg(method, param);
        break;

    case 0x1B14:
        s_gpu.tex.filter = param;
        record_tex_reg(method, param);
        break;

    case 0x1B24:
        s_gpu.tex.border_color = param;
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_CONTROL1:
        /* Pitch lives in the top half. Only meaningful for a linear format; a
         * swizzled texture has no pitch because it has no rows. */
        s_gpu.tex.pitch = param >> 16;
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_IMAGE_RECT:
        s_gpu.tex.width  = param >> 16;
        s_gpu.tex.height = param & 0xFFFF;
        record_tex_reg(method, param);
        break;

    /* Benign render state. Captured so the unhandled counter only names things
     * nobody has classified; the software rasteriser deliberately does not act
     * on any of it (point/dither/stipple/AA/occlusion-culling are all
     * optimisation or fixed-function choices D3D11 owns). */
    case NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN:
        s_gpu.transform_cxt_write_en = param;
        break;
    case NV097_SET_ANTI_ALIASING_CONTROL:
        s_gpu.anti_alias_control = param;
        break;
    case NV097_SET_OCCLUDE_ZSTENCIL_EN:
        s_gpu.occlude_zstencil_en = param;
        break;
    case NV097_SET_SWATH_WIDTH:
        s_gpu.swath_width = param;
        break;
    case NV097_SET_STIPPLE_ENABLE:
        s_gpu.stipple_enable = param;
        break;
    case NV097_SET_POINT_PARAMS_ENABLE:
        s_gpu.point_params_enable = param;
        break;
    case NV097_SET_POINT_SMOOTH_ENABLE:
        s_gpu.point_smooth_enable = param;
        break;
    case NV097_SET_DITHER_ENABLE:
        s_gpu.dither_enable = param;
        break;
    case NV097_SET_CLEAR_RECT_HORIZONTAL:
        s_gpu.clear_rect_x = param & 0xFFFF;
        s_gpu.clear_rect_w = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_CLEAR_RECT_VERTICAL:
        s_gpu.clear_rect_y = param & 0xFFFF;
        s_gpu.clear_rect_h = (param >> 16) & 0xFFFF;
        break;

    default:
        if (method >= NV_TEX_FIRST && method <= NV_TEX_LAST)
            record_tex_reg(method, param);
        if (method >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET
                && method < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + NV_VERTEX_ATTRS * 4) {
            /* Resolved here, once, so every consumer -- the rasteriser's
             * attribute reads and the diagnostics alike -- sees the same
             * address. A vertex array offset is a DMA-object offset exactly
             * like a surface offset: physical, and addressable only through
             * the window when it names contiguous memory. */
            s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4].offset =
                dma_resolve(param);
        } else if (method >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT
                && method < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + NV_VERTEX_ATTRS * 4) {
            VertexAttr *a = &s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4];
            a->type   =  param        & 0x0F;
            a->size   = (param >> 4)  & 0x0F;
            a->stride = (param >> 8)  & 0xFF;
        } else if (!imm_vertex_method(method, param)) {
            note_unhandled(subch, method, param);
        }
        break;
    }
}

/* Find where the title actually wrote its quad.
 *
 * The GPU is pointed at a buffer that stays zero, which says the data went
 * somewhere else -- and the only way to find somewhere else is to look for the
 * data. A screen-space quad for a 640x480 target contains 640.0f and 480.0f as
 * floats, which is a distinctive enough pair to search guest RAM for. Whatever
 * address that turns up is where the title's writes are landing, and the
 * difference from the programmed offset is the bug. */
static void find_quad_vertices(void)
{
    const uint32_t W = 0x44200000u;   /* 640.0f */
    const uint32_t H = 0x43F00000u;   /* 480.0f */
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for 640.0f/480.0f pairs...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 8 && hits < 12; i++) {
        if (ram[i] != W && ram[i] != H)
            continue;
        /* Both values within a few words of each other: a lone 640.0f is
         * common, the pair much less so. */
        {
            int has_w = 0, has_h = 0;
            uint32_t k;
            for (k = 0; k < 8; k++) {
                if (ram[i + k] == W) has_w = 1;
                if (ram[i + k] == H) has_h = 1;
            }
            if (!has_w || !has_h)
                continue;
        }
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 8; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 8;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none found -- the quad is not in RAM in"
                        " that form\n");
    fflush(stderr);
}

/* Locate NaN-filled transform matrices in guest RAM.
 *
 * A matrix arriving as NaN says the maths went wrong somewhere upstream, and
 * the only way to find where is to find the matrix and watch who writes it.
 * Three consecutive real-indefinite values is a distinctive enough signature:
 * ordinary data does not contain runs of 0xFFC00000. */
static void find_nan_matrices(void)
{
    const uint32_t NAN_NEG = 0xFFC00000u;
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for NaN matrices...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 20 && hits < 10; i++) {
        if (ram[i] != NAN_NEG || ram[i + 1] != NAN_NEG || ram[i + 2] != NAN_NEG)
            continue;
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 16;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none in RAM -- the NaNs are computed into"
                        " registers, not stored\n");
    fflush(stderr);
}

/* Print guest dwords named by RECOMP_PEEK, as hex and as float.
 *
 * Chasing a value backwards means reading it, and a value that is only wrong
 * for one frame in a thousand cannot be caught by stopping. Both
 * interpretations are printed because the question is usually "is this a
 * pointer or a number", and guessing wrong costs a run. */
static void peek_addresses(void)
{
    const char *spec = getenv("RECOMP_PEEK");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256];
    char *tok, *ctx = NULL;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t va = (uint32_t)strtoul(tok, NULL, 0);
        uint32_t v;
        float f;
        if (va < 0x1000u || va >= 0x04000000u)
            continue;
        v = *(const uint32_t *)(mem + va);
        memcpy(&f, &v, 4);
        fprintf(stderr, "  [PEEK] 0x%08X = %08X  (%g)\n", va, v, f);
    }
    fflush(stderr);
}

/* Walk a pointer chain and print every step.
 *
 * RECOMP_PEEK_CHAIN="0x1315A8,8,0x10,0" starts at that address, and for each
 * offset dereferences the current pointer and adds it. Following a chain by
 * hand costs one run per level; this costs one run for the whole chain, and
 * prints where it goes wrong when a level is null.
 */
static void peek_chain(void)
{
    const char *spec = getenv("RECOMP_PEEK_CHAIN");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256], *tok, *ctx = NULL;
    uint32_t cur = 0;
    int step = 0;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t off = (uint32_t)strtoul(tok, NULL, 0);
        if (step == 0) {
            cur = off;
            fprintf(stderr, "  [CHAIN] start 0x%08X\n", cur);
        } else {
            if (cur < 0x1000u || cur + 4 >= 0x04000000u) {
                fprintf(stderr, "  [CHAIN] step %d: 0x%08X is not a guest"
                                " pointer -- chain ends\n", step, cur);
                return;
            }
            cur = *(const uint32_t *)(mem + cur) + off;
            fprintf(stderr, "  [CHAIN] step %d: deref +0x%X -> 0x%08X\n",
                    step, off, cur);
        }
        step++;
    }
    if (cur >= 0x1000u && cur + 4 < 0x04000000u)
        fprintf(stderr, "  [CHAIN] final value at 0x%08X = 0x%08X\n",
                cur, *(const uint32_t *)(mem + cur));
    fflush(stderr);
}

void nv2a_pb_exec_report(void)
{
    s_gpu.flush_report++;
    nv2a_pb_exec_flush();
    fprintf(stderr, "[GPU-D3D11] batches: %u hardware, %u CPU fallback\n", s_gpu.gpu_batches, s_gpu.cpu_batches);
    fprintf(stderr, "[GPU-D3D11] index stream: %llu u16, %llu u32, %llu array-run indices; %llu above 65535; %llu native u32 batches\n",
            (unsigned long long)s_gpu.array16_indices, (unsigned long long)s_gpu.array32_indices,
            (unsigned long long)s_gpu.draw_array_indices, (unsigned long long)s_gpu.wide_indices,
            (unsigned long long)s_gpu.native_array32_batches);
    fprintf(stderr, "[GPU-D3D11] index storage: %u peak batch indices, %u allocated capacity\n",
            s_gpu.max_index_count, s_gpu.idx_capacity);
    fprintf(stderr, "[GPU-D3D11] vertex preparation: %llu program attribute fetches, %llu unused skipped; %.3f GiB zeroing avoided\n",
            (unsigned long long)s_gpu.gpu_attribute_fetches,
            (unsigned long long)s_gpu.gpu_attribute_skips,
            (double)s_gpu.gpu_zero_bytes_saved / (1024.0 * 1024 * 1024));
    fprintf(stderr, "[GPU-D3D11] executor time: preparation %.3fs backend %.3fs\n",
            s_gpu.gpu_prepare_seconds, s_gpu.gpu_submit_seconds);
    nv2a_gpu_report();
    xbox_Nv2aSoftwareMethodReport();
#ifdef _WIN32
    static clock_t previous_time;
    static uint32_t previous_flips;
    static double previous_raster_seconds;
    clock_t current_time = clock();
    if (current_time > previous_time)
        fprintf(stderr, "[GPU] presentation: %u flips, %.2f FPS over %.2f seconds\n", s_gpu.flips,
                (double)(s_gpu.flips - previous_flips) * CLOCKS_PER_SEC / (current_time - previous_time),
                (double)(current_time - previous_time) / CLOCKS_PER_SEC);
    if (current_time > previous_time)
        fprintf(stderr, "[GPU] draw time: %.3f seconds, %.1f%% of interval\n",
                s_gpu.raster_seconds - previous_raster_seconds,
                100.0 * (s_gpu.raster_seconds - previous_raster_seconds) * CLOCKS_PER_SEC /
                    (current_time - previous_time));
    previous_time = current_time;
    previous_flips = s_gpu.flips;
    previous_raster_seconds = s_gpu.raster_seconds;
#endif
    peek_addresses();
    peek_chain();
    if (getenv("RECOMP_FIND_NAN")) {
        /* Every report, not once: the matrix is fine early on and only turns
         * to NaN later, so a single scan at startup finds nothing and says
         * nothing. */
        find_nan_matrices();
    }
    if (getenv("RECOMP_FIND_QUAD")) {
        static int done;
        if (!done) { done = 1; find_quad_vertices(); }
    }
    int i, j;

    fprintf(stderr, "[GPU] flushes: flip %u, wait_idle %u, fence %u, "
                    "semaphore %u, clear %u, raster %u, report %u\n",
            s_gpu.flush_flip, s_gpu.flush_wait_idle, s_gpu.flush_fence,
            s_gpu.flush_semaphore, s_gpu.flush_clear, s_gpu.flush_raster,
            s_gpu.flush_report);
    fprintf(stderr, "[GPU] surface 0x%08X pitch %u clip %ux%u+%u+%u"
                    " clears %u | %u unhandled methods (%d distinct)\n",
            s_gpu.color_offset, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.clears,
            s_gpu.unhandled_total, s_unhandled_count);
    fprintf(stderr, "[GPU] draws %u (%u with coordinates), %u indices;"
                    " x %.1f..%.1f  y %.1f..%.1f\n",
            s_gpu.draws, s_gpu.nonzero_draws, s_gpu.verts,
            s_gpu.min_x, s_gpu.max_x, s_gpu.min_y, s_gpu.max_y);
    /* Drawn and skipped separately: "nothing appeared" and "every batch needed
     * a vertex program we do not run" look identical on screen, and only one
     * of them means the rasteriser is broken. */
    fprintf(stderr, "[GPU] brightest pixel written 0x%08X\n", s_gpu.pixel_max);
    fprintf(stderr, "[GPU] %llu pixels written; draw surface 0x%08X"
                    " -> 0x%08X, clear surface 0x%08X -> 0x%08X\n",
            (unsigned long long)s_gpu.pixels, s_gpu.drawn_offset,
            dma_resolve(s_gpu.drawn_offset), s_gpu.color_offset,
            dma_resolve(s_gpu.color_offset));
    fprintf(stderr, "[GPU] rasterised %u triangles; %u batches skipped as not"
                    " screen-space, %u triangles fully off-surface\n",
            s_gpu.tris_drawn, s_gpu.batches_untransformed,
            s_gpu.tris_skipped_offscreen);
    fprintf(stderr, "[GPU] shader state: combiner=0x%08X"
                    " stages=0x%08X transform=0x%08X\n",
            s_gpu.combiner_control, s_gpu.shader_stage_program,
            s_gpu.transform_execution_mode);
    fprintf(stderr, "[GPU] shader execution: %u vertices, %u pixels, %u failures\n",
            s_gpu.shader_vertices, s_gpu.shader_pixels, s_gpu.shader_failures);
        fprintf(stderr, "[GPU] depth: %u writes, %u rejected; enable %u func %X mask %u format %X control %X\n",
            s_gpu.depth_written, s_gpu.depth_rejected, s_gpu.depth_enable, s_gpu.depth_func,
            s_gpu.depth_mask, (s_gpu.format >> 4) & 15u, s_gpu.control0);
        fprintf(stderr, "[GPU] combiner words: rgb %08X -> %08X alpha %08X -> %08X final %08X %08X fast %d\n",
            s_gpu.combiners.rgb_input[0], s_gpu.combiners.rgb_output[0],
            s_gpu.combiners.alpha_input[0], s_gpu.combiners.alpha_output[0],
            s_gpu.combiners.final_input[0], s_gpu.combiners.final_input[1],
            nv_cpu_combiner_fast_mode(&s_gpu.combiners));
            fprintf(stderr, "[GPU] vertex uploads: %u program words, %u constant words; start %u"
                    " viewport %.3f,%.3f,%.3f,%.3f + %.3f,%.3f,%.3f,%.3f\n",
                s_gpu.vp_words, s_gpu.constant_words, s_gpu.vp_start,
                s_gpu.viewport_scale[0], s_gpu.viewport_scale[1], s_gpu.viewport_scale[2], s_gpu.viewport_scale[3],
                s_gpu.viewport_offset[0], s_gpu.viewport_offset[1], s_gpu.viewport_offset[2], s_gpu.viewport_offset[3]);
            if (s_gpu.vp_start < 136) {
            const uint32_t *instruction = s_gpu.vp_program[s_gpu.vp_start];
            fprintf(stderr, "[GPU] first vertex instruction: %08X %08X %08X %08X (valid %X)\n",
                instruction[0], instruction[1], instruction[2], instruction[3],
                s_gpu.vp_valid[s_gpu.vp_start]);
            }

    /* And of the batches that did rasterise, how many sampled anything. A menu
     * that draws its background from one texture and its text from another
     * shows both as flat colour if either half is missing, so the split is
     * what says which half. */
    fprintf(stderr, "[GPU] batches: %u textured, %u with no texcoords,"
                    " %u with texcoords but no usable stage\n",
            s_gpu.batches_textured, s_gpu.batches_no_uv, s_gpu.batches_no_tex);
    for (i = 0; i < s_tex_use_count; i++)
        fprintf(stderr, "  [TEXUSE] 0x%08X %ux%u fmt 0x%02X%s: %u batches\n",
                s_tex_use[i].offset, s_tex_use[i].width, s_tex_use[i].height,
                s_tex_use[i].color,
                d3d8_format_dxt_block_bytes(s_tex_use[i].color) ? " dxt"
                    : d3d8_format_is_swizzled(s_tex_use[i].color) ? " swz" : " lin",
                s_tex_use[i].batches);

    if (getenv("RECOMP_TEX_STATE")) {
        uint32_t k;
        for (k = 0; k < sizeof s_tex_set / sizeof s_tex_set[0]; k++)
            if (s_tex_set[k])
                fprintf(stderr, "  [TEX] 0x%04X = 0x%08X\n",
                        (unsigned)(NV_TEX_FIRST + k * 4), s_tex_reg[k]);
    }

    /* Top ten by frequency: selection sort over a small table, once every few
     * seconds, is not worth a better algorithm.
     *
     * RECOMP_PB_UNHANDLED_ALL lists every one instead. Ten is the right
     * default -- the tail is a long list of state registers nobody needs to
     * read -- but when a title stops and the question is which method it
     * stopped on, the answer is as likely to be the one seen twice as the
     * one seen a thousand times, and ten hides it. */
    {
        int shown = getenv("RECOMP_PB_UNHANDLED_ALL") ? s_unhandled_count : 10;
    for (i = 0; i < shown && i < s_unhandled_count; i++) {
        int best = i;
        for (j = i + 1; j < s_unhandled_count; j++)
            if (s_unhandled[j].count > s_unhandled[best].count)
                best = j;
        if (best != i) {
            PbUnhandled t = s_unhandled[i];
            s_unhandled[i] = s_unhandled[best];
            s_unhandled[best] = t;
        }
        {
            /* The value as well as the count. A method nobody decoded is
             * a guess until you see what it carried: screen coordinates,
             * a 0..1 texcoord and a packed colour are told apart at a
             * glance, and that is what says which vertex encoding a title
             * is using. */
            union { uint32_t u; float f; } v;
            v.u = s_unhandled[i].last_param;
            fprintf(stderr, "  [GPU]   0x%04X x%-8u last=0x%08X (%.4f) subch=%u\n",
                    s_unhandled[i].method, s_unhandled[i].count,
                    v.u, v.f, s_unhandled[i].subchannel);
        }
    }
    }    fflush(stderr);
}
