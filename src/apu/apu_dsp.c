/*
 * MCPX APU DSP (GP/EP)
 *
 * Two implementations live here:
 *
 *  - The real GP/EP DSP56300 pipeline, ported verbatim from xemu under
 *    dsp/. VP mixbins are written into the GP mix buffer, the GP and EP
 *    programs run, and the EP output fifo is sunk into the monitor frame
 *    buffer. Selected with RECOMP_APU_DSP=1.
 *
 *  - A passthrough stub (default). VP mixbins are mixed straight down to
 *    the monitor buffer. DirectSound command doorbells are acknowledged by
 *    RECOMP_APU_DSP_ACK rather than by a running DSP program.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "apu.h"
#include "fpconv.h"

#include "dsp/dsp.h"
#include "dsp/dsp_dma.h"
#include "dsp/dsp_state.h"
#include "dsp/debug.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

/* ── Engine selection ────────────────────────────────────────────────────
 *
 * Default ON: the DSP56300 interpreter runs the guest-downloaded GP/EP
 * programs, which is what produces the title's actual mix. The passthrough
 * stub is the fallback and can be selected with RECOMP_APU_DSP=0. */
static bool apu_dsp_engine_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_DSP");
        on = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return on != 0;
}

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. With the passthrough stub the word never changes and the title hangs
 * inside DirectSound initialisation.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 *
 * gp:<byte offset> and ep:<byte offset> resolve through the programmed scratch
 * scatter/gather tables. Those registers contain table addresses, not the
 * payload base; a fixed guest heap address becomes stale when allocation
 * order changes. This remains an explicitly enabled diagnostic bypass.
 */
#define APU_DSP_ACK_MAX 8
static struct { uint32_t offset, processor; } s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;

static void dsp_ack_config_error(const char *spec)
{
    fprintf(stderr, "[APU] invalid RECOMP_APU_DSP_ACK '%s'; expected aligned addresses or gp:/ep: offsets\n", spec);
    exit(EXIT_FAILURE);
}

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_APU_DSP_ACK");
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
    if (!spec || !*spec)
        return;
    if (strlen(spec) >= sizeof buf) dsp_ack_config_error(spec);
    memcpy(buf, spec, strlen(spec) + 1);
    for (p = buf; *p; ) {
        uint32_t processor = 0;
        if (!strncmp(p, "gp:", 3)) { processor = 1; p += 3; }
        else if (!strncmp(p, "ep:", 3)) { processor = 2; p += 3; }
        errno = 0;
        unsigned long long v = strtoull(p, &end, 0);
        if (*p < '0' || *p > '9' || end == p || errno ||
            v > UINT32_MAX || (v & 3) || (*end && *end != ','))
            dsp_ack_config_error(spec);
        if (v || processor) {
            if (s_dsp_ack_count == APU_DSP_ACK_MAX) dsp_ack_config_error(spec);
            s_dsp_ack[s_dsp_ack_count].offset = (uint32_t)v;
            s_dsp_ack[s_dsp_ack_count++].processor = processor;
        }
        if (!*end) break;
        p = end + 1;
        if (!*p) dsp_ack_config_error(spec);
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] diagnostic DSP passthrough ack: %d mailbox(es); DSP commands are NOT emulated\n",
                s_dsp_ack_count);
}

