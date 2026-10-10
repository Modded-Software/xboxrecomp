/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

static LONG g_reported_slot[XBOX_MAX_CONTROLLERS] = { -1, -1, -1, -1 };
static int g_host_slot = -1;
static unsigned g_button_map[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
static unsigned g_button_threshold = 30, g_deadzone[2], g_axis_flags;
static const char *const g_button_names[16] = {
    "UP","DOWN","LEFT","RIGHT","START","BACK","LCLICK","RCLICK",
    "A","B","X","Y","BLACK","WHITE","LT","RT"
};
static const char *const g_source_names[17] = {
    "UP","DOWN","LEFT","RIGHT","START","BACK","LCLICK","RCLICK",
    "A","B","X","Y","LB","RB","LT","RT","NONE"
};

static void input_config_error(const char *name, const char *value)
{
    fprintf(stderr, "[XINPUT] invalid %s: '%s'; input configuration rejected\n", name, value);
    exit(EXIT_FAILURE);
}

static unsigned input_number(const char *name, unsigned maximum, unsigned fallback)
{
    const char *value = getenv(name);
    if (!value) return fallback;
    char *end;
    unsigned long number = strtoul(value, &end, 10);
    if (*value < '0' || *value > '9' || *end || number > maximum)
        input_config_error(name, value);
    return (unsigned)number;
}

static void input_configure(void)
{
    const char *slot = getenv("RECOMP_XINPUT_SLOT");
    g_host_slot = -1;
    if (slot && _stricmp(slot, "auto"))
        g_host_slot = (int)input_number("RECOMP_XINPUT_SLOT", 3, 0);
    g_button_threshold = input_number("RECOMP_INPUT_THRESHOLD", 254, 30);
    g_deadzone[0] = input_number("RECOMP_INPUT_DEADZONE_LEFT", 32767, 0);
    g_deadzone[1] = input_number("RECOMP_INPUT_DEADZONE_RIGHT", 32767, 0);
    g_axis_flags = input_number("RECOMP_INPUT_AXES", 31, 0);
    for (unsigned i = 0; i < 16; i++) g_button_map[i] = i;
    const char *mapping = getenv("RECOMP_INPUT_MAP");
    if (mapping) {
        char buffer[1024], *context;
        unsigned seen = 0;
        size_t length = strlen(mapping);
        if (!length || length >= sizeof(buffer) || mapping[0] == ',' ||
            mapping[length - 1] == ',' || strstr(mapping, ",,"))
            input_config_error("RECOMP_INPUT_MAP", mapping);
        memcpy(buffer, mapping, length + 1);
        char *entry = strtok_s(buffer, ",", &context);
        while (entry) {
            char *source = strchr(entry, '=');
            if (!source) input_config_error("RECOMP_INPUT_MAP", mapping);
            *source++ = '\0';
            unsigned target = 0, host = 0;
            while (target < 16 && _stricmp(entry, g_button_names[target])) target++;
            while (host < 17 && _stricmp(source, g_source_names[host])) host++;
            if (target == 16 || host == 17 || (seen & (1u << target)))
                input_config_error("RECOMP_INPUT_MAP", mapping);
            seen |= 1u << target;
            g_button_map[target] = host;
            entry = strtok_s(NULL, ",", &context);
        }
    }
    fprintf(stderr, "[XINPUT] profile: slot=%d (-1=auto) threshold=%u deadzones=%u/%u axes=%u\n",
            g_host_slot, g_button_threshold, g_deadzone[0], g_deadzone[1], g_axis_flags);
}

static BYTE source_pressure(const XINPUT_GAMEPAD *pad, unsigned source)
{
    static const WORD masks[14] = {
        XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
        XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
        XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER
    };
    if (source < 14) return (pad->wButtons & masks[source]) ? 255 : 0;
    if (source == 14) return pad->bLeftTrigger;
    if (source == 15) return pad->bRightTrigger;
    return 0;
}

static SHORT mapped_axis(SHORT value, unsigned flag)
{
    int axis = (g_axis_flags & flag) ? -(int)value : (int)value;
    return (SHORT)(axis > 32767 ? 32767 : axis);
}

static void map_stick(SHORT x, SHORT y, unsigned stick, SHORT *out_x, SHORT *out_y)
{
    int64_t radius = (int64_t)x * x + (int64_t)y * y;
    if (radius <= (int64_t)g_deadzone[stick] * g_deadzone[stick])
        x = y = 0;
    *out_x = mapped_axis(x, 2u << (stick * 2));
    *out_y = mapped_axis(y, 4u << (stick * 2));
}

/* ---- keyboard, when there is no pad --------------------------------------
 *
 * A title that waits on PRESS START is unreachable on a machine with no
 * controller plugged in, which is most machines someone brings this up on.
 * The whole point of a recompilation is to be able to look at the thing
 * running, and a build nobody can press a button in cannot be looked at.
 *
 * Off by default, because a keyboard silently acting as player 1 is
 * surprising when a real pad is what you meant to use. RECOMP_KEYBOARD=1
 * turns it on. It only ever answers for port 0, and is merged on top of a
 * pad connected there, so a real controller keeps working.
 *
 * The keys are the ones a Dreamcast or Saturn emulator would pick, which is
 * the closest thing to a convention here:
 *
 *   arrows        d-pad             Enter      START
 *   Z X A S       A B X Y           Backspace  BACK
 *   Q E           white black       1 3        triggers
 *   numpad 8/2/4/6 left thumb       I/K/J/L    right thumb
 *
 * Keys come from the framebuffer window, which only receives them while it
 * has the focus, so typing in another window does not drive the game.
 */
static BOOL keyboard_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_KEYBOARD");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    return on ? TRUE : FALSE;
}

