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

#ifndef XBOX_CONTIG_BASE
#define XBOX_CONTIG_BASE 0x80000000u   /* physical P is at this + P */
#endif
#ifndef XBOX_CONTIG_SIZE
#define XBOX_CONTIG_SIZE (64u * 1024u * 1024u)
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;
/* Direct index into s_seen: note() runs once per pushbuffer method word on the
 * executor thread, so a linear scan of up to PB_MAX_METHODS was a real cost on
 * every parameter word. Method is (w & 0x1FFC) -> 2048 slots, subchannel 3
 * bits -> 8, so an [8][2048] table (int16 slots, 32 KB) makes it O(1) while
 * leaving s_seen and the report untouched. */
static int16_t s_seen_index[8][2048];
static int s_seen_index_ready;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;
static uint32_t s_tot_calls;
/* Target of the last ring JUMP (not a CALL's) seen by pb_walk: the true start
 * of the pushbuffer ring. xbox_memory_layout uses it to find the seam on a
 * wrap instead of guessing the bounds from the PUT range. */
static uint32_t s_last_jump;
/* Address of the parameter word most recently handed to the executor: lets a
 * handler that rejects an out-of-batch command dump the surrounding stream. */
uint32_t g_pb_current_va;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
static int s_exec_enabled = -1;

static void note(uint32_t subch, uint32_t method)
{
    uint32_t sub, m;
    int16_t slot;
    if (!s_seen_index_ready) {
        memset(s_seen_index, 0xFF, sizeof s_seen_index);
        s_seen_index_ready = 1;
    }
    sub = subch & 7u; m = (method >> 2) & 0x7FFu;
    slot = s_seen_index[sub][m];
    if (slot >= 0) {
        s_seen[slot].count++;
        return;
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
        return;
    }
    s_seen_index[sub][m] = (int16_t)s_seen_count;
    s_seen[s_seen_count].method = method;
    s_seen[s_seen_count].subch  = subch;
    s_seen[s_seen_count].count  = 1;
    s_seen_count++;
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
    { 0x1808, "ARRAY_ELEMENT32" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1818, "INLINE_ARRAY" },
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
        fprintf(stderr, "[PB] %u pushbuffer calls followed\n", s_tot_calls);
    }
    if (!s_seen_count || !getenv("RECOMP_PB_SCAN"))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u calls,"
                    " %u unrecognised -- %d distinct (subchannel, method)"
                    " pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_calls,
            s_tot_unknown, s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

/* Walk commands from va to end_va. Returns the words consumed.
 *
 * A CALL runs a subroutine -- a pushbuffer somewhere else in memory -- up to
 * its RETURN, then carries on after the CALL. This used to skip CALLs, which
 * was invisible until a title used them for real: RenderWare on the Xbox
 * compiles static geometry into pushbuffers and draws it with a CALL, so the
 * whole race track went missing from Burnout 3's main view while the
 * geometry it submits inline (environment maps, the HUD) drew fine.
 * The NV2A has one level of subroutine, so a CALL inside a call is not
 * followed. Addresses are physical, like PUT's, and reach guest memory the
 * same way: through the contiguous window. */
static uint32_t pb_walk(uint32_t va, uint32_t end_va, int in_call,
                        uint32_t *jumps, uint32_t *unknown)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t words = 0;

    while (va < end_va && words < 0x100000u) {
        uint32_t w = *(const uint32_t *)(mem + va);
        va += 4;
        words++;

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            uint32_t target = (w & 3u) == 1u ? (w & 0xFFFFFFFCu)
                                             : (w & 0x1FFFFFFCu);
            uint32_t tgt = XBOX_CONTIG_BASE | (target & 0x0FFFFFFFu);
            (*jumps)++;
            if (!in_call) {
                /* Record only a backward jump as the ring wrap; a forward
                 * jump is a mid-ring skip and must not be mistaken for the
                 * end of the lap (it would poison the wrap handler). */
                if (tgt <= va)
                    s_last_jump = tgt;
                break;                        /* the ring: a jump ends it */
            }
            va = tgt;
            end_va = va + 0x400000u;
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {  /* RETURN */
            if (in_call)
                break;
            continue;
        }
        if ((w & 3u) == 2u) {                    /* CALL */
            if (!in_call) {
                uint32_t sub = XBOX_CONTIG_BASE | ((w & 0xFFFFFFFCu) & 0x0FFFFFFFu);
                s_tot_calls++;
                words += pb_walk(sub, sub + 0x400000u, 1, jumps, unknown);
            }
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            for (uint32_t i = 0; i < count && va < end_va; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                note(subch, m);
                /* Same walk, two consumers: the survey counts, the executor
                 * acts. Keeping them on one decode means they can never
                 * disagree about what the stream said. */
                if (s_exec_enabled) {
                    g_pb_current_va = va;
                    nv2a_pb_exec_method(subch, m,
                                        *(const uint32_t *)(mem + va));
                }
                va += 4;
                words++;
            }
            continue;
        }
        { static unsigned n; if (n++ < 32)
            fprintf(stderr, "[PB-UNK] va %08X word %08X (end %08X)\n", (unsigned)(va - 4u), w, end_va); }
        (*unknown)++;
    }
    return words;
}

