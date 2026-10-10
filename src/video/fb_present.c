/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>

#if defined(_WIN32)
/* Raw mouse input (WM_INPUT) needs the Vista+ declarations. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern int xbox_VideoDesiredResolution(uint32_t *width, uint32_t *height);
int xbox_FramebufferDumpBmp(const char *path);

static volatile LONG s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */

/* A finished frame, taken at the flip and shown until the next one.
 *
 * The window used to convert straight out of guest memory every 16 ms. Even
 * pointed at the buffer the title had just finished, that races the executor
 * drawing the next frame into the other one and, whenever the two swap, puts
 * a half-drawn image on the screen -- which is the flicker. Copying the
 * finished frame once per flip means the window never reads memory the
 * rasteriser is writing, so what it shows cannot be half of anything.
 *
 * Two buffers and an index, swapped after the copy completes, so the window
 * thread is never reading the one being filled. */
static uint32_t     *s_present[2];
static volatile LONG s_present_idx = -1;   /* -1 until the first flip */
static DECLSPEC_ALIGN(8) volatile LONG64 s_present_frame;

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    LONG next;
    uint32_t bpp, x, y;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    InterlockedIncrement64(&s_present_frame);
    if (getenv("RECOMP_FB_VA"))
        return;                       /* pinned: leave the old path alone */
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height,
                                             4);
        if (!s_present[next])
            return;
    }
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        uint32_t *dst = s_present[next] + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
    /* Published only once it is whole. */
    InterlockedExchange(&s_present_idx, next);
}

/* Which keys are down, for the pad stand-in in src/input.
 *
 * GetAsyncKeyState looked like the cheaper way to ask and does not work
 * here: it reads a state Wine keeps for the X server, and a guest process
 * drawing through GDI never sees it change. The window that has the focus
 * is the thing that receives the keys, so that is what has to remember
 * them.
 *
 * Reading this needs no lock. Each entry is written only by the window
 * thread and read only by the USB thread, one byte at a time, and a press
 * seen a frame late is indistinguishable from one made a frame later. */
static volatile unsigned char s_key_down[256];

int xbox_FramebufferKeyDown(int vk)
{
    if ((unsigned)vk > 255)
        return 0;
    return s_key_down[vk] != 0;
}

/* Off-focus input injection for automated tests.
 *
 * XTEST routes key events to whatever holds the X input focus, and Wine's
 * wineserver then routes the resulting keyboard hardware message to the
 * focused window (server/queue.c:find_hardware_message_window). So an
 * automated tap either steals the user's focus or is dropped; XSendEvent to
 * the window is worse still, because Wine keeps its own key state and the key
 * sticks. There is no X-level way around it.
 *
 * Instead the test writes the set of held virtual-key codes to a small file
 * and this window thread applies it whenever it changes, straight into the
 * same s_key_down[] that WM_KEYDOWN writes. No X, no focus, no Wine keyboard
 * translation -- the events are injected from inside Wine, which is the only
 * place the keyboard model can be driven without the foreground. The file
 * holds whitespace-separated hex VK codes (e.g. "20 57" = Space + W); path is
 * RECOMP_INPUT_FILE (default C:\recomp-input.txt). */