/* The framebuffer window records what is held; see src/video/fb_present.c.
 *
 * GetAsyncKeyState was the first attempt and reads nothing here -- under
 * Wine it answers about a state a GDI-drawing guest never touches. Going
 * through the window is also the better gate: a window only receives keys
 * while it has the focus, so there is no separate focus check to get wrong
 * and no way for typing in another application to drive the game.
 *
 * Declared rather than included so src/input does not depend on the video
 * module's include path. */
extern int xbox_FramebufferKeyDown(int vk);

static BOOL key_down(int vk)
{
    return xbox_FramebufferKeyDown(vk) ? TRUE : FALSE;
}

/* A thumb axis from two keys, at the full deflection a digital key implies. */
static SHORT axis_from_keys(int negative, int positive)
{
    int v = 0;
    if (key_down(negative)) v -= 32767;
    if (key_down(positive)) v += 32767;
    return (SHORT)v;
}

static void keyboard_state(XBOX_INPUT_STATE *pState)
{
    static DWORD packet;
    WORD b = 0;

    memset(pState, 0, sizeof(*pState));

    if (key_down(VK_UP))     b |= XBOX_GAMEPAD_DPAD_UP;
    if (key_down(VK_DOWN))   b |= XBOX_GAMEPAD_DPAD_DOWN;
    if (key_down(VK_LEFT))   b |= XBOX_GAMEPAD_DPAD_LEFT;
    if (key_down(VK_RIGHT))  b |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (key_down(VK_RETURN)) b |= XBOX_GAMEPAD_START;
    if (key_down(VK_BACK))   b |= XBOX_GAMEPAD_BACK;
    if (key_down(VK_SHIFT))  b |= XBOX_GAMEPAD_LEFT_THUMB;
    if (key_down(VK_CONTROL))b |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = b;

    /* Analog on the console, so a key is 255 rather than a flag -- a title
     * that reads these as a pressure never sees a press if they are 1. */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A]        = key_down('Z') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B]        = key_down('X') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X]        = key_down('A') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y]        = key_down('S') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE]    = key_down('Q') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK]    = key_down('E') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = key_down('1') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = key_down('3') ? 255 : 0;

    /* W/A/S/D would collide with the face buttons above, so the left thumb
     * shares the arrow keys' row on the numeric pad instead. */
    pState->Gamepad.sThumbLX = axis_from_keys(VK_NUMPAD4, VK_NUMPAD6);
    pState->Gamepad.sThumbLY = axis_from_keys(VK_NUMPAD2, VK_NUMPAD8);
    pState->Gamepad.sThumbRX = axis_from_keys('J', 'L');
    pState->Gamepad.sThumbRY = axis_from_keys('K', 'I');

    /* The title's input layer looks for button edges, so the packet number
     * has to move whenever the state does or a press is never noticed. */
    pState->dwPacketNumber = ++packet;
}

/* ---- keyboard + mouse (RECOMP_KBM) ---------------------------------------
 *
 * A playing control scheme rather than the RECOMP_KEYBOARD bring-up probe:
 * WASD moves, the mouse aims, and the triggers land on the mouse buttons.
 * Enabled by RECOMP_KBM (set by the game's --kbm flag), port 0 only, and
 * merged on top of a pad like the probe so a controller keeps working.
 *
 * Mouse movement is read as a per-frame velocity: the right stick deflects in
 * proportion to how far the mouse moved since the last poll and centres the
 * moment it stops, which is how a stick-driven camera expects to be steered.
 * The window clips and recentres the cursor while this is on (see
 * xbox_FramebufferMouseCapture), so motion never runs into a screen edge.
 */
extern int xbox_FramebufferMouseDelta(int *dx, int *dy);
extern int xbox_FramebufferMouseButton(int which);
extern int xbox_FramebufferMouseWheel(void);

static BOOL kbm_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_KBM");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    return on ? TRUE : FALSE;
}

static BOOL overlay_enabled(void)
{
    return kbm_enabled() || keyboard_enabled();
}

static unsigned kbm_number(const char *name, unsigned maximum, unsigned fallback)
{
    const char *value = getenv(name);
    char *end;
    unsigned long number;
    if (!value || !*value) return fallback;
    number = strtoul(value, &end, 10);
    if (*end || number > maximum) return fallback;
    return (unsigned)number;
}