void nv2a_pb_scan(uint32_t start_va, uint32_t end_va)
{
    uint32_t words, jumps = 0, unknown = 0;

    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    static int scan = -1;
    if (scan < 0)
        scan = getenv("RECOMP_PB_SCAN") != NULL;
    if (!(scan || s_exec_enabled) || end_va <= start_va)
        return;
    if (end_va - start_va > 0x400000u)        /* a sane single-frame bound */
        end_va = start_va + 0x400000u;

    s_last_jump = 0;
    words = pb_walk(start_va, end_va, 0, &jumps, &unknown);

    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}

uint32_t nv2a_pb_last_jump(void)
{
    return s_last_jump;
}

/* Walk from the GPU's GET to the title's PUT the way the hardware does:
 * follow every JUMP (into or out of the ring) and CALL (one return level),
 * RETURN restores the saved PC, and stop exactly when GET reaches PUT.
 *
 * `s_get` persists across kicks so a jump into a secondary command buffer
 * (D3D8 submits the real BEGIN/vertex-array/indices there) and back is
 * stitched correctly instead of walking a fixed address range. The ring wrap
 * is just another jump, so the PUT-bracket guesswork and s_last_jump are not
 * needed here. PUT only advances on command boundaries, so a packet is never
 * split and the method loop may consume all of its parameters. */
