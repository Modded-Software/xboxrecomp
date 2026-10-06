/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "xbox_memory_layout.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096
#define PB_HISTORY_SIZE 32

static struct {
    uint32_t va, word, remaining, method;
} s_history[PB_HISTORY_SIZE];
static uint32_t s_history_next, s_history_count;
static int s_failure_trace = -1;
static struct {
    uint32_t va, word, target;
} s_control_history[16];
static uint32_t s_control_next, s_control_count;

static void note_control(uint32_t va, uint32_t word, uint32_t target)
{
    if (!s_failure_trace)
        return;
    s_control_history[s_control_next].va = va;
    s_control_history[s_control_next].word = word;
    s_control_history[s_control_next].target = target;
    s_control_next = (s_control_next + 1) % 16;
    if (s_control_count < 16)
        s_control_count++;
}

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;
static uint16_t s_seen_slots[8][2048];

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_flush(void);
extern void nv2a_pb_exec_report(void);
static int s_exec_enabled = -1;
static int s_bulk_indices = -1;
static uint64_t s_bulk_index_packets, s_bulk_index_words;
extern void nv2a_pb_exec_indices(uint32_t method, const uint32_t *parameters, uint32_t count);

static void note(uint32_t subch, uint32_t method, uint32_t count)
{
    uint16_t *slot = subch < 8 && method <= 0x1FFCu && !(method & 3u) ? &s_seen_slots[subch][method >> 2] : NULL;
    if (slot) {
        if (*slot && *slot <= s_seen_count && s_seen[*slot - 1].method == method && s_seen[*slot - 1].subch == subch) {
            s_seen[*slot - 1].count += count;
            return;
        }
    } else {
        for (int index = 0; index < s_seen_count; index++) {
            if (s_seen[index].method == method && s_seen[index].subch == subch) {
                s_seen[index].count += count;
                return;
            }
        }
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = count;
        s_seen_count++;
        if (slot) *slot = (uint16_t)s_seen_count;
    }
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1808 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0130, "SET_FLIP_READ" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_CLEAR_DEPTH" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_ZSTENCIL_CLEAR" },
    { 0x0B80, "SET_TRANSFORM_PROGRAM" },
    { 0x0B00, "SET_TRANSFORM_CONSTANT" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "INLINE_ARRAY" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B14, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0) {
        nv2a_pb_exec_report();
        fprintf(stderr, "[PB] bulk indices: %llu packets, %llu parameter words; %llu per-word GET updates avoided\n",
                (unsigned long long)s_bulk_index_packets, (unsigned long long)s_bulk_index_words,
                (unsigned long long)(s_bulk_index_words - s_bulk_index_packets));
    }
    if (!s_seen_count || !getenv("RECOMP_PB_SCAN"))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u unrecognised"
                    " -- %d distinct (subchannel, method) pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_unknown,
            s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