static SHORT kbm_clamp(long v)
{
    if (v > 32767) return 32767;
    if (v < -32767) return -32767;
    return (SHORT)v;
}

/* Expand a right-stick deflection through the guest's own deadzone.
 *
 * XReadGamepad (0x001B99C0) does, per axis: n = (|axis| - D) * S, and zero
 * when (|axis| - D) <= 0, with D a float at 0x3A947C (7696.0 here) and
 * S = 1/(32767 - D). So any axis below D is silently swallowed -- which is
 * what made precise aiming impossible: a slow move produced a stick value
 * under 7696 and the guest saw nothing.
 *
 * Emitting D + (|v|/32767)*(32767-D) cancels to an effective deflection of
 * exactly |v|/32767 after the guest's subtraction: linear, with no floor
 * step. v == 0 must stay 0, or the decaying tail would hold the axis just
 * above D and the camera would never stop. */
static SHORT kbm_deadzone_expand(float v, unsigned deadzone)
{
    float span = 32767.0f - (float)deadzone;
    float mag, out;
    if (v == 0.0f || span <= 0.0f) return kbm_clamp((long)v);
    mag = v < 0.0f ? -v : v;
    if (mag > 32767.0f) mag = 32767.0f;
    out = (float)deadzone + mag * (span / 32767.0f);
    return kbm_clamp((long)(v < 0.0f ? -out : out));
}

/* ---- direct camera look (RECOMP_KBM_LOOK_DIRECT) -------------------------
 *
 * The right stick's turn rate is capped by the game, so once the ball pins at
 * full deflection the camera stops speeding up however fast the mouse moves.
 * This bypasses the stick for look: each poll it adds the mouse delta straight
 * to the local camera's yaw/pitch fields.
 *
 * The camera is gameMultiplayer_GetFirstLocalCamera (0x00130C90):
 *   holder = *0x439DE8; camera = *(holder + 4)        (0 if no local player)
 * The live angles are tSimCamera yaw at +0x11C and pitch at +0x114, both in
 * degrees (tSimCamera::UpdateYaw 0x00057560 / UpdatePitch 0x00055450 drive
 * them; traced 0..360 and about -27..-4). tSimCamera::SetYaw/SetPitch
 * (0x000715E0/0x000715C0) instead write the desired aim at +0x20C/+0x204, and
 * the walk/aim path lerps the live angles toward a look target reached through
 * *(camera+0x188)+0x14/+0x10 -- so that target is turned too, or the lerp
 * pulls the camera straight back.
 *
 * Guest memory is a host mapping at va + xbox_GetMemoryOffset(); this runs on
 * the guest thread inside XInputGetState, the same thread the generated code
 * uses, so a plain volatile access matches it. */
extern ptrdiff_t xbox_GetMemoryOffset(void);

/* A plausible guest VA: below the device aperture and above the null page.
 * Camera-relative pointers (camera+0x188) are uninitialised (0xCCCCCCCC) while
 * the camera is being built on a load screen, so every deref is guarded. */
static int kbm_valid(uint32_t va)
{
    return va >= 0x00010000u && va < 0x80000000u;
}

static uint32_t kbm_u32(uint32_t va)
{
    return *(volatile uint32_t *)((uintptr_t)va + (uintptr_t)xbox_GetMemoryOffset());
}

static uint32_t kbm_camera(void)
{
    uint32_t holder = kbm_u32(0x439DE8u);
    uint32_t camera;
    if (!kbm_valid(holder)) return 0;
    camera = kbm_u32(holder + 4u);
    return kbm_valid(camera) ? camera : 0;
}

static float kbm_camf(uint32_t camera, uint32_t offset)
{
    return *(volatile float *)((uintptr_t)(camera + offset)
                               + (uintptr_t)xbox_GetMemoryOffset());
}

static void kbm_cam_setf(uint32_t camera, uint32_t offset, float v)
{
    *(volatile float *)((uintptr_t)(camera + offset)
                        + (uintptr_t)xbox_GetMemoryOffset()) = v;
}

/* Face/triggers, matching what the build actually does (not what the docs
 * guess): A is jump, B is use, X is melee, LT is crouch. So jump is on Space,
 * melee on F, crouch on Control. A pad is expected to be the primary device,
 * so this is the "there is no pad" scheme. Y is the right mouse button (no
 * key); left stick click is R; right stick click is the middle mouse button.
 * Override any key binding with
 *   RECOMP_KBM_MAP="A=Return,B=E,X=Space,BLACK=Shift,WHITE=G,LT=Control,START=Tab,BACK=Escape,LTHUMB=R" */
static int k_a = VK_SPACE, k_b = 'E', k_x = 'F', k_y = 0,
           k_black = VK_SHIFT, k_white = 'G', k_lt = VK_CONTROL,
           k_start = VK_TAB, k_back = VK_ESCAPE,
           k_lthumb = 'R', k_rthumb = 0;