void mcpx_apu_dsp_ack_frame(MCPXAPUState *d)
{
    int i;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;
    for (i = 0; i < s_dsp_ack_count; i++) {
        uint64_t physical = s_dsp_ack[i].offset;
        uint32_t processor = s_dsp_ack[i].processor;
        if (processor) {
            uint32_t table = qatomic_read(&d->regs[processor == 1 ? NV_PAPU_GPSADDR : NV_PAPU_EPSADDR]);
            if (!table) continue;
            uint32_t page = s_dsp_ack[i].offset >> 12;
            uint32_t last = qatomic_read(&d->regs[processor == 1 ? NV_PAPU_GPSMAXSGE : NV_PAPU_EPSMAXSGE]);
            if (page > last) {
                fprintf(stderr, "[APU] DSP ack offset 0x%08X exceeds scratch SGE limit %u\n",
                        s_dsp_ack[i].offset, last);
                exit(EXIT_FAILURE);
            }
            uint32_t base = ldl_le_phys(address_space_memory, (uint64_t)table + page * NV_PSGE_SIZE) & 0xFFFFF000u;
            if (!base) continue;
            physical = (uint64_t)base + (s_dsp_ack[i].offset & 0xFFFu);
        }
        volatile uint32_t *slot = (volatile uint32_t *)mcpx_apu_ram_address(physical, 4);
        if (*slot) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] diagnostic DSP doorbell physical 0x%08llX: command 0x%08X"
                                " bypassed (passthrough)\n", (unsigned long long)physical, *slot);
            *slot = 0;
        }
    }
}

/* SUM EVERY MIXBIN THE GUEST ROUTED TO, NOT JUST THE FIRST TWO.
 *
 * Default ON. RECOMP_APU_MIXDOWN_ALL=0 restores the previous two-bin read,
 * because this changes audible output for every title and an escape hatch
 * costs one branch. */
static int mcpx_apu_mixdown_all(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_MIXDOWN_ALL");
        on = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return on;
}

/* ============================================================
 * Passthrough stub frame
 * ============================================================ */

static void dsp_frame_stub(MCPXAPUState *d,
                           float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* The Xbox DirectSound typically routes:
     *   Mixbin 0 = Front Left
     *   Mixbin 1 = Front Right
     *   Mixbin 2 = Center (often unused in stereo)
     *   Mixbin 3 = LFE
     *   Mixbin 4-5 = Rear L/R
     *
     * Bins 2..31 used to be computed and then dropped on the floor. On
     * hardware the GP and EP mix the submixes down; here they are stubs, so
     * thirty of thirty-two bins were discarded every frame. Even bins left,
     * odd bins right, which preserves the stereo pairing the guest set up.
     * This is not what a real EP does; it is the cheapest mixdown that stops
     * discarding audio. */
    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
    bool diagnostic = mcpx_apu_diagnostics_enabled();

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float left, right;
            if (mcpx_apu_mixdown_all()) {
                left = 0.0f;
                right = 0.0f;
                for (int b = 0; b < NUM_MIXBINS; ++b) {
                    if (b & 1) right += mixbins[b][i];
                    else       left  += mixbins[b][i];
                }
            } else {
                left = mixbins[0][i];
                right = mixbins[1][i];
            }
            if (diagnostic) {
                g_dbg.ep.mix_peak_since_report = fmaxf(
                    g_dbg.ep.mix_peak_since_report,
                    fmaxf(fabsf(left), fabsf(right)));
                g_dbg.ep.clipped_since_report +=
                    (fabsf(left) > 1.0f) + (fabsf(right) > 1.0f);
                g_dbg.ep.nonfinite_since_report +=
                    !isfinite(left) + !isfinite(right);
            }
            if (left > 1.0f) left = 1.0f;
            if (left < -1.0f) left = -1.0f;
            if (right > 1.0f) right = 1.0f;
            if (right < -1.0f) right = -1.0f;

            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
        }
    }

    g_dbg.gp.cycles = 0;
    g_dbg.ep.cycles = 0;
}

/* ============================================================
 * Real GP/EP DSP56300 pipeline (ported from xemu gp_ep.c)
 * ============================================================ */

unsigned long mcpx_apu_dsp_gp_runs;
unsigned long mcpx_apu_dsp_ep_runs;
unsigned long mcpx_apu_dsp_ep_sinks;

static const int16_t ep_silence[256][2] = { 0 };

extern void xbox_ApuHostWrite(uint8_t *host, uint32_t bytes);
extern int xbox_DmaBankOf(uint64_t physical);
extern int xbox_ContigOwnsOffset(uint64_t physical);
extern void xbox_DumpArenas(uint32_t physical, uint32_t bytes);

