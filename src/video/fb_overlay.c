/**
 * Frametime overlay for the framebuffer window.
 *
 * Frame pacing is judged on the screen, and "it stutters in busy scenes" is
 * not a measurement. This keeps the last ten seconds of presented-frame
 * intervals, draws them as bars over the finished frame, and prints a summary
 * line often enough to land in a runtime log.
 *
 * The interval is flip-to-flip on the executor thread, which is the rate the
 * title actually presents at. The phase numbers beside it say where the time
 * went: the GPU wait (nv2a_pb_exec_flush / nv2a_gpu_flip) and the present
 * itself (the framebuffer copy and any external presenter -- in D3D11 mode
 * that includes the swap chain's vsync wait).
 *
 * Enabled with RECOMP_FRAMETIME_GRAPH=1; F9 toggles it live. Drawing is a
 * plain CPU composite into the BGRA buffer both presenters already hand to the
 * screen, so it needs no window of its own and works on the GDI, SDL and
 * D3D11 paths alike.
 *
 * RECOMP_FRAMETIME_TARGET sets the budget line in ms (default 16.7).
 * RECOMP_FRAMETIME_CSV=<path> writes one row per frame.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include "../platform/win32_compat.h"
#endif

#include "fb_overlay.h"

#define FT_SAMPLES 600           /* ~10 s at 60 Hz */
#define FT_PANEL_UNITS 300       /* panel width in unscaled pixels */
#define FT_PANEL_H_UNITS 122     /* four text lines + graph */
#define FT_GRAPH_H_UNITS 62
#define FT_LINE_H_UNITS 12
#define FT_PAD_UNITS 6
#define FT_TEXT_SCALE 2          /* glyph pixels: 3x5 * 2 */

static volatile LONG s_enabled;
static volatile LONG s_init;
static volatile LONG s_seq;      /* odd while a sample is being written */
static double s_target_ms = 16.7;
static FILE  *s_csv;
static unsigned s_csv_rows;

/* Ring of presented-frame samples. Written by the executor thread at the flip,
 * read by the window thread that draws. */
struct ft_sample {
    float    interval_ms;
    float    wait_ms;
    float    present_ms;
    uint32_t draws;
    uint32_t vertices;
};

static struct ft_sample s_ring[FT_SAMPLES];
static unsigned s_head;
static unsigned s_count;
static double   s_last_flip_ms;
static double   s_frame_start_ms;
static unsigned s_total_frames;
static float    s_last_interval, s_last_wait, s_last_present;
static uint32_t s_last_draws;
static uint32_t s_prev_draws;

/* Window-thread copy of the ring, taken through the sequence counter. */
static struct ft_sample s_snap[FT_SAMPLES];
static unsigned s_snap_head, s_snap_count;

static double s_stat_time;
static float  s_stat_avg, s_stat_min, s_stat_max, s_stat_p99, s_stat_low1;
static unsigned s_stat_spikes;

static double s_log_time;

static char s_text[4][64];

static double host_ms(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
#endif
}

static int env_on(const char *name, int fallback)
{
    const char *v = getenv(name);
    if (!v || !v[0])
        return fallback;
    return !(v[0] == '0' && !v[1]);
}

void xbox_OverlayInit(void)
{
    const char *target, *csv;

    if (InterlockedExchange(&s_init, 1))
        return;
    target = getenv("RECOMP_FRAMETIME_TARGET");
    if (target && target[0]) {
        double v = atof(target);
        if (v >= 1.0 && v <= 1000.0)
            s_target_ms = v;
    }
    csv = getenv("RECOMP_FRAMETIME_CSV");
    if (csv && csv[0]) {
        s_csv = fopen(csv, "w");
        if (s_csv) {
            fprintf(s_csv, "frame,interval_ms,wait_ms,present_ms,draws,vertices\n");
            fflush(s_csv);
            fprintf(stderr, "  [FRAMETIME] per-frame csv -> %s\n", csv);
        } else {
            fprintf(stderr, "  [FRAMETIME] cannot open csv %s\n", csv);
        }
    }
    InterlockedExchange(&s_enabled, env_on("RECOMP_FRAMETIME_GRAPH", 0));
    fprintf(stderr, "  [FRAMETIME] overlay %s (F9 toggles); budget %.2f ms\n",
            xbox_OverlayEnabled() ? "on" : "off", s_target_ms);
    fflush(stderr);
}