void nv2a_pb_run(uint32_t put_va)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    static uint32_t s_get;
    /* The CALL return PC persists across kicks: on hardware it lives in
     * DMA_SUBROUTINE (0x324C), and D3D can split a submission so that a kick
     * ends inside a called buffer and the matching RETURN arrives on the next
     * one. A local here lost it, and the RETURN then went nowhere (or worse,
     * took the stale s_get). */
    static uint32_t ret;
    uint32_t va;
    uint32_t budget = 0x200000u;
    const uint32_t win_lo = XBOX_CONTIG_BASE;
    const uint32_t win_hi = XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE;   /* exclusive */

    /* The first kick only establishes where the ring starts; the command
     * there is executed on the next kick, exactly as the old
     * scan(last_put, put) did (it set last_put on the first change). Without
     * this the ring's first submission -- the GPU bootstrap -- was skipped
     * and the guest hung waiting for it. */
    if (!s_get) {
        s_get = put_va;
        return;
    }
    va = s_get;

    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;

    /* Any walk that leaves the 64 MB contiguous window means we lost sync with
     * PUT: the stream was misparsed, a CALL/RETURN was unbalanced, or the ring
     * wrapped unannounced. Every read and every jump/call target is validated
     * against the window; the first violation aborts the walk and resyncs to
     * PUT, so a desync degrades to one dropped kick instead of a crash. */
    int desync = 0;
    const char *why = "?";

    /* PFIFO DMA_STATE. The hardware keeps the in-flight method packet across
     * kicks, so a packet whose payload crosses PUT (a GET->PUT kick boundary
     * landing in the middle of a header's parameters) is resumed on the next
     * kick. Dropping it -- the old `if (va == put_va) break;` -- left the
     * walker to re-read a payload word as a header, which executed garbage and
     * was the source of the `index command rejected (prim 0)` storm. */
    static struct { uint32_t method, subch, count; int noninc; } st;

    /* Keep the last top-level words the walk read, so the first time it lands
     * on an undecodable word (or a jump/call out of the window) we can show
     * what packets led there. A misparse -- one word read as a method/jump
     * header -- is otherwise invisible until it has already run off the end. */
    static struct { uint32_t va, w; } recent[64];
    static unsigned recent_n;
    static struct { uint32_t va, w; } kickwords[16384];
    static unsigned kick_n2;
    kick_n2 = 0;
    /* Last 16 JUMP/CALLs taken this kick (diagnostic for walks that loop). */
    struct { uint32_t from, w; } jtrace[16];
    unsigned jtrace_n = 0;

    while (va != put_va && budget && !desync) {
        if (va < win_lo || va + 4u > win_hi || (va & 3u)) {
            why = "read out of contiguous window";
            desync = 1;
            break;
        }
        uint32_t w = *(const uint32_t *)(mem + va);
        unsigned slot = recent_n++ & 63u;
        recent[slot].va = va;
        recent[slot].w = w;
        if (kick_n2 < 16384) { kickwords[kick_n2].va = va; kickwords[kick_n2].w = w; kick_n2++; }
        va += 4;
        budget--;

        if (st.count) {                                             /* payload */
            note(st.subch, st.method);
            if (s_exec_enabled) {
                g_pb_current_va = va - 4u;
                nv2a_pb_exec_method(st.subch, st.method, w);
            }
            if (!st.noninc) st.method += 4;
            st.count--;
            continue;
        }

        uint32_t off;
        if ((w & 0xE0000003u) == 0x20000000u) {                     /* old JUMP */
            off = w & 0x1FFFFFFCu;
        } else if ((w & 3u) == 1u) {                                /* JUMP */
            off = w & 0x0FFFFFFCu;
        } else if ((w & 3u) == 2u) {                                /* CALL */
            ret = va;                                               /* after CALL */
            off = w & 0x0FFFFFFCu;
        } else if (w == 0x00020000u) {                              /* RETURN */
            /* A RETURN with no pending CALL is a no-op; taking it to address 0
             * walked the fake TIB and executed garbage, hanging at boot. */
            if (ret) { va = ret; ret = 0; }
            continue;
        } else if ((w & 0xE0030003u) == 0u
                || (w & 0xE0030003u) == 0x40000000u) {              /* method */
            /* The method, subchannel, remaining count and non-increment bit
             * were what the old loop held in locals and dropped at PUT. Count
             * 0 (including a 0x00000000 word) is a no-op. */
            st.method = w & 0x1FFCu;
            st.subch  = (w >> 13) & 7u;
            st.count  = (w >> 18) & 0x7FFu;
            st.noninc = (w >> 30) & 1u;
            continue;
        } else {
            why = "reserved command";
            desync = 1;
            break;
        }
        off &= 0x0FFFFFFFu;
        if (off + 4u > XBOX_CONTIG_SIZE) {
            why = "jump/call target out of window";
            desync = 1;
            break;
        }
        jtrace[jtrace_n & 15u].from = va - 4u;
        jtrace[jtrace_n & 15u].w = w;
        jtrace_n++;
        va = win_lo | off;
    }
    if (desync) {
        static unsigned n;
        if (n < 8) {
            fprintf(stderr, "  [PB] desync at %08X (get %08X put %08X ret %08X): %s; resync to PUT\n",
                    va, s_get, put_va, ret, why);
            if (n == 0) {
                fprintf(stderr, "  [PB] FULL KICK words %u:\n", kick_n2);
                for (unsigned k = 0; k < kick_n2; k++) {
                    uint32_t rw = kickwords[k].w;
                    const char *kind = (rw & 3u) == 2u ? "CALL" :
                                       ((rw & 3u) == 1u || (rw & 0xE0000003u) == 0x20000000u) ? "JUMP" :
                                       rw == 0x00020000u ? "RET " :
                                       (rw & 0xE0030003u) == 0u ? "METH" : "    ";
                    fprintf(stderr, "    K %08X %08X %s\n", kickwords[k].va, rw, kind);
                }
            }
            for (int k = 63; k >= 0; k--) {
                unsigned idx = (recent_n - 1u - (unsigned)k) & 63u;
                uint32_t rw = recent[idx].w;
                const char *kind = (rw & 3u) == 2u ? "CALL" :
                                   ((rw & 3u) == 1u || (rw & 0xE0000003u) == 0x20000000u) ? "JUMP" :
                                   rw == 0x00020000u ? "RET " :
                                   (rw & 0xE0030003u) == 0u ? "METH" : "    ";
                fprintf(stderr, "    %08X %08X %s\n", recent[idx].va, rw, kind);
            }
        }
        n++;
    } else if (!budget) {
        static unsigned nb;
        fprintf(stderr, "  [PB] walk budget exhausted at %08X, resync to PUT %08X\n", va, put_va);
        if (nb++ < 4) {
            fprintf(stderr, "  [PB] %u jumps/calls taken; last ones (from word):\n", jtrace_n);
            for (unsigned k = jtrace_n > 16u ? jtrace_n - 16u : 0u; k < jtrace_n; k++)
                fprintf(stderr, "    J %08X %08X\n", jtrace[k & 15u].from, jtrace[k & 15u].w);
        }
    }
    /* A kick that did not stop cleanly at PUT leaves no packet to resume. */
    if (desync || !budget) {
        st.count = 0;
        ret = 0;
    }
    s_get = put_va;
}