/* Tripwire for DSP->guest-RAM DMA writes. Witnesses where the running effects
 * program scribbles; set RECOMP_APU_DMA_TRACE=1. Rate-limited. */
static void apu_dma_tripwire(uint32_t guest, size_t len)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_APU_DMA_TRACE") != NULL;
    if (!on) return;
    static unsigned long n;
    static uint32_t seen[16];
    static int nseen;
    if (n < 512)
        fprintf(stderr, "[APU-DMA] write guest=0x%08X len=%zu bank=%d contig_owns=%d\n",
                guest, len, xbox_DmaBankOf(guest), xbox_ContigOwnsOffset(guest));
    for (int i = 0; i < nseen; i++)
        if (seen[i] == guest) return;
    if (nseen < 16) {
        seen[nseen++] = guest;
        xbox_DumpArenas(guest, 16);
    }
    n++;
}

static void scatter_gather_rw(MCPXAPUState *d, hwaddr sge_base,
                              unsigned int max_sge, uint8_t *ptr, uint32_t addr,
                              size_t len, bool dir)
{
    unsigned int page_entry = addr / TARGET_PAGE_SIZE;
    unsigned int offset_in_page = addr % TARGET_PAGE_SIZE;
    unsigned int bytes_to_copy = TARGET_PAGE_SIZE - offset_in_page;

    (void)d;
    (void)max_sge;

    while (len > 0) {
        uint32_t prd_address = ldl_le_phys(address_space_memory,
                                           sge_base + page_entry * 8 + 0);
        uint8_t *guest = mcpx_apu_ram_address(prd_address + offset_in_page,
                                              TARGET_PAGE_SIZE);

        /* Diagnostic: force DSP payload writes to ordinary RAM instead of the
         * contiguous redirect, to test whether the write to the window is what
         * corrupts GPU memory. Set RECOMP_APU_DATA_ORDINARY=1. */
        if (dir) {
            static int ord = -1;
            if (ord < 0) ord = getenv("RECOMP_APU_DATA_ORDINARY") != NULL;
            if (ord) {
                extern ptrdiff_t xbox_GetMemoryOffset(void);
                guest = (uint8_t *)((uintptr_t)xbox_GetMemoryOffset() +
                                    prd_address + offset_in_page);
            }
        }

        if (bytes_to_copy > len) {
            bytes_to_copy = len;
        }

        if (dir) {
            static int nowrite = -1;
            if (nowrite < 0) nowrite = getenv("RECOMP_APU_DMA_NOWRITE") != NULL;
            apu_dma_tripwire(prd_address + offset_in_page, bytes_to_copy);
            xbox_ApuHostWrite(guest, bytes_to_copy);
            if (!nowrite)
                memcpy(guest, ptr, bytes_to_copy);
        } else {
            memcpy(ptr, guest, bytes_to_copy);
        }

        ptr += bytes_to_copy;
        len -= bytes_to_copy;

        /* After the first iteration, we are page aligned */
        page_entry += 1;
        bytes_to_copy = TARGET_PAGE_SIZE;
        offset_in_page = 0;
    }
}

static void gp_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len,
                          bool dir)
{
    MCPXAPUState *d = opaque;
    scatter_gather_rw(d, d->regs[NV_PAPU_GPSADDR], d->regs[NV_PAPU_GPSMAXSGE],
                      ptr, addr, len, dir);
}

static void ep_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len,
                          bool dir)
{
    MCPXAPUState *d = opaque;
    scatter_gather_rw(d, d->regs[NV_PAPU_EPSADDR], d->regs[NV_PAPU_EPSMAXSGE],
                      ptr, addr, len, dir);
}