int xbox_OverlayEnabled(void)
{
    xbox_OverlayInit();
    return InterlockedCompareExchange(&s_enabled, 1, 1) != 0;
}

/* Sampling is not the same as drawing. A run with RECOMP_FRAMETIME_CSV wants
 * every frame measured and written even when the panel is not on screen --
 * before this, the CSV and the [FRAMETIME] summary were gated on the overlay
 * being visible, so an automated run with the graph off logged nothing. */
static int ft_sampling(void)
{
    return s_csv != NULL || xbox_OverlayEnabled();
}

void xbox_OverlayToggle(void)
{
    int on;

    xbox_OverlayInit();
    on = !xbox_OverlayEnabled();
    InterlockedExchange(&s_enabled, on);
    fprintf(stderr, "  [FRAMETIME] overlay %s\n", on ? "on" : "off");
    fflush(stderr);
    if (on)
        s_stat_time = 0.0;       /* recompute on the next draw */
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static void compute_stats(const struct ft_sample *snap, unsigned n, double now_s)
{
    static float values[FT_SAMPLES];
    unsigned i, spikes = 0, low_n;
    double sum = 0.0, low_sum = 0.0;

    s_stat_time = now_s;
    s_stat_avg = s_stat_min = s_stat_max = s_stat_p99 = s_stat_low1 = 0.0f;
    s_stat_spikes = 0;
    if (!n)
        return;
    for (i = 0; i < n; i++) {
        float v = snap[i].interval_ms;
        values[i] = v;
        sum += v;
        if (i == 0 || v < s_stat_min) s_stat_min = v;
        if (v > s_stat_max) s_stat_max = v;
        if (v > (float)(s_target_ms * 2.0)) spikes++;
    }
    s_stat_avg = (float)(sum / n);
    s_stat_spikes = spikes;
    qsort(values, n, sizeof(values[0]), cmp_float);
    s_stat_p99 = values[(unsigned)((double)(n - 1) * 0.99)];
    low_n = n / 100;
    if (low_n < 1) low_n = 1;
    for (i = 0; i < low_n; i++)
        low_sum += values[n - 1 - i];
    s_stat_low1 = (float)(1000.0 * low_n / (low_sum + 1e-9));
}

/* Snapshot through the sequence counter so a sample landing mid-copy does not
 * show up as a torn bar. */
static unsigned snapshot(double now_s)
{
    unsigned attempt;

    (void)now_s;
    for (attempt = 0; attempt < 4; attempt++) {
        LONG a = InterlockedCompareExchange(&s_seq, 0, 0);
        if (a & 1)
            continue;
        memcpy(s_snap, s_ring, sizeof(s_snap));
        s_snap_head = s_head;
        s_snap_count = s_count;
        if (InterlockedCompareExchange(&s_seq, 0, 0) == a)
            break;
    }
    return s_snap_count;
}

/* Snapshot order is oldest-first. */
static const struct ft_sample *snap_at(unsigned i)
{
    unsigned start = (s_snap_count < FT_SAMPLES) ? 0 : s_snap_head;
    return &s_snap[(start + i) % FT_SAMPLES];
}

static uint32_t blend(uint32_t dst, uint32_t src, unsigned alpha)
{
    unsigned inv = 256 - alpha;
    unsigned rb = (((src & 0xFF00FF) * alpha) + ((dst & 0xFF00FF) * inv)) >> 8;
    unsigned g  = (((src & 0x00FF00) * alpha) + ((dst & 0x00FF00) * inv)) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00);
}

