/*
 * assert_shim/assert.h - non-fatal, logging replacement for <assert.h>
 * inside the xbox_apu target.
 *
 * The APU/VP/DSP code is ported from xemu, where the invariant checks that
 * guard the emulated DSP56300 (scatter-gather bounds, PRAM/XRAM/YRAM limits,
 * FIFO widths, ADPCM block sizes) stay live. Our CMake Release build defines
 * NDEBUG, so every one of those asserts is compiled out and a protocol
 * violation silently reads or writes out of bounds - which surfaces as corrupt
 * audio instead of a diagnosable failure.
 *
 * This directory is prepended to xbox_apu's include path, so every
 * `#include <assert.h>` in the target resolves here. We pull in the real
 * header first (keeping static_assert and the _assert declaration), then
 * redefine `assert` to a throttled, non-fatal logger. Execution continues
 * exactly as it did under NDEBUG; we only gain a greppable [APU-ASSERT]
 * line naming the first (and periodically repeated) violating site.
 *
 * Everything is gated on RECOMP_APU_ASSERT_LOG (set by CMakeLists.txt);
 * without it this is just the untouched system <assert.h>.
 */
#ifndef APU_ASSERT_SHIM_H
#define APU_ASSERT_SHIM_H

/* Real system <assert.h> (next in the search path). Its own header
 * unconditionally #undef's and redefines `assert`, which is why this shim
 * must be the one place every APU translation unit goes through. */
#include_next <assert.h>

#ifdef RECOMP_APU_ASSERT_LOG

#undef assert

#ifdef __cplusplus
extern "C" {
#endif

void apu_assert_log(const char *expr, const char *file, int line);

#ifdef __cplusplus
}
#endif

#define assert(cond)                                    \
    do {                                                \
        if (!(cond)) {                                  \
            apu_assert_log(#cond, __FILE__, __LINE__);  \
        }                                               \
    } while (0)

#endif /* RECOMP_APU_ASSERT_LOG */

#endif /* APU_ASSERT_SHIM_H */