static void fb_inject_poll(void)
{
    static int enabled = -1;
    static DWORD last;
    static unsigned char held[256];
    unsigned char next[256];
    const char *path;
    char buf[4096], *p;
    DWORD now;
    size_t n;
    FILE *f;
    int i;

    if (enabled < 0)
        enabled = getenv("RECOMP_INPUT_FILE") != NULL;
    if (!enabled)
        return;
    now = GetTickCount();
    if (now - last < 8)                    /* ~120 Hz is plenty */
        return;
    last = now;

    path = getenv("RECOMP_INPUT_FILE");
    f = fopen(path, "rb");
    if (!f)
        return;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    memset(next, 0, sizeof next);
    for (p = buf; *p; ) {
        unsigned long v;
        while (*p && !((*p >= '0' && *p <= '9') ||
                       (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
            p++;
        if (!*p)
            break;
        v = strtoul(p, &p, 16);
        if (v < 256)
            next[v] = 1;
    }
    for (i = 0; i < 256; i++) {
        if (next[i] == held[i])
            continue;
        held[i] = next[i];
        s_key_down[i] = next[i];
        if (next[i] && i == VK_F12) {
            const char *cap = getenv("RECOMP_FB_CAPTURE");
            if (!cap || !cap[0])
                cap = "framebuffer.bmp";
            if (xbox_FramebufferDumpBmp(cap) != 0)
                fprintf(stderr, "[FBWIN] failed to capture framebuffer to %s\n", cap);
        }
        if (getenv("RECOMP_KEY_TRACE"))
            fprintf(stderr, "  [KEY] inject %s vk=0x%02X\n",
                    next[i] ? "down" : "up", i);
    }
}

/* Mouse state, for the keyboard+mouse mode (src/input, RECOMP_KBM).
 *
 * Absolute cursor position is useless for a camera stick: the pointer hits the
 * window edge and stops. While KBM is active the window clips the cursor to its
 * client area and recentres it after every move, so what the reader consumes
 * is a delta, not a position. Deltas accumulate between polls and are cleared
 * on read; the input layer polls at the title's rate, so one read is one
 * frame's worth of motion.
 *
 * Same lock-free rule as the keys: written only by the window thread, read
 * only by the input thread, one word at a time. A delta seen a frame late is
 * indistinguishable from one made a frame later. */
static volatile LONG s_mouse_dx, s_mouse_dy, s_mouse_wheel;
static volatile unsigned char s_mouse_btn[3];   /* 0=L, 1=R, 2=M */
static int s_mouse_x, s_mouse_y, s_mouse_have;
static int s_mouse_capture, s_cursor_hidden;
static int s_mouse_raw;      /* WM_INPUT deltas are authoritative when set */
static HWND s_fb_hwnd;

int xbox_FramebufferMouseDelta(int *dx, int *dy)
{
    int x = (int)InterlockedExchange(&s_mouse_dx, 0);
    int y = (int)InterlockedExchange(&s_mouse_dy, 0);
    if (dx) *dx = x;
    if (dy) *dy = y;
    return (x || y);
}

int xbox_FramebufferMouseButton(int which)
{
    if ((unsigned)which > 2)
        return 0;
    return s_mouse_btn[which] != 0;
}

int xbox_FramebufferMouseWheel(void)
{
    return (int)InterlockedExchange(&s_mouse_wheel, 0);
}

/* Turn relative mouse mode on/off. Called on focus changes and once at startup
 * when RECOMP_KBM is set. */
void xbox_FramebufferMouseCapture(int on)
{
    if (!s_fb_hwnd)
        return;
    on = on ? 1 : 0;
    if (on == s_mouse_capture)
        return;
    s_mouse_capture = on;
    if (on) {
        RECT c, clip;
        POINT tl = { 0, 0 }, br, centre;
        RAWINPUTDEVICE rid;
        /* Raw input is the only motion source that does not fight the cursor:
         * it reports relative deltas straight from the device, so nothing has
         * to be clipped or recentred and the view cannot stall at a screen
         * edge. WM_MOUSEMOVE stays as the fallback for hosts where it fails. */
        rid.usUsagePage = 0x01;      /* generic desktop */
        rid.usUsage = 0x02;          /* mouse */
        rid.dwFlags = 0;
        rid.hwndTarget = s_fb_hwnd;
        s_mouse_raw = RegisterRawInputDevices(&rid, 1, sizeof(rid)) ? 1 : 0;
        if (s_mouse_raw)
            fprintf(stderr, "[FBWIN] mouse: raw input active\n");
        GetClientRect(s_fb_hwnd, &c);
        br.x = c.right; br.y = c.bottom;
        ClientToScreen(s_fb_hwnd, &tl);
        ClientToScreen(s_fb_hwnd, &br);
        clip.left = tl.x; clip.top = tl.y; clip.right = br.x; clip.bottom = br.y;
        ClipCursor(&clip);
        centre.x = (c.right - c.left) / 2;
        centre.y = (c.bottom - c.top) / 2;
        ClientToScreen(s_fb_hwnd, &centre);
        SetCursorPos(centre.x, centre.y);
        s_mouse_have = 1;
        if (!s_cursor_hidden) { ShowCursor(FALSE); s_cursor_hidden = 1; }
    } else {
        if (s_mouse_raw) {
            RAWINPUTDEVICE rid;
            rid.usUsagePage = 0x01; rid.usUsage = 0x02;
            rid.dwFlags = RIDEV_REMOVE; rid.hwndTarget = NULL;
            RegisterRawInputDevices(&rid, 1, sizeof(rid));
            s_mouse_raw = 0;
        }
        ClipCursor(NULL);
        if (s_cursor_hidden) { ShowCursor(TRUE); s_cursor_hidden = 0; }
    }
}

/* ---- input recording and replay ------------------------------------------
 *
 * RECOMP_INPUT_RECORD=<path> writes every input edge the window thread sees --
 * key down/up, mouse delta, button and wheel -- with a millisecond timestamp,
 * so a hands-on session can be replayed later. RECOMP_INPUT_REPLAY=<path> feeds
 * such a file back at the same times, with no human and no X focus involved.
 *
 * Recording happens on the window thread, at the same place WM_KEYDOWN and the
 * raw-input path already write s_key_down/s_mouse_*, so nothing else has to be
 * instrumented and no lock is needed. */
static FILE *s_rec;
static DWORD s_rec_start;
static int s_rec_state = -1;

static void rec_init(void)
{
    const char *path;
    s_rec_state = 0;
    path = getenv("RECOMP_INPUT_RECORD");
    if (!path || !path[0])
        return;
    s_rec = fopen(path, "wb");
    if (!s_rec) {
        fprintf(stderr, "[REC] cannot open %s for recording\n", path);
        return;
    }
    s_rec_start = GetTickCount();
    s_rec_state = 1;
    fprintf(s_rec, "# recomp input record v1: t_ms type args\n");
    fflush(s_rec);
    fprintf(stderr, "[REC] recording input to %s\n", path);
}

static void rec_event(const char *fmt, ...)
{
    va_list ap;
    if (s_rec_state < 0)
        rec_init();
    if (!s_rec)
        return;
    fprintf(s_rec, "%lu ", (unsigned long)(GetTickCount() - s_rec_start));
    va_start(ap, fmt);
    vfprintf(s_rec, fmt, ap);
    va_end(ap);
    fputc('\n', s_rec);
    fflush(s_rec);
}

struct RecEvent { DWORD t; char type; long a, b; };
static struct RecEvent *s_rep;
static int s_rep_n, s_rep_cap, s_rep_i;
static DWORD s_rep_start;
static int s_rep_state = -1;

static void rep_load(void)
{
    FILE *f;
    const char *path = getenv("RECOMP_INPUT_REPLAY");
    char line[256];
    s_rep_state = 0;
    if (!path || !path[0])
        return;
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[REP] cannot open %s for replay\n", path);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        struct RecEvent e;
        char ty = 0;
        if (line[0] == '#' || line[0] == '\n')
            continue;
        e.a = e.b = 0;
        if (sscanf(line, "%lu %c", &e.t, &ty) < 2)
            continue;
        /* Key codes are recorded in hex ("k %x"); everything else is decimal. */
        if (ty == 'k') {
            unsigned long vk = 0;
            long down = 0;
            sscanf(line, "%lu %c %lx %li", &e.t, &ty, &vk, &down);
            e.a = (long)vk;
            e.b = down;
        } else {
            sscanf(line, "%lu %c %li %li", &e.t, &ty, &e.a, &e.b);
        }
        e.type = ty;
        if (s_rep_n == s_rep_cap) {
            int cap = s_rep_cap ? s_rep_cap * 2 : 256;
            struct RecEvent *grown = (struct RecEvent *)realloc(s_rep, (size_t)cap * sizeof *grown);
            if (!grown) break;
            s_rep = grown;
            s_rep_cap = cap;
        }
        s_rep[s_rep_n++] = e;
    }
    fclose(f);
    s_rep_start = GetTickCount();
    s_rep_state = 1;
    /* The pad stand-in only reads keys/mouse while the cursor is captured. */
    if (getenv("RECOMP_KBM"))
        xbox_FramebufferMouseCapture(1);
    fprintf(stderr, "[REP] replaying %d events from %s\n", s_rep_n, path);
}

static void rep_apply(const struct RecEvent *e)
{
    switch (e->type) {
    case 'k':
        if ((unsigned)e->a < 256)
            s_key_down[e->a] = e->b ? 1 : 0;
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 80)
                fprintf(stderr, "  [KEY] replay %s vk=0x%02X\n",
                        e->b ? "down" : "up", (unsigned)e->a);
        }
        break;
    case 'u':
        memset((void *)s_key_down, 0, sizeof s_key_down);
        s_mouse_btn[0] = s_mouse_btn[1] = s_mouse_btn[2] = 0;
        break;
    case 'm':
        InterlockedExchangeAdd(&s_mouse_dx, (LONG)e->a);
        InterlockedExchangeAdd(&s_mouse_dy, (LONG)e->b);
        break;
    case 'b':
        if ((unsigned)e->a < 3)
            s_mouse_btn[e->a] = e->b ? 1 : 0;
        break;
    case 'w':
        InterlockedExchangeAdd(&s_mouse_wheel, (LONG)e->a);
        break;
    default:
        break;
    }
}