struct kbm_bind { const char *name; int *vk; };
static struct kbm_bind kbm_binds[] = {
    { "A", &k_a }, { "B", &k_b }, { "X", &k_x }, { "Y", &k_y },
    { "BLACK", &k_black }, { "WHITE", &k_white }, { "LT", &k_lt },
    { "START", &k_start }, { "BACK", &k_back },
    { "LTHUMB", &k_lthumb }, { "RTHUMB", &k_rthumb },
};

static int kbm_vk_name(const char *name)
{
    if (!strcmp(name, "Return"))  return VK_RETURN;
    if (!strcmp(name, "Space"))   return VK_SPACE;
    if (!strcmp(name, "Tab"))     return VK_TAB;
    if (!strcmp(name, "Escape"))  return VK_ESCAPE;
    if (!strcmp(name, "Back"))    return VK_BACK;
    if (!strcmp(name, "Shift"))   return VK_SHIFT;
    if (!strcmp(name, "Control")) return VK_CONTROL;
    if (!strcmp(name, "Alt"))     return VK_MENU;
    if (name[0] && !name[1])      return (unsigned char)name[0];
    return 0;
}

static void kbm_load_map(void)
{
    char *spec = getenv("RECOMP_KBM_MAP");
    char *tok;
    if (!spec || !*spec) return;
    spec = strdup(spec);
    for (tok = strtok(spec, ","); tok; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        unsigned i;
        int vk;
        if (!eq) continue;
        *eq = '\0';
        vk = kbm_vk_name(eq + 1);
        if (!vk) continue;
        for (i = 0; i < sizeof(kbm_binds) / sizeof(kbm_binds[0]); i++)
            if (!strcmp(kbm_binds[i].name, tok)) *kbm_binds[i].vk = vk;
    }
    free(spec);
}