static void fill_rect(uint32_t *px, int w, int h, int stride,
                      int x, int y, int rw, int rh, uint32_t color, unsigned a)
{
    int yy, xx;

    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x + rw > w) rw = w - x;
    if (y + rh > h) rh = h - y;
    if (rw <= 0 || rh <= 0)
        return;
    for (yy = 0; yy < rh; yy++) {
        uint32_t *row = px + (size_t)(y + yy) * stride;
        for (xx = 0; xx < rw; xx++)
            row[x + xx] = a >= 256 ? color : blend(row[x + xx], color, a);
    }
}

/* 3x5 glyphs, one 3-bit row each, rows laid out in bits 0..14 top first, the
 * leftmost column in bit 2 of each row (the patterns below read left to
 * right). Uppercase only; anything unknown draws as a space. */
static const char s_glyph_chars[] = "0123456789ACDEFGIKLMNOPRSTVWXY:-./%";
static const unsigned short s_glyph_rows[] = {
    31599, 29874, 31183, 29647, 5101, 29671, 31719, 4687, 31727, 29679,
    23535, 31015, 27502, 31207, 18919, 31591, 29847, 23469, 31012, 23549,
    24573, 31599, 18927, 24047, 29671, 5271, 11117, 24557, 23213, 9389,
    1040, 448, 8192, 18569, 22669
};

static void draw_text(uint32_t *px, int w, int h, int stride, int x, int y,
                      int scale, const char *text, uint32_t color)
{
    size_t i;

    for (i = 0; text[i]; i++) {
        char c = text[i];
        size_t g;
        unsigned short rows = 0;
        int col, row;

        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        for (g = 0; g < sizeof(s_glyph_chars) - 1; g++) {
            if (s_glyph_chars[g] == c) {
                rows = s_glyph_rows[g];
                break;
            }
        }
        for (row = 0; row < 5; row++) {
            unsigned bits = (rows >> (row * 3)) & 7u;
            for (col = 0; col < 3; col++) {
                /* bit 2 is the leftmost column, as the tables read */
                if ((bits & (1u << (2 - col))) && c != ' ')
                    fill_rect(px, w, h, stride,
                              x + (int)i * 4 * scale + col * scale,
                              y + row * scale, scale, scale, color, 256);
            }
        }
    }
}

/* Height of a frame interval inside the graph box, clipped so an 11 s freeze
 * does not flatten the rest; `clipped` tells the caller to colour it. */
static int bar_height(float ms, float ymax, int graph_h, int *clipped)
{
    double v = (double)ms / (double)ymax * (double)graph_h;

    *clipped = 0;
    if (v > graph_h) {
        *clipped = 1;
        v = graph_h;
    }
    if (v < 1.0)
        v = 1.0;
    return (int)v;
}