void fb_replay_poll(void)
{
    DWORD now;
    if (s_rep_state < 0)
        rep_load();
    if (!s_rep)
        return;
    now = GetTickCount() - s_rep_start;
    while (s_rep_i < s_rep_n && s_rep[s_rep_i].t <= now)
        rep_apply(&s_rep[s_rep_i++]);
}

static void fb_exit_process(void)
{
    InterlockedExchange(&s_fb_running, 0);
    fprintf(stderr, "[FBWIN] close requested; exiting process\n");
    fflush(stderr);
    ExitProcess(EXIT_SUCCESS);
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CLOSE:
        fb_exit_process();
        return 0;

    case WM_DESTROY:
        InterlockedExchange(&s_fb_running, 0);
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        rec_event("k %x 1", (unsigned)w);
        if (w == VK_F12) {
            const char *path = getenv("RECOMP_FB_CAPTURE");
            if (!path || !path[0]) path = "framebuffer.bmp";
            if (xbox_FramebufferDumpBmp(path) != 0)
                fprintf(stderr, "[FBWIN] failed to capture framebuffer to %s\n", path);
            return 0;
        }
        /* Alt+Esc releases the mouse so the window can be moved or the desktop
         * reached; clicking the client grabs it again. Bare Esc is the game's
         * Back, so it must not release on its own. Alt is the only modifier the
         * game leaves unbound (Ctrl is crouch, Shift is BLACK), but two things
         * ate the combination: the default handler opens the system-menu loop on
         * Alt's WM_SYSKEYDOWN and swallows the Esc, and the desktop's
         * cycle-windows binding (GNOME) owns Alt+Esc at the compositor. Alt is
         * swallowed below so no menu loop forms; the compositor binding must be
         * cleared for the chord to arrive in the first place. */
        if (w == VK_ESCAPE && (GetKeyState(VK_MENU) & 0x8000)) {
            if (getenv("RECOMP_KBM"))
                xbox_FramebufferMouseCapture(0);
            return 0;
        }
        /* Alt (VK_MENU) must not reach the default handler: it opens the
         * system-menu loop, which swallowed the Esc. Other system keys (Alt+F4)
         * still fall through. */
        if (w == VK_MENU || w == VK_LMENU || w == VK_RMENU) {
            if ((unsigned)w < 256)
                s_key_down[w] = 1;
            return 0;
        }
        if ((unsigned)w < 256)
            s_key_down[w] = 1;
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered.
         *
         * The obvious diagnostic -- sampling which keys are held, once a
         * second, from the input path -- cannot tell a key that was never
         * pressed from one that was tapped: a 100 ms press is caught about
         * one time in ten. That ambiguity is expensive when the only way
         * to test is to ask someone to press a key and describe what
         * happened. This answers "did it arrive" on its own. */
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        rec_event("k %x 0", (unsigned)w);
        if ((unsigned)w < 256)
            s_key_down[w] = 0;
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    case WM_MOUSEMOVE: {
        int x = (int)(short)LOWORD(l), y = (int)(short)HIWORD(l);
        if (s_mouse_capture) {
            RECT c;
            int cx, cy;
            GetClientRect(h, &c);
            cx = (c.right - c.left) / 2;
            cy = (c.bottom - c.top) / 2;
            if (s_mouse_raw) {
                /* Raw input (WM_INPUT) owns the deltas. Only keep the hidden
                 * cursor centred so clicks stay in the window. */
                if (x != cx || y != cy) {
                    POINT p;
                    p.x = cx; p.y = cy;
                    ClientToScreen(h, &p);
                    SetCursorPos(p.x, p.y);
                }
            } else {
                int dx = x - cx, dy = y - cy;
                /* Clip/recentre lands the cursor within a pixel of centre under
                 * Wine, and that residual would otherwise leak out as a constant
                 * slow drift. One pixel of motion is below anything usable. */
                if (dx > -2 && dx < 2) dx = 0;
                if (dy > -2 && dy < 2) dy = 0;
                if (dx || dy) {
                    POINT p;
                    rec_event("m %d %d", dx, dy);
                    InterlockedExchangeAdd(&s_mouse_dx, dx);
                    InterlockedExchangeAdd(&s_mouse_dy, dy);
                    p.x = cx; p.y = cy;
                    ClientToScreen(h, &p);
                    SetCursorPos(p.x, p.y);
                }
            }
        } else {
            if (s_mouse_have) {
                if (x != s_mouse_x || y != s_mouse_y)
                    rec_event("m %d %d", x - s_mouse_x, y - s_mouse_y);
                InterlockedExchangeAdd(&s_mouse_dx, x - s_mouse_x);
                InterlockedExchangeAdd(&s_mouse_dy, y - s_mouse_y);
            }
            s_mouse_x = x; s_mouse_y = y; s_mouse_have = 1;
        }
        return 0;
    }

    case WM_INPUT: {
        if (s_mouse_capture && s_mouse_raw) {
            RAWINPUT raw;
            UINT size = sizeof(raw);
            if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &raw, &size,
                                sizeof(RAWINPUTHEADER)) == sizeof(raw) &&
                raw.header.dwType == RIM_TYPEMOUSE &&
                !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                rec_event("m %d %d", (int)raw.data.mouse.lLastX, (int)raw.data.mouse.lLastY);
                InterlockedExchangeAdd(&s_mouse_dx, raw.data.mouse.lLastX);
                InterlockedExchangeAdd(&s_mouse_dy, raw.data.mouse.lLastY);
            }
        }
        return DefWindowProcA(h, m, w, l);
    }

    /* Non-client buttons (title bar, borders, menu box) must reach the default
     * handler so the window can be dragged or resized. A click there never
     * grabs the mouse; only a click inside the rendered client area does. */
    case WM_NCLBUTTONDOWN:
    case WM_NCRBUTTONDOWN:
    case WM_NCMBUTTONDOWN:
        return DefWindowProcA(h, m, w, l);

    case WM_LBUTTONDOWN:
        /* Clicking into the game grabs the mouse for mouse-look; Esc releases
         * it again so the window can be dragged. */
        if (getenv("RECOMP_KBM"))
            xbox_FramebufferMouseCapture(1);
        rec_event("b 0 1");
        s_mouse_btn[0] = 1;
        return 0;
    case WM_LBUTTONUP:   rec_event("b 0 0"); s_mouse_btn[0] = 0; return 0;
    case WM_RBUTTONDOWN: rec_event("b 1 1"); s_mouse_btn[1] = 1; return 0;
    case WM_RBUTTONUP:   rec_event("b 1 0"); s_mouse_btn[1] = 0; return 0;
    case WM_MBUTTONDOWN: rec_event("b 2 1"); s_mouse_btn[2] = 1; return 0;
    case WM_MBUTTONUP:   rec_event("b 2 0"); s_mouse_btn[2] = 0; return 0;

    case WM_MOUSEWHEEL:
        rec_event("w %d", (int)(GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA));
        InterlockedExchangeAdd(&s_mouse_wheel,
                               (LONG)(GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA));
        return 0;

    case WM_SETFOCUS:
        return 0;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        rec_event("u");
        memset((void *)s_key_down, 0, sizeof s_key_down);
        s_mouse_btn[0] = s_mouse_btn[1] = s_mouse_btn[2] = 0;
        xbox_FramebufferMouseCapture(0);
        return 0;

    /* The window is never maximized. The WM force-fits a window larger than
     * the monitor, which crops a 1:1 framebuffer to the screen and makes the
     * window look fullscreen, so both the state and the size are pinned. */
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)l;
        RECT r;
        r.left = 0; r.top = 0;
        r.right = (LONG)s_fb_width; r.bottom = (LONG)s_fb_height;
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, FALSE);
        mmi->ptMaxSize.x = mmi->ptMaxTrackSize.x = r.right - r.left;
        mmi->ptMaxSize.y = mmi->ptMaxTrackSize.y = r.bottom - r.top;
        return 0;
    }

    case WM_SIZE:
        if (IsZoomed(h))
            ShowWindow(h, SW_RESTORE);
        return 0;

    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_MAXIMIZE)
            return 0;
        break;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch;
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_fb_width * 3u) + 3u) & ~3u;
    uint32_t img = row * s_fb_height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !s_fb_va)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_fb_width, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    line = (uint8_t *)calloc(1, row);
    if (!line) { fclose(f); return -1; }
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        free(line); fclose(f); return -1;
    }
    for (y = 0; y < s_fb_height; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_fb_height - 1 - y) * s_fb_width;
        for (x = 0; x < s_fb_width; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        if (fwrite(line, 1, row, f) != row) {
            free(line); fclose(f); return -1;
        }
    }
    free(line);
    if (fclose(f) != 0) return -1;
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, s_fb_width, s_fb_height, s_fb_va);
    return 0;
}