static void kbm_state(XBOX_INPUT_STATE *pState)
{
    static DWORD packet;
    static int configured;
    static unsigned deadzone, invert_y, maxdelta, stick_deadzone, sens, ratio;
    static unsigned look_direct, look_trace;
    static float look_gain, look_gain_y;
    static float dbg_last_pitch, dbg_last_yaw;
    static int dbg_have;
    /* Scroll -> left-stick alias: a notch is a single poll, which the title's
     * per-frame integration barely sees, so hold the deflection for a beat. */
    static int wheel_dir;
    static DWORD wheel_until;
    WORD b = 0;
    int dx, dy, wheel;

    if (!configured) {
        /* The right stick is just the mouse delta scaled straight through:
         * moving the mouse N counts deflects the stick by N/sens of full.
         * No ball, no spring, no friction, no acceleration -- the game already
         * turns the camera from the stick, so anything between the mouse and
         * the stick only adds lag. sens is the mouse counts for full
         * deflection (lower = faster); deadzone drops sub-count sensor noise.
         * stick_deadzone is the guest's own per-axis stick deadzone
         * (XReadGamepad subtracts it before scaling); output is expanded
         * through it so slow aim stays linear. */
        deadzone  = kbm_number("RECOMP_KBM_DEADZONE", 64, 1);
        stick_deadzone = kbm_number("RECOMP_KBM_STICK_DEADZONE", 32767, 7696);
        invert_y  = kbm_number("RECOMP_KBM_INVERT_Y", 1, 0);
        maxdelta  = kbm_number("RECOMP_KBM_MAXDELTA", 8192, 512);
        sens      = kbm_number("RECOMP_KBM_SENS", 32767, 300);
        /* per-axis output ratio (Y as a percent of X) */
        ratio     = kbm_number("RECOMP_KBM_RATIO", 1000, 100);
        /* Direct camera look: add the mouse delta straight to the camera's
         * yaw/pitch, bypassing the stick's turn-rate cap. GAIN is degrees per
         * mouse count (x1000); _Y is the pitch axis. TRACE logs the camera
         * each 200 ms and changes nothing. */
        look_direct = kbm_number("RECOMP_KBM_LOOK_DIRECT", 1, 1);
        look_trace  = kbm_number("RECOMP_KBM_LOOK_TRACE", 1, 0);
        look_gain   = (float)kbm_number("RECOMP_KBM_LOOK_GAIN",   1000000, 100) / 1000.0f;
        look_gain_y = (float)kbm_number("RECOMP_KBM_LOOK_GAIN_Y", 1000000, 100) / 1000.0f;
        kbm_load_map();
        configured = 1;
        fprintf(stderr, "[KBM] profile: deadzone=%u stick_deadzone=%u invert_y=%u maxdelta=%u "
                "sens=%u ratio=%u look_direct=%u look_gain=%.4f look_gain_y=%.4f "
                "A=%d B=%d X=%d Y=%d BLACK=%d WHITE=%d LT=%d START=%d BACK=%d "
                "LTHUMB=%d RTHUMB=%d\n",
                deadzone, stick_deadzone, invert_y, maxdelta, sens, ratio,
                look_direct, look_gain, look_gain_y,
                k_a, k_b, k_x, k_y, k_black, k_white, k_lt, k_start, k_back,
                k_lthumb, k_rthumb);
    }

    memset(pState, 0, sizeof(*pState));

    /* Left stick: WASD, full deflection. */
    pState->Gamepad.sThumbLX = axis_from_keys('A', 'D');
    pState->Gamepad.sThumbLY = axis_from_keys('S', 'W');

    /* Right stick: the mouse delta scaled straight to the stick. Clamp each
     * poll's delta so a focus change or a stuck cursor cannot slam it to full,
     * while real flicks still scale; the inlet deadzone drops sub-count noise. */
    xbox_FramebufferMouseDelta(&dx, &dy);
    if (dx >  (int)maxdelta) dx =  (int)maxdelta;
    if (dx < -(int)maxdelta) dx = -(int)maxdelta;
    if (dy >  (int)maxdelta) dy =  (int)maxdelta;
    if (dy < -(int)maxdelta) dy = -(int)maxdelta;
    if (dx > -(int)deadzone && dx < (int)deadzone) dx = 0;
    if (dy > -(int)deadzone && dy < (int)deadzone) dy = 0;

    {
        float scale = 32767.0f / (float)sens;
        float sy = (float)dy * scale * (float)ratio / 100.0f;
        if (!invert_y) sy = -sy;
        pState->Gamepad.sThumbRX = kbm_deadzone_expand((float)dx * scale, stick_deadzone);
        pState->Gamepad.sThumbRY = kbm_deadzone_expand(sy, stick_deadzone);
    }

    /* Direct camera look: the mouse delta IS the look input, added straight
     * to the local camera's yaw/pitch. No stick, no ball. The right stick is
     * zeroed so the game does not also turn. In the free-look branch the game
     * reads +0x11C/+0x114 directly, so writing them is enough; in the walk/aim
     * branch (UpdateYaw/UpdatePitch) it instead lerps them toward a look
     * target at *(camera+0x188)+0x14/+0x10, so that target is turned by the
     * same delta too and the camera ends the frame where the mouse put it. */
    if (look_direct) {
        uint32_t camera;
        pState->Gamepad.sThumbRX = 0;
        pState->Gamepad.sThumbRY = 0;
        camera = kbm_camera();
        if (camera) {
            float startP = kbm_camf(camera, 0x114u);
            float startY = kbm_camf(camera, 0x11Cu);
            float dY = -(float)dx * look_gain;
            float dP = (invert_y ? (float)dy : -(float)dy) * look_gain_y;
            float yaw = startY + dY;
            float pitch = startP + dP;
            uint32_t look = kbm_u32(camera + 0x188u);
            while (yaw >= 360.0f) yaw -= 360.0f;
            while (yaw <    0.0f) yaw += 360.0f;
            if (pitch >  89.0f) pitch =  89.0f;
            if (pitch < -89.0f) pitch = -89.0f;
            kbm_cam_setf(camera, 0x11Cu, yaw);
            kbm_cam_setf(camera, 0x114u, pitch);
            /* While moving, tSimCamera::UpdatePitch recenters +0x114 toward
             * (+0x124 + +0x104) = (0 + rest). Writing our pitch into the rest
             * field makes that recenter target *our* pitch, so vertical look
             * survives movement instead of snapping back to -10. */
            kbm_cam_setf(camera, 0x104u, pitch);
            /* the aim/desired fields the weapon path reads (SetYaw/SetPitch) */
            kbm_cam_setf(camera, 0x20Cu, yaw);
            kbm_cam_setf(camera, 0x204u, pitch);
            if (kbm_valid(look)) {
                float tY = kbm_camf(look, 0x14u) + dY;
                float tP = kbm_camf(look, 0x10u) + dP;
                while (tY >= 360.0f) tY -= 360.0f;
                while (tY <    0.0f) tY += 360.0f;
                if (tP >  89.0f) tP =  89.0f;
                if (tP < -89.0f) tP = -89.0f;
                kbm_cam_setf(look, 0x14u, tY);
                kbm_cam_setf(look, 0x10u, tP);
            }
            if (look_trace && dbg_have && (dx || dy))
                fprintf(stderr, "[PROBE] dy=%d startP=%+.3f prevSetP=%+.3f startY=%+.3f prevSetY=%+.3f\n",
                        dy, (double)startP, (double)dbg_last_pitch,
                        (double)startY, (double)dbg_last_yaw);
            dbg_last_pitch = pitch;
            dbg_last_yaw = yaw;
            dbg_have = 1;
        }
    }

    /* Diagnostic: hold a signed +-45 value in a configurable camera field,
     * alternating every 2 s, to identify which field actually drives the
     * rendered pitch. RECOMP_KBM_SWEEP=1 enables, RECOMP_KBM_SWEEP_OFF=<hex>
     * picks the offset (default 0x114). Logs nothing on its own. */
    {
        static int sweep = -1;
        static unsigned sweep_off;
        if (sweep < 0) {
            const char *v = getenv("RECOMP_KBM_SWEEP");
            const char *o = getenv("RECOMP_KBM_SWEEP_OFF");
            sweep = (v && *v && *v != '0') ? 1 : 0;
            sweep_off = o ? (unsigned)strtoul(o, NULL, 0) : 0x114u;
        }
        if (sweep) {
            uint32_t cam = kbm_camera();
            if (cam) {
                unsigned long t = (unsigned long)GetTickCount();
                float v = ((t / 2000u) & 1u) ? 45.0f : -45.0f;
                kbm_cam_setf(cam, sweep_off, v);
            }
        }
    }

    /* Trace the camera angles so the yaw/pitch offsets and signs can be
     * confirmed; logs nothing and changes nothing on its own. */
    if (look_trace) {
        static unsigned long next_look;
        unsigned long t = (unsigned long)GetTickCount();
        if ((long)(t - next_look) >= 0) {
            uint32_t camera = kbm_camera();
            fprintf(stderr, "[LOOK] cam=%08x dx=%d dy=%d\n", camera, dx, dy);
            if (camera) {
                unsigned o;
                uint32_t look = kbm_u32(camera + 0x188u);
                uint32_t thing = kbm_u32(camera + 0x168u);
                if (!kbm_valid(look)) look = 0;
                if (!kbm_valid(thing)) thing = 0;
                for (o = 0x100u; o <= 0x140u; o += 4u)
                    fprintf(stderr, "  +%03x %+12.5f\n", o,
                            (double)kbm_camf(camera, o));
                fprintf(stderr, "  look=%08x rateP=%+.4f rateY=%+.4f tgtP=%+.4f tgtY=%+.4f\n",
                        look,
                        (double)kbm_camf(camera, 0x58u),
                        (double)kbm_camf(camera, 0x60u),
                        look ? (double)kbm_camf(look, 0x10u) : 0.0,
                        look ? (double)kbm_camf(look, 0x14u) : 0.0);
                fprintf(stderr, "  aimP=%+.4f aimY=%+.4f thing=%08x lx=%+.4f ly=%+.4f lz=%+.4f\n",
                        (double)kbm_camf(camera, 0x204u),
                        (double)kbm_camf(camera, 0x20Cu),
                        thing,
                        thing ? (double)kbm_camf(thing, 0x90u) : 0.0,
                        thing ? (double)kbm_camf(thing, 0x94u) : 0.0,
                        thing ? (double)kbm_camf(thing, 0x98u) : 0.0);
                {
                    uint32_t holder = kbm_u32(0x439DE8u);
                    uint32_t player = (kbm_valid(holder)) ? kbm_u32(holder) : 0;
                    uint32_t wa = 0, wb = 0, walkCam = 0;
                    if (kbm_valid(player)) {
                        wa = kbm_u32(player + 0x180u);
                        if (kbm_valid(wa)) {
                            wb = kbm_u32(wa + 0x520u);
                            if (kbm_valid(wb)) walkCam = kbm_u32(wb + 4u);
                        }
                    }
                    fprintf(stderr, "  walk: holder=%08x player=%08x wa=%08x wb=%08x walkCam=%08x same=%d\n",
                            holder, player, wa, wb, walkCam,
                            walkCam && walkCam == camera);
                }
                for (o = 0x00u; o <= 0x40u; o += 4u)
                    fprintf(stderr, "  m%02x %+12.5f\n", o,
                            (double)kbm_camf(camera, o));
                for (o = 0x1A0u; o <= 0x210u; o += 4u)
                    fprintf(stderr, "  p%03x %+12.5f\n", o,
                            (double)kbm_camf(camera, o));
                fprintf(stderr, "  lookPos=%+.4f %+.4f %+.4f eye=%+.4f %+.4f %+.4f dist=%+.4f pitchoff=%+.4f\n",
                        (double)kbm_camf(camera, 0x74u),
                        (double)kbm_camf(camera, 0x78u),
                        (double)kbm_camf(camera, 0x7Cu),
                        (double)kbm_camf(camera, 0x210u),
                        (double)kbm_camf(camera, 0x214u),
                        (double)kbm_camf(camera, 0x218u),
                        (double)kbm_camf(camera, 0xF8u),
                        (double)kbm_camf(camera, 0xF4u));
                fprintf(stderr, "  tgt170=%+.4f %+.4f %+.4f c118=%+.4f c120=%+.4f c158=%+.4f c1A4=%+.4f c1A8=%+.4f\n",
                        (double)kbm_camf(camera, 0x170u),
                        (double)kbm_camf(camera, 0x174u),
                        (double)kbm_camf(camera, 0x178u),
                        (double)kbm_camf(camera, 0x118u),
                        (double)kbm_camf(camera, 0x120u),
                        (double)kbm_camf(camera, 0x158u),
                        (double)kbm_camf(camera, 0x1A4u),
                        (double)kbm_camf(camera, 0x1A8u));
                for (o = 0x240u; o <= 0x27Cu; o += 4u)
                    fprintf(stderr, "  M%03x %+12.5f\n", o,
                            (double)kbm_camf(camera, o));
            }
            next_look = t + 200;
        }
    }
    {
        static int trace = -1;
        static unsigned long next_log;
        unsigned long t = (unsigned long)GetTickCount();
        if (trace < 0) {
            const char *v = getenv("RECOMP_KBM_TRACE");
            trace = (v && *v && *v != '0') ? 1 : 0;
        }
        if (trace && (long)(t - next_log) >= 0) {
            fprintf(stderr, "[KBM] dx=%d dy=%d out=(%d,%d)\n",
                    dx, dy,
                    pState->Gamepad.sThumbRX, pState->Gamepad.sThumbRY);
            next_log = t + 200;
        }
    }

    /* Digital: d-pad on the arrows (and 1-4); Start/Back from the map. */
    if (key_down(VK_UP)    || key_down('1')) b |= XBOX_GAMEPAD_DPAD_UP;
    if (key_down(VK_DOWN)  || key_down('2')) b |= XBOX_GAMEPAD_DPAD_DOWN;
    if (key_down(VK_LEFT)  || key_down('3')) b |= XBOX_GAMEPAD_DPAD_LEFT;
    if (key_down(VK_RIGHT) || key_down('4')) b |= XBOX_GAMEPAD_DPAD_RIGHT;
    wheel = xbox_FramebufferMouseWheel();
    if (wheel > 0) b |= XBOX_GAMEPAD_DPAD_UP;
    if (wheel < 0) b |= XBOX_GAMEPAD_DPAD_DOWN;
    /* Scroll also aliases the left stick's up/down (forward/back). */
    if (wheel > 0)      { wheel_dir =  1; wheel_until = GetTickCount() + 150; }
    else if (wheel < 0) { wheel_dir = -1; wheel_until = GetTickCount() + 150; }
    if (wheel_dir && (long)(GetTickCount() - wheel_until) < 0)
        pState->Gamepad.sThumbLY = wheel_dir > 0 ? 32767 : -32767;
    if (key_down(k_start)) b |= XBOX_GAMEPAD_START;
    /* BackSpace stays a Back alias regardless of the configured Back key. */
    if (key_down(k_back) || key_down(VK_BACK)) b |= XBOX_GAMEPAD_BACK;
    /* Stick clicks: left on a key, right on the middle mouse button. */
    if (key_down(k_lthumb)) b |= XBOX_GAMEPAD_LEFT_THUMB;
    if (key_down(k_rthumb) || xbox_FramebufferMouseButton(2))
        b |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = b;

    /* Analog face buttons and triggers. Y is the right mouse button; White
     * (the old right-mouse action) is on G; crouch lives on the LT button,
     * which the map puts on Control. */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A]     = key_down(k_a) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B]     = key_down(k_b) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X]     = key_down(k_x) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        (key_down(k_y) || xbox_FramebufferMouseButton(1)) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] = key_down(k_black) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] = key_down(k_white) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        key_down(k_lt) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        xbox_FramebufferMouseButton(0) ? 255 : 0;

    pState->dwPacketNumber = ++packet;
}