void xbox_OverlayComposite(uint32_t *px, int w, int h, int stride)
{
    int scale, pad, line_h, panel_w, panel_h, gx, gy, gw, gh, i;
    unsigned n, nbars, step;
    double now_ms, now_s;
    float ymax;

    if (!px || w <= 0 || h <= 0 || !xbox_OverlayEnabled())
        return;
    /* The panel is sized to be readable but unobtrusive: the constants are
     * already in screen pixels, so it stays the same physical size from 480p
     * to 1080p and only doubles past 4K. */
    scale = h / 900;
    if (scale < 1) scale = 1;
    if (scale > 2) scale = 2;
    pad = FT_PAD_UNITS * scale;
    line_h = FT_LINE_H_UNITS * scale;
    panel_w = FT_PANEL_UNITS * scale;
    panel_h = FT_PANEL_H_UNITS * scale;
    if (panel_w > w) panel_w = w;
    if (panel_h > h) panel_h = h;
    /* Top right: the safe corners of a HUD are the upper ones, but the left
     * one sits over the title's own readouts. */
    gx = w - panel_w + pad / 2;
    gy = pad + 4 * line_h;
    gw = panel_w - 2 * pad;
    gh = panel_h - gy - pad;
    if (gx < pad) gx = pad;
    if (gw < 16 || gh < 8)
        return;

    now_ms = host_ms();
    now_s = now_ms / 1000.0;
    n = snapshot(now_s);
    if (n && now_s - s_stat_time >= 0.5)
        compute_stats(s_snap, n, now_s);

    ymax = (float)(s_target_ms * 2.0);
    if (s_stat_max > ymax) ymax = s_stat_max;
    if (ymax > (float)(s_target_ms * 8.0))
        ymax = (float)(s_target_ms * 8.0);

    fill_rect(px, w, h, stride, gx - pad / 2, pad / 2, panel_w - pad / 3,
              panel_h, 0x000000, 190);
    fill_rect(px, w, h, stride, gx, gy, gw, gh, 0x101010, 110);
    {
        int clipped = 0;
        int ty = gy + gh - bar_height((float)s_target_ms, ymax, gh, &clipped);
        fill_rect(px, w, h, stride, gx, ty, gw, 1, 0x808080, 160);
    }

    /* Oldest on the left; when there are more samples than pixels a column
     * shows the worst of its pair. */
    nbars = (unsigned)gw;
    step = (n > nbars) ? (n + nbars - 1) / nbars : 1;
    for (i = 0; i < (int)nbars && n; i++) {
        unsigned first = (unsigned)i * step;
        unsigned last = first + step, j;
        float worst = 0.0f;
        int bh, clipped;
        uint32_t color;

        if (first >= n)
            break;
        if (last > n) last = n;
        for (j = first; j < last; j++) {
            float v = snap_at(j)->interval_ms;
            if (v > worst) worst = v;
        }
        bh = bar_height(worst, ymax, gh, &clipped);
        if (clipped || worst > (float)(s_target_ms * 2.5))
            color = 0xD03030;
        else if (worst > (float)s_target_ms)
            color = 0xD0B030;
        else
            color = 0x30B050;
        fill_rect(px, w, h, stride, gx + i, gy + gh - bh, 1, bh, color, 210);
    }

    {
        int y = pad;
        snprintf(s_text[0], sizeof(s_text[0]), "FRAME %.1f %.0fFPS",
                 (double)s_last_interval,
                 s_last_interval > 0.01f ? (double)(1000.0f / s_last_interval) : 0.0);
        snprintf(s_text[1], sizeof(s_text[1]), "AVG %.1f MAX %.1f",
                 (double)s_stat_avg, (double)s_stat_max);
        snprintf(s_text[2], sizeof(s_text[2]), "P99 %.1f 1%%LOW %.0f",
                 (double)s_stat_p99, (double)s_stat_low1);
        snprintf(s_text[3], sizeof(s_text[3]), "W %.1f P %.1f D %u SPK %u",
                 (double)s_last_wait, (double)s_last_present, s_last_draws,
                 s_stat_spikes);
        for (i = 0; i < 4; i++) {
            draw_text(px, w, h, stride, gx, y, FT_TEXT_SCALE * scale,
                      s_text[i], 0xE8E8E8);
            y += line_h;
        }
        /* A flip that never came is the one thing the bars cannot show: they
         * just stop. */
        if (s_last_flip_ms > 0.0 && now_ms - s_last_flip_ms > 500.0) {
            char stall[48];
            snprintf(stall, sizeof(stall), "NO FRAME %.1fS",
                     (now_ms - s_last_flip_ms) / 1000.0);
            draw_text(px, w, h, stride, gx, pad + 4 * line_h + gh + pad / 2,
                      FT_TEXT_SCALE * scale, stall, 0xFF4040);
        }
    }
}