static int fb_format_window_title(char *caption, size_t capacity, const char *game_title,
                                  double fps, LONG64 frame)
{
    int length = snprintf(caption, capacity, "%s | %ux%u | %.2f FPS | Frame %llu",
                          game_title, s_fb_width, s_fb_height, fps,
                          (unsigned long long)frame);
    if (length < 0 || (size_t)length >= capacity) {
        fprintf(stderr, "[FBWIN] failed to format window-title statistics\n");
        return 0;
    }
    return 1;
}

static void fb_set_window_title(HWND hwnd, const char *caption)
{
    if (!SetWindowTextA(hwnd, caption))
        fprintf(stderr, "[FBWIN] failed to update window title: error %lu\n", GetLastError());
}

typedef struct {
    HICON large_icon, small_icon;
    int found;
} FbWindowIcons;

static BOOL CALLBACK fb_load_icon_resource(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR context)
{
    FbWindowIcons *icons = (FbWindowIcons *)context;
    (void)type;
    icons->found = 1;
    icons->large_icon = (HICON)LoadImageW(module, name, IMAGE_ICON,
                                   GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED);
    if (!icons->large_icon)
        fprintf(stderr, "[FBWIN] cannot load executable large icon: error %lu\n", GetLastError());
    icons->small_icon = (HICON)LoadImageW(module, name, IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    if (!icons->small_icon)
        fprintf(stderr, "[FBWIN] cannot load executable small icon: error %lu\n", GetLastError());
    return FALSE;
}

static void fb_load_window_icons(HMODULE module, FbWindowIcons *icons)
{
    if (!EnumResourceNamesW(module, (LPCWSTR)RT_GROUP_ICON, fb_load_icon_resource, (LONG_PTR)icons)
        && !icons->found) {
        DWORD error = GetLastError();
        if (error == ERROR_RESOURCE_TYPE_NOT_FOUND || error == ERROR_RESOURCE_DATA_NOT_FOUND)
            fprintf(stderr, "[FBWIN] executable has no icon resource; using Windows default\n");
        else
            fprintf(stderr, "[FBWIN] cannot enumerate executable icons: error %lu\n", error);
    }
    if (!icons->large_icon) {
        icons->large_icon = (HICON)LoadImageW(NULL, MAKEINTRESOURCEW(32512), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED);
        if (!icons->large_icon)
            fprintf(stderr, "[FBWIN] cannot load default large icon: error %lu\n", GetLastError());
    }
    if (!icons->small_icon) {
        icons->small_icon = (HICON)LoadImageW(NULL, MAKEINTRESOURCEW(32512), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
        if (!icons->small_icon)
            fprintf(stderr, "[FBWIN] cannot load default small icon: error %lu\n", GetLastError());
    }
}

/* The certificate's UTF-16 title name, for the title bar. Set once while the
 * XBE loads, read once when the window is created. */
static char s_cert_title[64];

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;

    for (i = 0; i < max_chars && i < (int)sizeof(s_cert_title) - 1 && name[i]; i++)
        s_cert_title[i] = (char)name[i];
    s_cert_title[i] = 0;
}

/* Flip/draw counters the pushbuffer executor reports each frame. */
static volatile LONG s_frame_flips;
static volatile LONG s_frame_draws;

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    InterlockedIncrement(&s_frame_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;
    const char *window_title = getenv("RECOMP_WINDOW_TITLE");
    const char *game_title = window_title && window_title[0] ? window_title :
        s_cert_title[0] ? s_cert_title : "Xbox Recomp - Framebuffer";
    size_t caption_capacity = strlen(game_title) + 96;
    char *caption = (char *)malloc(caption_capacity);
    LARGE_INTEGER title_frequency, title_clock;
    LONG64 title_frame = InterlockedCompareExchange64(&s_present_frame, 0, 0);
    int title_stats = caption != NULL;
    HMODULE module = GetModuleHandleA(NULL);
    FbWindowIcons icons = {0};

    (void)unused;
    if (!caption)
        fprintf(stderr, "[FBWIN] cannot allocate window-title statistics buffer\n");
    else if (!QueryPerformanceFrequency(&title_frequency) || title_frequency.QuadPart <= 0 ||
             !QueryPerformanceCounter(&title_clock)) {
        fprintf(stderr, "[FBWIN] window-title statistics unavailable: cannot query performance clock\n");
        title_stats = 0;
    } else if (!fb_format_window_title(caption, caption_capacity, game_title, 0.0, title_frame))
        title_stats = 0;

    {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = module;
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        fb_load_window_icons(module, &icons);
        wc.hIcon         = icons.large_icon;
        wc.hIconSm       = icons.small_icon;
        wc.lpszClassName = "XboxRecompFramebuffer";
        if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            fprintf(stderr, "[FBWIN] framebuffer class registration failed: error %lu\n", GetLastError());
            InterlockedExchange(&s_fb_running, 0);
            free(caption);
            return 0;
        }
    }
    r.left = 0; r.top = 0; r.right = (LONG)s_fb_width; r.bottom = (LONG)s_fb_height;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExA(0, "XboxRecompFramebuffer",
                           title_stats ? caption : game_title,
                           (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX) | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, module, NULL);
    if (!hwnd) {
        fprintf(stderr, "[FBWIN] framebuffer window creation failed: error %lu\n", GetLastError());
        InterlockedExchange(&s_fb_running, 0);
        free(caption);
        return 0;
    }
    s_fb_hwnd = hwnd;
    /* Wine maps the window maximized when its requested size matches a monitor
     * mode; on a rotated display that clamps the 1:1 framebuffer to the
     * monitor and leaves the window undraggable. It is not maximizable (above),
     * so restoring keeps it a normal, movable, resizable 1:1 window. */
    ShowWindow(hwnd, SW_RESTORE);
    /* Take the keyboard. The exe is console-subsystem, so Wine also creates a
     * console window that otherwise holds the focus and receives every key --
     * the game window then sees none of them. Force this window forward and
     * focused so the pad-overlay keys arrive without a manual click.
     *
     * A plain SetForegroundWindow is not enough: Wine enforces the foreground
     * lock, and a game launched from a terminal is not the foreground process,
     * so the call is denied and the window stays unfocused until the user
     * clicks it. The topmost flip is the documented workaround -- raising the
     * window while it is topmost makes it the foreground one even when the
     * lock would otherwise say no -- and it is left not-topmost afterwards so
     * nothing else is occluded. */
    /* RECOMP_NO_FOCUS: an automated run must not pull focus away from the
     * user's work, and with file injection (fb_inject_poll) it does not need
     * the keyboard at all. Set to 0 to force the window foreground again. */
    {
        const char *nf = getenv("RECOMP_NO_FOCUS");
        if (!(nf && *nf && *nf != '0')) {
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(hwnd);
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE);
            SetFocus(hwnd);
        }
    }
    /* Mouse capture is opt-in per click (below), not on creation, so the
     * window can be dragged and other windows reached until the user clicks
     * into the game. */
    SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)icons.large_icon);
    SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icons.small_icon);
    hdc = GetDC(hwnd);

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);

    long last_drawn = -2;
    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                fb_exit_process();
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!InterlockedCompareExchange(&s_fb_running, 1, 1))
            break;
        if (title_stats) {
            LARGE_INTEGER now;
            if (!QueryPerformanceCounter(&now)) {
                fprintf(stderr, "[FBWIN] window-title statistics unavailable: cannot query performance counter\n");
                title_stats = 0;
                fb_set_window_title(hwnd, game_title);
            } else if (now.QuadPart - title_clock.QuadPart >= title_frequency.QuadPart) {
                LONG64 frame = InterlockedCompareExchange64(&s_present_frame, 0, 0);
                double elapsed = (double)(now.QuadPart - title_clock.QuadPart) / title_frequency.QuadPart;
                double fps = (double)(frame - title_frame) / elapsed;
                if (fb_format_window_title(caption, caption_capacity, game_title, fps, frame))
                    fb_set_window_title(hwnd, caption);
                else {
                    title_stats = 0;
                    fb_set_window_title(hwnd, game_title);
                }
                title_clock = now;
                title_frame = frame;
            }
        }
        if (s_present_idx >= 0 && s_rgb) {
            /* A finished frame, published by the flip. Copied into s_rgb so
             * the dump path and GDI see one consistent image even if the
             * next flip lands mid-blit. */
            LONG idx = s_present_idx;
            if (idx != last_drawn) {
                if (s_present[idx])
                    memcpy(s_rgb, s_present[idx],
                           (size_t)s_fb_width * s_fb_height * 4);
                StretchDIBits(hdc, 0, 0, (int)s_fb_width, (int)s_fb_height,
                              0, 0, (int)s_fb_width, (int)s_fb_height,
                              s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
                last_drawn = idx;
            }
        } else if (s_fb_va && s_fb_pitch && s_rgb) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory as
             * before, which is also what a title that never flips needs. */
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            fb_convert(src, s_fb_pitch / s_fb_width);
            StretchDIBits(hdc, 0, 0, (int)s_fb_width, (int)s_fb_height,
                          0, 0, (int)s_fb_width, (int)s_fb_height,
                          s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        /* Pump often enough to sample WM_INPUT/WM_KEYDOWN smoothly, but not
         * so tightly that the window thread monopolises a core. Rendering is
         * gated on a new frame, above. */
        fb_inject_poll();
        if (s_rec_state < 0)
            rec_init();
        fb_replay_poll();
        Sleep(4);
    }

    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(caption);
    free(s_rgb);
    s_rgb = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;
    uint32_t dw, dh;

    /* A requested resolution is what the title is steered into rendering, so
     * read the framebuffer at that size rather than the 480i default. */
    if (xbox_VideoDesiredResolution(&dw, &dh)) {
        s_fb_width  = dw;
        s_fb_height = dh;
    }

    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

#else
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowStart(void) {}
int xbox_FramebufferKeyDown(int vk) { (void)vk; return 0; }
int xbox_FramebufferMouseDelta(int *dx, int *dy) { if (dx) *dx = 0; if (dy) *dy = 0; return 0; }
int xbox_FramebufferMouseButton(int which) { (void)which; return 0; }
int xbox_FramebufferMouseWheel(void) { return 0; }
void xbox_FramebufferMouseCapture(int on) { (void)on; }
#endif
