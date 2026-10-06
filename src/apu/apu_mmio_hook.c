/*
 * MCPX APU MMIO Hook - VEH instruction decoder for APU register access
 *
 * Uses the shared x86-64 instruction decoder (platform/mmio_decode.h) that the
 * NV2A/OHCI hooks also use, routing reads/writes through the MCPX APU register
 * handlers. It used to carry its own opcode table; that copy was missing the
 * immediate and register-destination forms DirectSound emits against the DSP
 * mailboxes, so a `test dword ptr [...], imm32` faulted.
 */

#include "apu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

/* Global APU state pointer -- referenced from main.c regardless of which
 * platform's MMIO hook is active, so define it before the #if guard. */
MCPXAPUState *g_apu_state = NULL;

#if defined(_WIN32)
#include <windows.h>
#include "platform/mmio_decode.h"

/* APU MMIO base in Xbox VA space */
#define APU_MMIO_BASE  0xFE800000u
#define APU_MMIO_SIZE  0x00080000u  /* 512KB */

/* Statistics */
static int g_apu_mmio_read_count = 0;
static int g_apu_mmio_write_count = 0;
static int g_apu_mmio_decode_fail = 0;

static uint64_t apu_mmio_rd(void *dev, uint32_t off, int size)
{
    g_apu_mmio_read_count++;
    return mcpx_apu_mmio_read((MCPXAPUState *)dev, off, (unsigned)size);
}

static void apu_mmio_wr(void *dev, uint32_t off, uint64_t val, int size)
{
    g_apu_mmio_write_count++;
    mcpx_apu_mmio_write((MCPXAPUState *)dev, off, val, (unsigned)size);
}

static bool apu_decode_and_handle(PCONTEXT ctx, uint32_t mmio_offset, int is_write)
{
    (void)is_write;
    if (!g_apu_state)
        return false;
    return mmio_emulate(ctx, mmio_offset, g_apu_state,
                        apu_mmio_rd, apu_mmio_wr) != 0;
}

/* ============================================================
 * Public API (called from VEH in main.c)
 * ============================================================ */

bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write)
{
    uint32_t mmio_offset = fault_xbox_va - APU_MMIO_BASE;
    bool ok = apu_decode_and_handle(ctx, mmio_offset, is_write);

    if (!ok && g_apu_mmio_decode_fail++ < 20) {
        const uint8_t *ip = (const uint8_t *)ctx->Rip;
        fprintf(stderr, "[APU] MMIO decode fail at RIP=%p offset=0x%X: "
                        "%02X %02X %02X %02X %02X %02X\n",
                (void *)ctx->Rip, mmio_offset,
                ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }

    /* What the title actually asks the APU for. The DSPs are stubbed here, so
     * a title that waits on one waits forever, and the only way to work out
     * what it is waiting for is to see the register traffic that precedes the
     * wait. */
    if (getenv("RECOMP_APU_TRACE")) {
        static unsigned n;
        if (n++ < 400) {
            uint64_t v = g_apu_state
                       ? mcpx_apu_mmio_read(g_apu_state, mmio_offset, 4) : 0;
            fprintf(stderr, "  [APUMMIO] %s 0x%05X = %08X%s\n",
                    is_write ? "write" : "read ", mmio_offset,
                    (uint32_t)v, ok ? "" : "  (decode failed)");
        }
    }
    return ok;
}

#endif /* _WIN32 */
