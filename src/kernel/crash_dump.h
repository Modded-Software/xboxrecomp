#ifndef XBOX_CRASH_DUMP_H
#define XBOX_CRASH_DUMP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Write a host minidump (and, with RECOMP_CRASH_RAM=1, guest memory) next to
 * the runtime log. `exception_pointers` is the PEXCEPTION_POINTERS of a fault
 * or NULL for an on-demand dump; `force` writes even if a dump was already
 * written this process (on-demand dumps). Returns 1 if anything was written.
 * See crash_dump.c for the knobs. */
int xbox_CrashDump(void *exception_pointers, const char *reason, int force);

/* Fault deliberately, RECOMP_CRASH_TEST seconds after this call, on a helper
 * thread. Verifies the crash handler and the dumps it writes end to end
 * ("does a minidump actually come out, and is it usable") without having to
 * wait for a real crash. No-op unless RECOMP_CRASH_TEST is set. */
void xbox_CrashTestStart(void);

#ifdef __cplusplus
}
#endif

#endif