void xbox_OverlayFlip(double wait_ms, uint32_t draws, uint32_t vertices)
{
    double now_ms;
    float interval;
    struct ft_sample *s;

    if (!ft_sampling())
        return;
    now_ms = host_ms();
    s_frame_start_ms = now_ms;
    if (s_last_flip_ms > 0.0 && now_ms > s_last_flip_ms)
        interval = (float)(now_ms - s_last_flip_ms);
    else
        interval = 0.0f;
    s_last_flip_ms = now_ms;
    s_last_interval = interval;
    s_last_wait = (float)wait_ms;
    /* The guest counter is cumulative; per-frame is what says how busy the
     * frame was. */
    s_last_draws = draws >= s_prev_draws ? draws - s_prev_draws : draws;
    s_prev_draws = draws;
    s_total_frames++;
    if (s_log_time == 0.0)
        s_log_time = now_ms / 1000.0;

    InterlockedIncrement(&s_seq);
    s = &s_ring[s_head];
    s->interval_ms = interval;
    s->wait_ms = (float)wait_ms;
    s->present_ms = s_last_present;
    s->draws = s_last_draws;
    s->vertices = vertices;
    s_head = (s_head + 1) % FT_SAMPLES;
    if (s_count < FT_SAMPLES)
        s_count++;
    InterlockedIncrement(&s_seq);

    if (s_csv) {
        fprintf(s_csv, "%u,%.3f,%.3f,%.3f,%u,%u\n", s_total_frames,
                (double)interval, wait_ms, s_last_present, s_last_draws,
                vertices);
        if (++s_csv_rows % 60u == 0)
            fflush(s_csv);
    }

    /* Periodic summary for the runtime log: the overlay itself is visible only
     * on the screen, and a log line is what a diary or a test can read. */
    if (now_ms / 1000.0 - s_log_time >= 5.0) {
        unsigned n = s_count < FT_SAMPLES ? s_count : FT_SAMPLES;
        static struct ft_sample local[FT_SAMPLES];
        static float vals[FT_SAMPLES];
        double sum = 0.0, low_sum = 0.0;
        float mx = 0.0f, p99 = 0.0f, low1 = 0.0f;
        unsigned spikes = 0, m = 0, low_n, i;

        s_log_time = now_ms / 1000.0;
        memcpy(local, s_ring, sizeof(local));
        for (i = 0; i < n; i++) {
            unsigned idx = (s_count < FT_SAMPLES) ? i
                                                  : (s_head + i) % FT_SAMPLES;
            float v = local[idx].interval_ms;
            vals[m++] = v;
            sum += v;
            if (v > mx) mx = v;
            if (v > (float)(s_target_ms * 2.0)) spikes++;
        }
        if (!m)
            return;
        qsort(vals, m, sizeof(vals[0]), cmp_float);
        p99 = vals[(unsigned)((double)(m - 1) * 0.99)];
        low_n = m / 100;
        if (low_n < 1) low_n = 1;
        for (i = 0; i < low_n; i++)
            low_sum += vals[m - 1 - i];
        low1 = (float)(1000.0 * low_n / (low_sum + 1e-9));
        fprintf(stderr,
                "  [FRAMETIME] %u frames avg %.1f p99 %.1f max %.1f 1%%low %.0ffps spikes %u\n",
                n, sum / m, (double)p99, (double)mx, (double)low1, spikes);
        fflush(stderr);
    }
}

void xbox_OverlayPresentDone(void)
{
    double present_ms;

    if (!ft_sampling() || s_frame_start_ms <= 0.0)
        return;
    present_ms = host_ms() - s_frame_start_ms;
    if (present_ms < 0.0)
        present_ms = 0.0;
    InterlockedIncrement(&s_seq);
    s_last_present = (float)present_ms;
    if (s_count)
        s_ring[(s_head + FT_SAMPLES - 1) % FT_SAMPLES].present_ms =
            (float)present_ms;
    InterlockedIncrement(&s_seq);
}