static uint32_t circular_scatter_gather_rw(MCPXAPUState *d, hwaddr sge_base,
                                           unsigned int max_sge, uint8_t *ptr,
                                           uint32_t base, uint32_t end,
                                           uint32_t cur, size_t len, bool dir)
{
    while (len > 0) {
        unsigned int bytes_to_copy = end - cur;

        if (bytes_to_copy > len) {
            bytes_to_copy = len;
        }

        DPRINTF("circular scatter gather %s in range 0x%x - 0x%x at 0x%x of "
                "length 0x%x / 0x%lx bytes\n",
                dir ? "write" : "read", base, end, cur, bytes_to_copy, len);

        assert((cur >= base) && ((cur + bytes_to_copy) <= end));
        scatter_gather_rw(d, sge_base, max_sge, ptr, cur, bytes_to_copy, dir);

        ptr += bytes_to_copy;
        len -= bytes_to_copy;

        /* After the first iteration we might have to wrap */
        cur += bytes_to_copy;
        if (cur >= end) {
            assert(cur == end);
            cur = base;
        }
    }

    return cur;
}

static void gp_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                       size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    uint32_t base;
    uint32_t end;
    hwaddr cur_reg;
    if (dir) {
        assert(index < GP_OUTPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_GPOFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_GPOFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_GPOFCUR0 + 0x10 * index;
    } else {
        assert(index < GP_INPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_GPIFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_GPIFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_GPIFCUR0 + 0x10 * index;
    }

    uint32_t cur = GET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE);

    /* DSP hangs if current >= end; but forces current >= base */
    assert(cur < end);
    if (cur < base) {
        cur = base;
    }

    cur = circular_scatter_gather_rw(d,
        d->regs[NV_PAPU_GPFADDR], d->regs[NV_PAPU_GPFMAXSGE],
        ptr, base, end, cur, len, dir);

    SET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE, cur);
}

static bool ep_sink_samples(MCPXAPUState *d, uint8_t *ptr, size_t len)
{
    if (d->monitor.point == MCPX_APU_DEBUG_MON_AC97) {
        return false;
    } else if ((d->monitor.point == MCPX_APU_DEBUG_MON_EP) ||
        (d->monitor.point == MCPX_APU_DEBUG_MON_GP_OR_EP)) {
        assert(len == sizeof(d->monitor.frame_buf));
        memcpy(d->monitor.frame_buf, ptr, len);
        mcpx_apu_dsp_ep_sinks++;
    }

    return true;
}

static void ep_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                       size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    uint32_t base;
    uint32_t end;
    hwaddr cur_reg;
    if (dir) {
        assert(index < EP_OUTPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_EPOFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_EPOFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_EPOFCUR0 + 0x10 * index;
    } else {
        assert(index < EP_INPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_EPIFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_EPIFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_EPIFCUR0 + 0x10 * index;
    }

    uint32_t cur = GET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE);

    if (dir && index == 0) {
        bool did_sink = ep_sink_samples(d, ptr, len);
        if (did_sink) {
            /* Since we are sinking, push silence out */
            assert(len <= sizeof(ep_silence));
            ptr = (uint8_t*)ep_silence;
        }
    }

    /* DSP hangs if current >= end; but forces current >= base */
    if (cur >= end) {
        cur = cur % (end - base);
    }
    if (cur < base) {
        cur = base;
    }

    cur = circular_scatter_gather_rw(d,
        d->regs[NV_PAPU_EPFADDR], d->regs[NV_PAPU_EPFMAXSGE],
        ptr, base, end, cur, len, dir);

    SET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE, cur);
}

static void proc_rst_write(DSPState *dsp, uint32_t oldval, uint32_t val)
{
    if (mcpx_apu_diagnostics_enabled() && (val != oldval))
        fprintf(stderr, "[APU-DSP] %s RST 0x%X -> 0x%X\n",
                dsp->is_gp ? "GP" : "EP", oldval, val);
    if (!(val & NV_PAPU_GPRST_GPRST) || !(val & NV_PAPU_GPRST_GPDSPRST)) {
        dsp_reset(dsp);
    } else if (
        (!(oldval & NV_PAPU_GPRST_GPRST) || !(oldval & NV_PAPU_GPRST_GPDSPRST))
        && ((val & NV_PAPU_GPRST_GPRST) && (val & NV_PAPU_GPRST_GPDSPRST))) {
        dsp_bootstrap(dsp);
    }
}