static DWORD poll_host(DWORD port, XINPUT_STATE *state, DWORD *host_slot)
{
    DWORD connected = 0;
    if (g_host_slot >= 0 && port == 0) {
        DWORD result = XInputGetState((DWORD)g_host_slot, state);
        LONG selected = result == ERROR_SUCCESS ? g_host_slot : -1;
        LONG previous = InterlockedExchange(&g_reported_slot[port], selected);
        if (selected >= 0 && previous != selected)
            fprintf(stderr, "[XINPUT] selected host slot %d -> Xbox port 0\n", g_host_slot);
        if (result != ERROR_SUCCESS && result != ERROR_DEVICE_NOT_CONNECTED)
            fprintf(stderr, "[XINPUT] selected slot query failed: %lu\n", result);
        *host_slot = (DWORD)g_host_slot;
        return result;
    }
    DWORD wanted = g_host_slot >= 0 ? port - 1 : port;
    for (DWORD slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++) {
        if ((int)slot == g_host_slot) continue;
        DWORD result = XInputGetState(slot, state);
        if (result != ERROR_SUCCESS) {
            if (result != ERROR_DEVICE_NOT_CONNECTED) {
                fprintf(stderr, "[XINPUT] slot %lu query failed: %lu\n", slot, result);
                return result;
            }
            continue;
        }
        if (connected++ == wanted) {
            LONG previous = InterlockedExchange(&g_reported_slot[port], (LONG)slot);
            if (previous != (LONG)slot)
                fprintf(stderr, "[XINPUT] host slot %lu -> Xbox port %lu\n", slot, port);
            *host_slot = slot;
            return ERROR_SUCCESS;
        }
    }
    if (InterlockedExchange(&g_reported_slot[port], -1) != -1)
        fprintf(stderr, "[XINPUT] Xbox port %lu disconnected\n", port);
    return ERROR_DEVICE_NOT_CONNECTED;
}

