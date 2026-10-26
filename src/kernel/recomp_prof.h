/**
 * Guest call profiler and shadow call stack.
 *
 * An instrumented build (one whose lifted functions open with
 * RECOMP_PROF_SCOPE) keeps, per host thread, a stack of the guest functions
 * currently executing and a table of how many times each was entered and how
 * much time it spent -- inclusive of its callees and on its own. The stack is
 * what a crash or a hang report needs and the guest otherwise cannot give: the
 * lift keeps no frame chain, so without it the only "backtrace" is a scan of
 * the stack for values that look like code.
 *
 * An ordinary build links this module but never enters it; every query then
 * reports that no shadow stack is available, rather than an empty one.
 *
 * Run-time knobs (instrumented build only):
 *   RECOMP_PROF=1            periodic report to stderr + CSV file
 *   RECOMP_PROF_SECS=N       report period, seconds (default 30)
 *   RECOMP_PROF_TOP=N        rows in the stderr report (default 25)
 *   RECOMP_PROF_OUT=path     CSV, rewritten each period (default: beside
 *                            RECOMP_LOG as <log>.prof.csv)
 */
#ifndef XBOX_RECOMP_PROF_H
#define XBOX_RECOMP_PROF_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Token handed back by enter and consumed by leave: the frame's depth + 1, or
 * 0 for a frame that was not pushed (stack full). */
typedef int recomp_prof_token;

recomp_prof_token recomp_prof_enter(const char *name, uint32_t va);
void recomp_prof_leave(recomp_prof_token *tok);

/* 1 once any thread has entered an instrumented function. */
int recomp_prof_active(void);

/* Print the calling thread's guest shadow stack, innermost first. */
void recomp_prof_print_stack(FILE *out, const char *indent);
/* Print every registered thread's shadow stack (racy snapshot; diagnostic). */
void recomp_prof_print_all_stacks(FILE *out, const char *indent);
/* Entry count for one guest function VA, across threads; 0 when the build is
 * not instrumented. For a hang: two samples a second apart say whether a call
 * is still being made, which a stack alone cannot. */
unsigned long long recomp_prof_calls(uint32_t va);
/* Write the profile table now (stderr summary + CSV). */
void recomp_prof_report(void);

#if defined(__GNUC__) || defined(__clang__)
#define RECOMP_PROF_SCOPE(name, va) \
    recomp_prof_token _recomp_prof_tok \
        __attribute__((cleanup(recomp_prof_leave), unused)) = \
        recomp_prof_enter((name), (va))
#else
#define RECOMP_PROF_SCOPE(name, va) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif
