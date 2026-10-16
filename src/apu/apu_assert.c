/*
 * apu_assert.c - backing for the non-fatal assert logger (assert_shim/assert.h).
 *
 * Keeps one record per distinct source site and throttles repeats so a
 * per-sample violation cannot flood the runtime log while still making the
 * first occurrence, and its location, loud and greppable ([APU-ASSERT]).
 */
#include <stdio.h>
#include <stdlib.h>

#define APU_ASSERT_MAX_SITES 512

static const char *g_site_file[APU_ASSERT_MAX_SITES];
static int         g_site_line[APU_ASSERT_MAX_SITES];
static unsigned long g_site_hits[APU_ASSERT_MAX_SITES];
static int         g_site_used;

void apu_assert_log(const char *expr, const char *file, int line)
{
    int i;

    for (i = 0; i < g_site_used; i++) {
        if (g_site_line[i] == line && g_site_file[i] == file) {
            g_site_hits[i]++;
            /* Throttle: first hit is logged above; then every 4096th. */
            if ((g_site_hits[i] & 0xFFF) == 0) {
                fprintf(stderr, "[APU-ASSERT] %s:%d (%s) hit x%lu\n",
                        file, line, expr, g_site_hits[i]);
            }
            return;
        }
    }

    if (g_site_used < APU_ASSERT_MAX_SITES) {
        g_site_file[g_site_used] = file;
        g_site_line[g_site_used] = line;
        g_site_hits[g_site_used] = 1;
        g_site_used++;
    }

    fprintf(stderr, "[APU-ASSERT] %s:%d (%s) failed (non-fatal)\n",
            file, line, expr);
}