void xbox_InputInit(void)
{
    input_configure();
    XINPUT_STATE state;
    DWORD slot;
    for (DWORD port = 0; port < XBOX_MAX_CONTROLLERS; port++)
        poll_host(port, &state, &slot);
    fprintf(stderr, "[XINPUT] live controller discovery enabled\n");
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XINPUT_STATE xi_state;
    DWORD result, host_slot;

    /* RECOMP_POLL_TRACE: how often the title actually asks for input. A menu
     * that renders at 30 FPS but polls the pad at 1 Hz is the difference
     * between "laggy" and "broken", and nothing else in the chain shows it. */
    {
        static int trace = -1;
        static unsigned long last;
        static unsigned calls;
        if (trace < 0)
            trace = getenv("RECOMP_POLL_TRACE") != NULL;
        if (trace) {
            unsigned long now = GetTickCount();
            calls++;
            if (now - last >= 1000) {
                fprintf(stderr, "  [POLL] xbox_InputGetState %u/s\n", calls);
                fflush(stderr);
                last = now;
                calls = 0;
            }
        }
    }

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = poll_host(dwPort, &xi_state, &host_slot);
    if (result != ERROR_SUCCESS) {
        if (dwPort == 0 && overlay_enabled()) {
            if (kbm_enabled()) kbm_state(pState);
            else               keyboard_state(pState);
            return ERROR_SUCCESS;
        }
        return result;
    }

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = xi_state.dwPacketNumber;
    for (unsigned target = 0; target < 16; target++) {
        BYTE pressure = source_pressure(&xi_state.Gamepad, g_button_map[target]);
        if (target < 8) {
            if (pressure > g_button_threshold)
                pState->Gamepad.wButtons |= (WORD)(1u << target);
        } else pState->Gamepad.bAnalogButtons[target - 8] = pressure;
    }
    const XINPUT_GAMEPAD *pad = &xi_state.Gamepad;
    map_stick(g_axis_flags & 1 ? pad->sThumbRX : pad->sThumbLX,
              g_axis_flags & 1 ? pad->sThumbRY : pad->sThumbLY, 0,
              &pState->Gamepad.sThumbLX, &pState->Gamepad.sThumbLY);
    map_stick(g_axis_flags & 1 ? pad->sThumbLX : pad->sThumbRX,
              g_axis_flags & 1 ? pad->sThumbLY : pad->sThumbRY, 1,
              &pState->Gamepad.sThumbRX, &pState->Gamepad.sThumbRY);

    /* Merge the keyboard on top rather than only standing in for a missing
     * pad.
     *
     * The first version put the keyboard behind XInput's failure, on the
     * assumption that with nothing plugged in the call would fail. It does
     * not: under Wine XInputGetState returns ERROR_SUCCESS and a gamepad
     * with every button at rest, so the fallback was unreachable and
     * pressing a key did nothing at all.
     *
     * Merging is also the better rule. A real pad keeps working -- its
     * buttons are already in pState and the keyboard only adds to them --
     * and there is no special case left to get wrong. */
    if (dwPort == 0 && overlay_enabled()) {
        XBOX_INPUT_STATE kb;
        int i;
        if (kbm_enabled()) kbm_state(&kb);
        else               keyboard_state(&kb);
        pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
        for (i = 0; i < 8; i++)
            if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
                pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
        if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
        if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
        if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
        if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
        /* The input layer records edges, so an unchanged packet number is
         * read as the same state and the press never happens. */
        pState->dwPacketNumber = kb.dwPacketNumber;
    }

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;
    XINPUT_STATE state;
    DWORD slot, result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = poll_host(dwPort, &state, &slot);
    if (result != ERROR_SUCCESS) return result;
    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    return XInputSetState(slot, &xi_vib);
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    XINPUT_STATE state;
    DWORD slot;
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return poll_host(dwPort, &state, &slot) == ERROR_SUCCESS ||
        (dwPort == 0 && overlay_enabled());
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    XINPUT_STATE state;
    DWORD result, slot;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = poll_host(dwPort, &state, &slot);
    if (result != ERROR_SUCCESS) return result;
    result = XInputGetCapabilities(slot, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS) return result;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

/* Open up to XBOX_MAX_CONTROLLERS attached game controllers. */
static void open_controllers(void)
{
    int slot = 0;
    for (int i = 0; i < SDL_NumJoysticks() && slot < XBOX_MAX_CONTROLLERS; i++) {
        if (!SDL_IsGameController(i))
            continue;
        if (!g_pads[slot]) {
            g_pads[slot] = SDL_GameControllerOpen(i);
            g_controller_connected[slot] = (g_pads[slot] != NULL);
        }
        slot++;
    }
}

void xbox_InputInit(void)
{
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    open_controllers();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c || !SDL_GameControllerGetAttached(c)) {
        g_controller_connected[dwPort] = FALSE;
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    SDL_GameControllerUpdate();
    g_controller_connected[dwPort] = TRUE;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    WORD btn = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    pState->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    pState->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c) return ERROR_DEVICE_NOT_CONNECTED;

    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). */
    SDL_GameControllerRumble(c, pVibration->wLeftMotorSpeed,
                             pVibration->wRightMotorSpeed, 1000);
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_pads[dwPort])
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
