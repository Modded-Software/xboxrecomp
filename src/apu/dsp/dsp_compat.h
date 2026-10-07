#ifndef MCPX_DSP_COMPAT_H
#define MCPX_DSP_COMPAT_H

#include "../apu_shim.h"

#ifndef memory_region_set_dirty
static inline void memory_region_set_dirty(MemoryRegion *mr, hwaddr offset,
                                           uint64_t size)
{
    (void)mr;
    (void)offset;
    (void)size;
}
#endif

#define trace_dsp56k_execute_instruction(...) ((void)0)
#define trace_dsp56k_execute_instruction_disasm(...) ((void)0)
#define trace_dsp_read_peripheral(...) ((void)0)
#define trace_dsp_write_peripheral(...) ((void)0)
#define trace_event_get_state(x) 0
#define TRACE_DSP56K_EXECUTE_INSTRUCTION_DISASM 0

#endif