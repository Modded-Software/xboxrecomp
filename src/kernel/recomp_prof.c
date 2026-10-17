/**
 * Guest call profiler and shadow call stack (see recomp_prof.h).
 *
 * Time, not counts. recomp_trace.c's RECOMP_TRACE_PROFILE answers "where do
 * the calls go"; this answers "where does the time go", which is the question
 * once a title is merely slow rather than stuck. Both inclusive (the function
 * and everything it called) and self (minus its instrumented callees) time are
 * kept, because a hot function is either expensive itself or the root of an
 * expensive tree and the two need different fixes.
 *
 * Everything is per host thread, so the hot path takes no lock and does no
 * atomic: a TLS pointer, a push, an rdtsc, and at leave a pop and three adds
 * into the thread's own hash table. The reporter merges the tables with racy
 * reads, which is fine for a diagnostic -- a count read mid-increment is off by
 * one call.
 *
 * Keeping the stack honest. A lifted function leaves through a C return, so
 * the cleanup attribute fires on every normal exit, including through a tail
 * call (`sub_X(); return;`). It does not fire when the guest's own SEH unwinds
 * past a frame or a host longjmp skips it. The stack is therefore also checked
 * against the guest stack pointer at every entry: a call at guest esp E means
 * every recorded frame whose entry esp is strictly below E has already gone
 * (the guest stack grows down), so those are retired before the new frame is
 * pushed. Equal esp is kept, because a tail jump enters its target at the
 * caller's esp and the caller is still logically live.
 *
 * Recursion double-counts inclusive time (each active instance adds its own
 * span). Self time is exact.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "xbox_memory_layout.h"
#include "recomp_prof.h"

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>
#endif

extern RECOMP_TLS uint32_t g_esp;

#define PROF_MAX_DEPTH  1024
#define PROF_SLOTS      32768           /* power of two; ~17k lifted functions */
#define PROF_THREADS    64

typedef struct {
    uint32_t    va;
    const char *name;
    uint64_t    calls;
    uint64_t    incl;
    uint64_t    self;
} prof_ent;

typedef struct {
    uint32_t    va;
    uint32_t    esp;
    const char *name;
    uint64_t    t0;
    uint64_t    child;
} prof_frame;

typedef struct {
    unsigned long tid;
    volatile int  depth;
    int           max_depth;
    uint64_t      overflow;     /* entries not pushed: stack full */
    uint64_t      resynced;     /* frames retired by the esp check */
    uint64_t      table_full;
    prof_frame    frames[PROF_MAX_DEPTH];
    prof_ent      table[PROF_SLOTS];
} prof_thread;

static prof_thread *volatile g_threads[PROF_THREADS];
static volatile long g_nthreads;
static volatile int  g_active;
static RECOMP_TLS prof_thread *t_self;
static RECOMP_TLS int t_failed;

/* ---- clocks ------------------------------------------------------------ */

static uint64_t prof_ticks(void) { return __rdtsc(); }

static double host_seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

static uint64_t g_cal_tsc;
static double   g_cal_sec;

/* TSC ticks per second, measured against the host clock over the run so far.
 * The first report is taken seconds into a run, which is plenty of baseline. */
static double ticks_per_sec(void)
{
    double dt = host_seconds() - g_cal_sec;
    uint64_t dk = prof_ticks() - g_cal_tsc;
    if (dt <= 0.01)
        return 3.0e9;
    return (double)dk / dt;
}

/* ---- per-thread state ----------------------------------------------------- */

static unsigned long current_tid(void)
{
#ifdef _WIN32
    return (unsigned long)GetCurrentThreadId();
#else
    return (unsigned long)(uintptr_t)pthread_self();
#endif
}

static void start_reporter(void);

static prof_thread *prof_self(void)
{
    prof_thread *t = t_self;
    long slot;

    if (t || t_failed)
        return t;
#ifdef _WIN32
    t = (prof_thread *)VirtualAlloc(NULL, sizeof *t, MEM_COMMIT | MEM_RESERVE,
                                    PAGE_READWRITE);
    slot = InterlockedIncrement(&g_nthreads) - 1;
#else
    t = (prof_thread *)calloc(1, sizeof *t);
    slot = __sync_add_and_fetch(&g_nthreads, 1) - 1;
#endif
    if (!t || slot >= PROF_THREADS) {
        t_failed = 1;
        fprintf(stderr, "[PROF] thread %lu not profiled (%s)\n", current_tid(),
                t ? "thread table full" : "out of memory");
        return NULL;
    }
    t->tid = current_tid();
    g_threads[slot] = t;
    t_self = t;
    if (!g_active) {
        g_cal_tsc = prof_ticks();
        g_cal_sec = host_seconds();
        g_active = 1;
        start_reporter();
    }
    return t;
}

static prof_ent *lookup(prof_thread *t, uint32_t va, const char *name)
{
    unsigned i = (va * 2654435761u) & (PROF_SLOTS - 1);
    unsigned n;

    for (n = 0; n < PROF_SLOTS; n++) {
        prof_ent *e = &t->table[(i + n) & (PROF_SLOTS - 1)];
        if (e->va == va && e->name)
            return e;
        if (!e->name) {
            e->va = va;
            e->name = name;
            return e;
        }
    }
    t->table_full++;
    return NULL;
}

static void pop_frame(prof_thread *t, uint64_t now)
{
    int d = t->depth - 1;
    prof_frame *f = &t->frames[d];
    uint64_t dt = now - f->t0;
    prof_ent *e = lookup(t, f->va, f->name);

    if (e) {
        e->calls++;
        e->incl += dt;
        e->self += dt > f->child ? dt - f->child : 0;
    }
    t->depth = d;
    if (d > 0)
        t->frames[d - 1].child += dt;
}

recomp_prof_token recomp_prof_enter(const char *name, uint32_t va)
{
    prof_thread *t = prof_self();
    uint32_t esp = g_esp;
    uint64_t now;
    prof_frame *f;

    if (!t)
        return 0;
    now = prof_ticks();
    while (t->depth > 0 && t->frames[t->depth - 1].esp < esp) {
        pop_frame(t, now);
        t->resynced++;
    }
    if (t->depth >= PROF_MAX_DEPTH) {
        t->overflow++;
        return 0;
    }
    f = &t->frames[t->depth];
    f->va = va;
    f->esp = esp;
    f->name = name;
    f->t0 = now;
    f->child = 0;
    t->depth++;
    if (t->depth > t->max_depth)
        t->max_depth = t->depth;
    return t->depth;
}

void recomp_prof_leave(recomp_prof_token *tok)
{
    prof_thread *t = t_self;
    int idx = *tok - 1;
    uint64_t now;

    if (!t || idx < 0 || t->depth <= idx)
        return;                 /* not pushed, or already retired */
    now = prof_ticks();
    while (t->depth > idx)      /* stale deeper frames, then this one */
        pop_frame(t, now);
}

int recomp_prof_active(void) { return g_active; }

/* ---- stacks ------------------------------------------------------------- */

static void print_thread_stack(FILE *out, const char *indent, prof_thread *t)
{
    int d = t->depth, i, shown = 0;

    if (d > PROF_MAX_DEPTH)
        d = PROF_MAX_DEPTH;
    fprintf(out, "%sguest shadow stack, thread %lu, depth %d "
                 "(innermost first):\n", indent, t->tid, d);
    for (i = d - 1; i >= 0 && shown < 64; i--, shown++) {
        const prof_frame *f = &t->frames[i];
        fprintf(out, "%s  #%-3d %s (0x%08X) esp=%08X\n", indent, d - 1 - i,
                f->name ? f->name : "?", f->va, f->esp);
    }
    if (i >= 0)
        fprintf(out, "%s  ... %d outer frames not shown\n", indent, i + 1);
}

void recomp_prof_print_stack(FILE *out, const char *indent)
{
    if (!indent)
        indent = "  ";
    if (!t_self) {
        fprintf(out, "%sguest shadow stack: unavailable (%s)\n", indent,
                g_active ? "this thread never entered an instrumented function"
                         : "not an instrumented build; build with PROF=1");
        return;
    }
    print_thread_stack(out, indent, t_self);
    fflush(out);
}

void recomp_prof_print_all_stacks(FILE *out, const char *indent)
{
    long n = g_nthreads, i;

    if (!indent)
        indent = "  ";
    if (!g_active) {
        fprintf(out, "%sguest shadow stacks: unavailable (not an instrumented "
                     "build; build with PROF=1)\n", indent);
        return;
    }
    if (n > PROF_THREADS)
        n = PROF_THREADS;
    for (i = 0; i < n; i++)
        if (g_threads[i] && g_threads[i]->depth > 0)
            print_thread_stack(out, indent, g_threads[i]);
    fflush(out);
}

/* ---- report ------------------------------------------------------------- */

typedef struct { uint32_t va; const char *name; uint64_t calls, incl, self; } merged;

static int by_self(const void *a, const void *b)
{
    const merged *x = (const merged *)a, *y = (const merged *)b;
    return x->self < y->self ? 1 : x->self > y->self ? -1 : 0;
}

static merged *g_merged;

void recomp_prof_report(void)
{
    static const unsigned MS = PROF_SLOTS;
    const char *out_path = getenv("RECOMP_PROF_OUT");
    const char *top_env = getenv("RECOMP_PROF_TOP");
    unsigned top = top_env ? (unsigned)atoi(top_env) : 25u;
    long nt = g_nthreads, ti;
    unsigned used = 0, i;
    uint64_t total_self = 0, overflow = 0, resync = 0, full = 0;
    double tps = ticks_per_sec();
    FILE *csv;

    if (!g_active)
        return;
    if (!g_merged)
        g_merged = (merged *)calloc(MS, sizeof *g_merged);
    if (!g_merged)
        return;
    memset(g_merged, 0, MS * sizeof *g_merged);
    if (nt > PROF_THREADS)
        nt = PROF_THREADS;

    /* Merge by VA across threads (open addressing over the merged array). */
    for (ti = 0; ti < nt; ti++) {
        prof_thread *t = g_threads[ti];
        if (!t)
            continue;
        overflow += t->overflow;
        resync += t->resynced;
        full += t->table_full;
        for (i = 0; i < PROF_SLOTS; i++) {
            const prof_ent *e = &t->table[i];
            unsigned h, n;
            if (!e->name || !e->calls)
                continue;
            h = (e->va * 2654435761u) & (MS - 1);
            for (n = 0; n < MS; n++) {
                merged *m = &g_merged[(h + n) & (MS - 1)];
                if (!m->name) { m->va = e->va; m->name = e->name; used++; }
                if (m->va == e->va) {
                    m->calls += e->calls; m->incl += e->incl; m->self += e->self;
                    break;
                }
            }
        }
    }
    /* Compact and sort. */
    {
        unsigned w = 0;
        for (i = 0; i < MS; i++)
            if (g_merged[i].name)
                g_merged[w++] = g_merged[i];
        used = w;
    }
    qsort(g_merged, used, sizeof *g_merged, by_self);
    for (i = 0; i < used; i++)
        total_self += g_merged[i].self;

    fprintf(stderr, "[PROF] %u functions, %ld threads, %.2f s of guest "
            "self time; overflow=%llu resync=%llu table_full=%llu; top %u by self:\n",
            used, nt, total_self / tps, (unsigned long long)overflow,
            (unsigned long long)resync, (unsigned long long)full, top);
    fprintf(stderr, "[PROF]   %8s %8s %6s %12s  %s\n",
            "self_ms", "incl_ms", "self%", "calls", "function");
    for (i = 0; i < used && i < top; i++)
        fprintf(stderr, "[PROF]   %8.1f %8.1f %5.1f%% %12llu  %s (0x%08X)\n",
                g_merged[i].self * 1e3 / tps, g_merged[i].incl * 1e3 / tps,
                total_self ? 100.0 * g_merged[i].self / total_self : 0.0,
                (unsigned long long)g_merged[i].calls,
                g_merged[i].name, g_merged[i].va);
    fflush(stderr);

    /* Full table, rewritten in place each period: a killed run still leaves
     * the last one, and the file never grows past one table (~1.5 MB). */
    if (!out_path || !*out_path) {
        static char derived[1024];
        const char *log = getenv("RECOMP_LOG");
        if (!derived[0]) {
            size_t len = log ? strlen(log) : 0;
            if (len > 4 && !strcmp(log + len - 4, ".log"))
                len -= 4;
            if (len && len < sizeof derived - 16)
                snprintf(derived, sizeof derived, "%.*s.prof.csv", (int)len, log);
            else
                snprintf(derived, sizeof derived, "recomp_prof.csv");
        }
        out_path = derived;
    }
    csv = fopen(out_path, "w");
    if (csv) {
        fprintf(csv, "va,name,calls,self_ms,incl_ms\n");
        for (i = 0; i < used; i++)
            fprintf(csv, "0x%08X,%s,%llu,%.3f,%.3f\n", g_merged[i].va,
                    g_merged[i].name, (unsigned long long)g_merged[i].calls,
                    g_merged[i].self * 1e3 / tps, g_merged[i].incl * 1e3 / tps);
        fclose(csv);
    } else {
        fprintf(stderr, "[PROF] cannot write %s\n", out_path);
    }
}

#ifdef _WIN32
static DWORD WINAPI reporter(LPVOID unused)
#else
static void *reporter(void *unused)
#endif
{
    const char *s = getenv("RECOMP_PROF_SECS");
    unsigned secs = s ? (unsigned)atoi(s) : 30u;

    (void)unused;
    if (!secs)
        secs = 30;
    for (;;) {
#ifdef _WIN32
        Sleep(secs * 1000u);
#else
        sleep(secs);
#endif
        recomp_prof_report();
    }
    return 0;
}

static void start_reporter(void)
{
    const char *on = getenv("RECOMP_PROF");

    fprintf(stderr, "[PROF] instrumented build: guest shadow stack on; "
            "profile reports %s\n", on && *on && *on != '0'
            ? "on (RECOMP_PROF)" : "off (set RECOMP_PROF=1)");
    if (!on || !*on || *on == '0')
        return;
    atexit(recomp_prof_report);
#ifdef _WIN32
    {
        HANDLE h = CreateThread(NULL, 0, reporter, NULL, 0, NULL);
        if (h)
            CloseHandle(h);
    }
#else
    {
        pthread_t th;
        if (pthread_create(&th, NULL, reporter, NULL) == 0)
            pthread_detach(th);
    }
#endif
}