static void failure_context(uint32_t start_va, uint32_t end_va, uint32_t va,
                            uint32_t words, uint32_t remaining,
                            uint32_t method, uint32_t return_va)
{
    fprintf(stderr, "[PB] segment 0x%08X -> 0x%08X, current 0x%08X, words %u, remaining %u, method 0x%04X, return 0x%08X\n",
            start_va, end_va, va, words, remaining, method, return_va);
    if (s_failure_trace) {
        volatile uint32_t *dma = xbox_Nv2aRegisterPointer(0x800040, 8);
        if (dma)
            fprintf(stderr, "[PB] live DMA PUT 0x%08X GET 0x%08X\n", dma[0], dma[1]);
        uint32_t first = (s_history_next + PB_HISTORY_SIZE - s_history_count) % PB_HISTORY_SIZE;
        for (uint32_t i = 0; i < s_history_count; i++) {
            uint32_t index = (first + i) % PB_HISTORY_SIZE;
            fprintf(stderr, "[PB] recent 0x%08X: 0x%08X, remaining %u, method 0x%04X\n",
                    s_history[index].va, s_history[index].word,
                    s_history[index].remaining, s_history[index].method);
        }
        first = (s_control_next + 16 - s_control_count) % 16;
        for (uint32_t i = 0; i < s_control_count; i++) {
            uint32_t index = (first + i) % 16;
            fprintf(stderr, "[PB] control 0x%08X: 0x%08X -> 0x%08X\n",
                    s_control_history[index].va, s_control_history[index].word,
                    s_control_history[index].target);
            uint32_t source = s_control_history[index].va;
            uint32_t target = s_control_history[index].target;
            if (source >= XBOX_CONTIG_BASE + 256 &&
                    source < XBOX_CONTIG_BASE + 0x200000 &&
                    target >= XBOX_CONTIG_BASE + 0x200000) {
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                fprintf(stderr, "[PB] invoke prefix for 0x%08X\n", source);
                for (uint32_t at = source - 256; at <= source; at += 32) {
                    const uint32_t *p = (const uint32_t *)(mem + at);
                    fprintf(stderr, "[PB] prefix 0x%08X: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                            at, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
                }
            }
        }
        nv2a_pb_scan_report();
        nv2a_pb_exec_report();
    }
}

void nv2a_pb_scan(uint32_t start_va, uint32_t end_va)
{
    static uint32_t pending_count, pending_method, pending_subch, pending_next;
    static uint32_t return_va;
    static int pending_noninc;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t address_base = start_va >= XBOX_CONTIG_BASE ? XBOX_CONTIG_BASE : 0;
    uint32_t va = start_va;
    uint32_t words = 0, jumps = 0, unknown = 0;
    volatile uint32_t *dma_get = NULL;

    if (s_exec_enabled < 0)
        s_exec_enabled = 1;
    if (s_failure_trace < 0)
        s_failure_trace = getenv("RECOMP_PB_FAILURE_TRACE") != NULL;
    if (s_bulk_indices < 0) {
        const char *value = getenv("RECOMP_PB_BULK_INDICES");
        s_bulk_indices = (!value || strcmp(value, "0") != 0) &&
                         !s_failure_trace && !getenv("RECOMP_PB_EXEC_VERBOSE");
    }
    if (end_va == start_va)
        return;
    if (s_exec_enabled) {
        dma_get = xbox_Nv2aRegisterPointer(0x800044, 4);
        if (!dma_get) {
            fflush(stderr); _Exit(EXIT_FAILURE);
        }
    }
    if (pending_count && start_va != pending_next) {
        fprintf(stderr, "[PB] discontinuous method packet: expected 0x%08X, received 0x%08X, %u parameters remain\n",
                pending_next, start_va, pending_count);
        failure_context(start_va, end_va, va, words, pending_count, pending_method, return_va);
        fflush(stderr);
        _Exit(EXIT_FAILURE);
    }
    while (va != end_va && words < 0x100000u) {
        if ((va & 3u) || va < address_base || (uint64_t)va + 4 > (uint64_t)address_base + XBOX_CONTIG_SIZE) {
            fprintf(stderr, "[PB] invalid command address 0x%08X\n", va);
            failure_context(start_va, end_va, va, words, pending_count, pending_method, return_va);
            fflush(stderr); _Exit(EXIT_FAILURE);
        }
        if (s_bulk_indices && s_exec_enabled && pending_count && pending_noninc &&
            pending_subch == 0 && (pending_method == 0x1800u || pending_method == 0x1808u) &&
            va < end_va && !(end_va & 3u)) {
            uint32_t count = pending_count;
            uint32_t available = (end_va - va) / 4;
            uint64_t mapped = ((uint64_t)address_base + XBOX_CONTIG_SIZE - va) / 4;
            if (count > available) count = available;
            if (count > mapped) count = (uint32_t)mapped;
            if (count > 0x100000u - words) count = 0x100000u - words;
            if (count) {
                /* Indices have no guest-visible side effects; retain GET until all source words are copied. */
                nv2a_pb_exec_indices(pending_method, (const uint32_t *)(mem + va), count);
                note(pending_subch, pending_method, count);
                va += count * 4;
                words += count;
                pending_count -= count;
                pending_next = va;
                *dma_get = va & 0x0FFFFFFFu;
                s_bulk_index_packets++;
                s_bulk_index_words += count;
                continue;
            }
        }
        uint32_t w = *(const uint32_t *)(mem + va);
        if (s_failure_trace) {
            s_history[s_history_next].va = va;
            s_history[s_history_next].word = w;
            s_history[s_history_next].remaining = pending_count;
            s_history[s_history_next].method = pending_count ? pending_method : 0;
            s_history_next = (s_history_next + 1) % PB_HISTORY_SIZE;
            if (s_history_count < PB_HISTORY_SIZE)
                s_history_count++;
        }
        va += 4;
        words++;

        if (pending_count) {
            note(pending_subch, pending_method, 1);
            if (s_exec_enabled) {
                /* Publish the fetched parameter before its semaphore or
                 * callback can wake a CPU rewriting later command words. */
                *dma_get = va & 0x0FFFFFFFu;
                nv2a_pb_exec_method(pending_subch, pending_method, w);
            }
            if (!pending_noninc) pending_method += 4;
            pending_count--;
            pending_next = va;
            continue;
        }
        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            jumps++;
            uint32_t target = address_base | ((w & 3u) == 1u ? w & 0xFFFFFFFCu : w & 0x1FFFFFFCu);
            note_control(va - 4, w, target);
            va = target;
            if (dma_get) *dma_get = va & 0x0FFFFFFFu;
            continue;
        }
        if ((w & 3u) == 2u) {
            if (return_va) {
                fprintf(stderr, "[PB] nested DMA subroutine at 0x%08X\n", va - 4);
                failure_context(start_va, end_va, va, words, pending_count, pending_method, return_va);
                fflush(stderr); _Exit(EXIT_FAILURE);
            }
            return_va = va;
            uint32_t target = address_base | (w & 0xFFFFFFFCu);
            note_control(va - 4, w, target);
            va = target;
            if (dma_get) *dma_get = va & 0x0FFFFFFFu;
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {
            if (!return_va) {
                fprintf(stderr, "[PB] DMA return without call at 0x%08X\n", va - 4);
                failure_context(start_va, end_va, va, words, pending_count, pending_method, return_va);
                fflush(stderr); _Exit(EXIT_FAILURE);
            }
            note_control(va - 4, w, return_va);
            va = return_va;
            return_va = 0;
            if (dma_get) *dma_get = va & 0x0FFFFFFFu;
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            pending_count = (w >> 18) & 0x7FFu;
            pending_subch = (w >> 13) & 7u;
            pending_method = w & 0x1FFCu;
            pending_noninc = (w & 0xE0000000u) == 0x40000000u;
            pending_next = va;
            if (dma_get) *dma_get = va & 0x0FFFFFFFu;
            continue;
        }
        unknown++;
        if (dma_get) *dma_get = va & 0x0FFFFFFFu;
    }

    if (va != end_va) {
        fprintf(stderr, "[PB] command walk failed to reach PUT 0x%08X from 0x%08X (%u jumps, %u unknown words)\n",
                end_va, start_va, jumps, unknown);
        failure_context(start_va, end_va, va, words, pending_count, pending_method, return_va);
        fflush(stderr); _Exit(EXIT_FAILURE);
    }
    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
    if (s_exec_enabled && !xbox_Nv2aNativeFencesEnabled()) nv2a_pb_exec_flush();
}
