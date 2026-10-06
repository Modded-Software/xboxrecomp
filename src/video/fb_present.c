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
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);
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
        if (w == VK_F12) {
            const char *path = getenv("RECOMP_FB_CAPTURE");
            if (!path || !path[0]) path = "framebuffer.bmp";
            if (xbox_FramebufferDumpBmp(path) != 0)
                fprintf(stderr, "[FBWIN] failed to capture framebuffer to %s\n", path);
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
        if ((unsigned)w < 256)
            s_key_down[w] = 0;
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        memset((void *)s_key_down, 0, sizeof s_key_down);
        return 0;
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
    int length = snprintf(caption, capacity, "%s | %.2f FPS | Frame %llu",
                          game_title, fps, (unsigned long long)frame);
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

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;
    const char *window_title = getenv("RECOMP_WINDOW_TITLE");
    const char *game_title = window_title && window_title[0] ? window_title : "Xbox Recomp - Framebuffer";
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
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, module, NULL);
    if (!hwnd) {
        fprintf(stderr, "[FBWIN] framebuffer window creation failed: error %lu\n", GetLastError());
        InterlockedExchange(&s_fb_running, 0);
        free(caption);
        return 0;
    }
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
            if (s_present[idx])
                memcpy(s_rgb, s_present[idx],
                       (size_t)s_fb_width * s_fb_height * 4);
            StretchDIBits(hdc, 0, 0, (int)s_fb_width, (int)s_fb_height,
                          0, 0, (int)s_fb_width, (int)s_fb_height,
                          s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
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
        Sleep(16);
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
#endif