/* Global Processor - programmable DSP */
uint64_t mcpx_apu_gp_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = opaque;

    assert(size == 4);
    assert(addr % 4 == 0);

    uint64_t r = 0;
    switch (addr) {
    case NV_PAPU_GPXMEM ... NV_PAPU_GPXMEM + 0x1000 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_GPXMEM) / 4;
        r = dsp_read_memory(d->gp.dsp, 'X', xaddr);
        break;
    }
    case NV_PAPU_GPMIXBUF ... NV_PAPU_GPMIXBUF + 0x400 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_GPMIXBUF) / 4;
        r = dsp_read_memory(d->gp.dsp, 'X', GP_DSP_MIXBUF_BASE + xaddr);
        break;
    }
    case NV_PAPU_GPYMEM ... NV_PAPU_GPYMEM + 0x800 * 4 - 1: {
        uint32_t yaddr = (addr - NV_PAPU_GPYMEM) / 4;
        r = dsp_read_memory(d->gp.dsp, 'Y', yaddr);
        break;
    }
    case NV_PAPU_GPPMEM ... NV_PAPU_GPPMEM + 0x1000 * 4 - 1: {
        uint32_t paddr = (addr - NV_PAPU_GPPMEM) / 4;
        r = dsp_read_memory(d->gp.dsp, 'P', paddr);
        break;
    }
    default:
        r = d->gp.regs[addr];
        break;
    }
    DPRINTF("mcpx apu GP: read [0x%" HWADDR_PRIx "] -> 0x%lx\n", addr, r);

    return r;
}

void mcpx_apu_gp_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;

    assert(size == 4);
    assert(addr % 4 == 0);

    DPRINTF("mcpx apu GP: [0x%" HWADDR_PRIx "] = 0x%lx\n", addr, val);

    switch (addr) {
    case NV_PAPU_GPXMEM ... NV_PAPU_GPXMEM + 0x1000 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_GPXMEM) / 4;
        dsp_write_memory(d->gp.dsp, 'X', xaddr, val);
        break;
    }
    case NV_PAPU_GPMIXBUF ... NV_PAPU_GPMIXBUF + 0x400 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_GPMIXBUF) / 4;
        dsp_write_memory(d->gp.dsp, 'X', GP_DSP_MIXBUF_BASE + xaddr, val);
        break;
    }
    case NV_PAPU_GPYMEM ... NV_PAPU_GPYMEM + 0x800 * 4 - 1: {
        uint32_t yaddr = (addr - NV_PAPU_GPYMEM) / 4;
        dsp_write_memory(d->gp.dsp, 'Y', yaddr, val);
        break;
    }
    case NV_PAPU_GPPMEM ... NV_PAPU_GPPMEM + 0x1000 * 4 - 1: {
        uint32_t paddr = (addr - NV_PAPU_GPPMEM) / 4;
        dsp_write_memory(d->gp.dsp, 'P', paddr, val);
        break;
    }
    case NV_PAPU_GPRST:
        proc_rst_write(d->gp.dsp, d->gp.regs[NV_PAPU_GPRST], val);
        d->gp.regs[NV_PAPU_GPRST] = val;
        break;
    default:
        d->gp.regs[addr] = val;
        break;
    }
}

/* Encode Processor - encoding DSP */
uint64_t mcpx_apu_ep_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = opaque;

    assert(size == 4);
    assert(addr % 4 == 0);

    uint64_t r = 0;
    switch (addr) {
    case NV_PAPU_EPXMEM ... NV_PAPU_EPXMEM + 0xC00 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_EPXMEM) / 4;
        r = dsp_read_memory(d->ep.dsp, 'X', xaddr);
        break;
    }
    case NV_PAPU_EPYMEM ... NV_PAPU_EPYMEM + 0x100 * 4 - 1: {
        uint32_t yaddr = (addr - NV_PAPU_EPYMEM) / 4;
        r = dsp_read_memory(d->ep.dsp, 'Y', yaddr);
        break;
    }
    case NV_PAPU_EPPMEM ... NV_PAPU_EPPMEM + 0x1000 * 4 - 1: {
        uint32_t paddr = (addr - NV_PAPU_EPPMEM) / 4;
        r = dsp_read_memory(d->ep.dsp, 'P', paddr);
        break;
    }
    default:
        r = d->ep.regs[addr];
        break;
    }
    DPRINTF("mcpx apu EP: read [0x%" HWADDR_PRIx "] -> 0x%lx\n", addr, r);

    return r;
}

void mcpx_apu_ep_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;

    assert(size == 4);
    assert(addr % 4 == 0);

    DPRINTF("mcpx apu EP: [0x%" HWADDR_PRIx "] = 0x%lx\n", addr, val);

    switch (addr) {
    case NV_PAPU_EPXMEM ... NV_PAPU_EPXMEM + 0xC00 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_EPXMEM) / 4;
        dsp_write_memory(d->ep.dsp, 'X', xaddr, val);
        break;
    }
    case NV_PAPU_EPYMEM ... NV_PAPU_EPYMEM + 0x100 * 4 - 1: {
        uint32_t yaddr = (addr - NV_PAPU_EPYMEM) / 4;
        dsp_write_memory(d->ep.dsp, 'Y', yaddr, val);
        break;
    }
    case NV_PAPU_EPPMEM ... NV_PAPU_EPPMEM + 0x1000 * 4 - 1: {
        uint32_t paddr = (addr - NV_PAPU_EPPMEM) / 4;
        dsp_write_memory(d->ep.dsp, 'P', paddr, val);
        break;
    }
    case NV_PAPU_EPRST:
        proc_rst_write(d->ep.dsp, d->ep.regs[NV_PAPU_EPRST], val);
        d->ep.regs[NV_PAPU_EPRST] = val;
        d->ep_frame_div = 0; /* FIXME: Still unsure about frame sync */
        break;
    default:
        d->ep.regs[addr] = val;
        break;
    }
}

static void dsp_frame_engine(MCPXAPUState *d,
                             float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    static bool logged_gp, logged_ep;

    /* Write VP results to the GP DSP MIXBUF */
    for (int mixbin = 0; mixbin < NUM_MIXBINS; mixbin++) {
        uint32_t base = GP_DSP_MIXBUF_BASE + mixbin * NUM_SAMPLES_PER_FRAME;
        for (int sample = 0; sample < NUM_SAMPLES_PER_FRAME; sample++) {
            dsp_write_memory(d->gp.dsp, 'X', base + sample,
                             float_to_24b(mixbins[mixbin][sample]));
        }
    }

    bool ep_enabled = (d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPRST) &&
                      (d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPDSPRST);

    /* Run GP */
    if ((d->gp.regs[NV_PAPU_GPRST] & NV_PAPU_GPRST_GPRST) &&
        (d->gp.regs[NV_PAPU_GPRST] & NV_PAPU_GPRST_GPDSPRST)) {
        dsp_start_frame(d->gp.dsp);
        d->gp.dsp->core.is_idle = false;
        d->gp.dsp->core.cycle_count = 0;
        do {
            dsp_run(d->gp.dsp, 1000);
        } while (!d->gp.dsp->core.is_idle && d->gp.realtime);
        g_dbg.gp.cycles = d->gp.dsp->core.cycle_count;
        mcpx_apu_dsp_gp_runs++;
        if (!logged_gp) {
            logged_gp = true;
            fprintf(stderr, "[APU-DSP] GP first frame: cycles=%u\n",
                    d->gp.dsp->core.cycle_count);
        }

        if ((d->monitor.point == MCPX_APU_DEBUG_MON_GP) ||
            (d->monitor.point == MCPX_APU_DEBUG_MON_GP_OR_EP && !ep_enabled)) {
            int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                uint32_t l = dsp_read_memory(d->gp.dsp, 'X', 0x1400 + i);
                d->monitor.frame_buf[off + i][0] = l >> 8;
                uint32_t r =
                    dsp_read_memory(d->gp.dsp, 'X', 0x1400 + 1 * 0x20 + i);
                d->monitor.frame_buf[off + i][1] = r >> 8;
            }
        }
    }

    /* Run EP */
    if ((d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPRST) &&
        (d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPDSPRST)) {
        if (d->ep_frame_div % 8 == 0) {
            dsp_start_frame(d->ep.dsp);
            d->ep.dsp->core.is_idle = false;
            d->ep.dsp->core.cycle_count = 0;
            do {
                dsp_run(d->ep.dsp, 1000);
            } while (!d->ep.dsp->core.is_idle && d->ep.realtime);
            g_dbg.ep.cycles = d->ep.dsp->core.cycle_count;
            mcpx_apu_dsp_ep_runs++;
            if (!logged_ep) {
                logged_ep = true;
                fprintf(stderr, "[APU-DSP] EP first frame: cycles=%u\n",
                        d->ep.dsp->core.cycle_count);
            }
        }
    }
}

static void dsp_init_engine(MCPXAPUState *d)
{
    d->gp.dsp = dsp_init(d, gp_scratch_rw, gp_fifo_rw);
    for (int i = 0; i < DSP_PRAM_SIZE; i++) {
        d->gp.dsp->core.pram[i] = 0xCACACACA;
    }
    memset(d->gp.dsp->core.pram_opcache, 0,
           sizeof(d->gp.dsp->core.pram_opcache));
    d->gp.dsp->is_gp = true;
    d->gp.dsp->core.is_gp = true;
    d->gp.dsp->core.is_idle = false;
    d->gp.dsp->core.cycle_count = 0;

    d->ep.dsp = dsp_init(d, ep_scratch_rw, ep_fifo_rw);
    for (int i = 0; i < DSP_PRAM_SIZE; i++) {
        d->ep.dsp->core.pram[i] = 0xCACACACA;
    }
    memset(d->ep.dsp->core.pram_opcache, 0,
           sizeof(d->ep.dsp->core.pram_opcache));
    for (int i = 0; i < DSP_XRAM_SIZE; i++) {
        d->ep.dsp->core.xram[i] = 0xCACACACA;
    }
    for (int i = 0; i < DSP_YRAM_SIZE; i++) {
        d->ep.dsp->core.yram[i] = 0xCACACACA;
    }
    d->ep.dsp->is_gp = false;
    d->ep.dsp->core.is_gp = false;
    d->ep.dsp->core.is_idle = false;
    d->ep.dsp->core.cycle_count = 0;

    fprintf(stderr, "[APU] DSP GP/EP initialized (DSP56300 engine, RECOMP_APU_DSP=1)\n");
}

/* ============================================================
 * Public API
 * ============================================================ */

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    static int last_known_preference = -1;
    int preference = apu_dsp_engine_enabled() ? 1 : 0;

    if (last_known_preference == preference) {
        return;
    }

    if (preference) {
        d->monitor.point = MCPX_APU_DEBUG_MON_GP_OR_EP;
        d->gp.realtime = true;
        d->ep.realtime = true;
    } else {
        /* Stub path: leave monitor.point alone so the passthrough mixdown
         * keeps writing to the monitor buffer. */
        d->gp.realtime = false;
        d->ep.realtime = false;
    }

    last_known_preference = preference;
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    if (apu_dsp_engine_enabled()) {
        dsp_init_engine(d);
        mcpx_apu_update_dsp_preference(d);
        return;
    }

    /* Allocate minimal DSP state for GP and EP. We need these to exist so
     * reset doesn't crash, but they won't actually run DSP programs. */
    d->gp.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->ep.dsp = (DSPState *)calloc(1, sizeof(DSPState));

    if (d->gp.dsp) d->gp.dsp->is_gp = true;
    if (d->ep.dsp) d->ep.dsp->is_gp = false;

    d->gp.realtime = false;
    d->ep.realtime = false;

    fprintf(stderr, "[APU] DSP GP/EP initialized (STUBBED - passthrough mode)\n");
}

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    if (apu_dsp_engine_enabled()) {
        dsp_frame_engine(d, mixbins);
    } else {
        dsp_frame_stub(d, mixbins);
    }
}