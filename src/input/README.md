# xbox_input — Xbox Gamepad to XInput

Maps the Xbox controller API to Windows XInput. The original Xbox used `XInputGetState` with a slightly different structure layout than the XInput API on Windows. This layer translates between them.

## Ghost's hardware input path

Ghost runs its statically linked Xbox XAPI driver, rather than calling these
host wrappers directly. Its startup initializes the host input layer and the
OHCI model after the kernel bridge. Its fault handler routes USB-register
accesses to OHCI. The path is:

```text
guest XAPI -> OHCI registers/interrupts -> Controller S XID report -> host XInput
```

OHCI resolves descriptor/HCCA metadata through the contiguous-memory window.
Payloads can also occupy ordinary low RAM: `MmGetPhysicalAddress` records
translation provenance, and the DMA resolver selects the corresponding bank.
The banks are separate storage, not aliases. This is a bring-up translation
model, not a general solution for overlapping physical offsets.

Ghost's USB class registry contains callbacks that seeded function discovery
missed. `Ghost/tools/recover_usb_callbacks.py` translates eight bounded original
SDK functions into a separate generated file and registers them through the
manual dispatch. These are real initialization/AddDevice/RemoveDevice bodies
and XID report parsers, not replacements that fabricate connected-device state.
The report parsers at `0x0035D763` and `0x0035D7B5` are referenced by the XID
type registry, not the USB class registry. Their external tail jumps preserve
the caller's return stack; recovery permits only their verified existing
target `0x001B858A`, rather than arbitrary branches outside the recovered body.
The user confirmed working physical-controller input after this repair.

On Windows, Xbox ports enumerate the currently connected native XInput slots
in ascending order. A single USB Xbox One controller in native slot 1, 2 or 3
therefore supplies Xbox port 0. Every poll rediscovers connected devices;
capabilities, connectivity and direct API vibration use the same selection.
Adding/removing a controller can renumber this compact port list. The OHCI
model currently exposes one virtual gamepad, not four independent USB pads.

Ghost defaults to `RECOMP_USB=1` and `RECOMP_USB_NDP=2` (root-hub ports, not
controller count). `RECOMP_USB_PADS` was not consumed by the OHCI model.
For diagnostics, `RECOMP_USB_TRACE=1` traces enumeration and
`RECOMP_INPUT_DIAG=1` reports host-to-XID button delivery. With no physical
controller, `RECOMP_KEYBOARD=1` maps Enter to Start; this is a guest-path probe,
not proof of physical XInput acceptance.
The input diagnostic prints the actual keyboard environment value, including
`0`, rather than treating the presence of the variable as enablement.

## Windows launcher and remapping

From the workspace root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Ghost\launch.ps1
```

The WinForms launcher displays live XInput connections, selects an automatic
or fixed native slot, remaps all sixteen Xbox buttons, and configures radial
deadzones, stick swapping/inversion, and optional keyboard fallback. Menu maps
to Start and View to Back. Its bundled Xbox One profile uses RB for Black and
LB for White; change those dropdowns to reproduce the legacy mapping below.
Face buttons/bumpers provide pressure 0 or 255, while triggers retain 0-255.
There is no way to recover pressure-sensitive face buttons from Xbox One hardware.

The bundled profile is `Ghost/input-profile.json`. Save writes a per-user
profile to `%LOCALAPPDATA%\XboxRecomp\Ghost\input-profile.json`, not the bundled
file. `-ConfigPath` selects another profile/save destination; `-ExecutablePath`
selects another build. `-ValidateOnly` validates the JSON and prints the child
environment without launching. `-NoUI` launches with the same profile, and
`-Wait` additionally waits and returns the game's exit code. The launcher
disables the diagnostic watchdog unless the parent explicitly supplied it.

Settings are parsed once by `xbox_InputInit`, shared by the direct and USB
input paths, and rejected explicitly when malformed:

| Environment | Meaning / default |
|-------------|-------------------|
| `RECOMP_XINPUT_SLOT` | `auto` compacts connected slots; `0`-`3` reserves that native slot for Xbox port zero. Other ports exclude the reserved slot. |
| `RECOMP_INPUT_MAP` | Comma-separated `TARGET=SOURCE` overrides; unspecified targets keep their legacy bindings. |
| `RECOMP_INPUT_THRESHOLD` | Digital targets activate above this source pressure; integer 0-254, default 30. |
| `RECOMP_INPUT_DEADZONE_LEFT`, `RECOMP_INPUT_DEADZONE_RIGHT` | Radial center deadzones, 0-32767; default zero. Outside the deadzone, values are not rescaled. |
| `RECOMP_INPUT_AXES` | Bitmask: 1 swaps sticks; 2/4 invert left X/Y; 8/16 invert right X/Y. Default zero; applied to destination sticks. |
| `RECOMP_KEYBOARD` | Optional fixed Xbox keyboard overlay on port zero; disabled by default and not controller-remapped. |

Targets: `UP,DOWN,LEFT,RIGHT,START,BACK,LCLICK,RCLICK,A,B,X,Y,BLACK,WHITE,LT,RT`.
Sources use those names except that native bumpers are `LB,RB`, not
`BLACK,WHITE`; `NONE` disables a binding. Triggers mapped to a digital target
use the threshold, while analog targets retain their source pressure.
Duplicate/unknown bindings, empty entries and out-of-range numbers fail
startup rather than silently substituting defaults.

## Files

| File | LOC | Purpose |
|------|-----|---------|
| `xinput_xbox.h` | 189 | Public header — types, button constants, function prototypes |
| `xinput_device.c` | 23 | Implementation (maps Xbox calls to Windows XInput) |

## Quick Start

```c
#include "xinput_xbox.h"

// Initialize input system
xbox_InputInit();

// Poll controller state (port 0-3)
XBOX_INPUT_STATE state;
if (xbox_InputGetState(0, &state) == 0) {
    // Digital buttons
    if (state.Gamepad.wButtons & XBOX_GAMEPAD_START)
        pause_game();

    // Analog buttons (0-255 pressure)
    uint8_t trigger_r = state.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER];
    if (trigger_r > XBOX_ANALOG_BUTTON_THRESHOLD)
        accelerate(trigger_r / 255.0f);

    // Stick axes (-32768 to +32767)
    float steer = state.Gamepad.sThumbLX / 32768.0f;
}

// Vibration feedback
XBOX_VIBRATION vib = { .wLeftMotorSpeed = 32000, .wRightMotorSpeed = 16000 };
xbox_InputSetState(0, &vib);
```

## API

```c
// Initialize (call once at startup)
void xbox_InputInit(void);

// Poll controller state (returns 0 on success, non-zero if disconnected)
DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState);

// Set vibration motors
DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration);

// Check if controller is connected
BOOL xbox_InputIsConnected(DWORD dwPort);

// Query controller capabilities
DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps);
```

## Types

```c
typedef struct {
    WORD  wButtons;                     // Digital button bitmask
    BYTE  bAnalogButtons[8];            // Analog button pressure (0-255)
    SHORT sThumbLX, sThumbLY;           // Left stick (-32768 to +32767)
    SHORT sThumbRX, sThumbRY;           // Right stick
} XBOX_GAMEPAD;

typedef struct {
    DWORD dwPacketNumber;               // Increments on state change
    XBOX_GAMEPAD Gamepad;
} XBOX_INPUT_STATE;

typedef struct {
    WORD wLeftMotorSpeed;               // 0-65535
    WORD wRightMotorSpeed;              // 0-65535
} XBOX_VIBRATION;
```

## Button Constants

### Digital Buttons (wButtons bitmask)

```c
XBOX_GAMEPAD_DPAD_UP         0x0001
XBOX_GAMEPAD_DPAD_DOWN       0x0002
XBOX_GAMEPAD_DPAD_LEFT       0x0004
XBOX_GAMEPAD_DPAD_RIGHT      0x0008
XBOX_GAMEPAD_START            0x0010
XBOX_GAMEPAD_BACK             0x0020
XBOX_GAMEPAD_LEFT_THUMB       0x0040    // Left stick click
XBOX_GAMEPAD_RIGHT_THUMB      0x0080    // Right stick click
```

### Analog Buttons (bAnalogButtons[] indices)

The original Xbox had pressure-sensitive face buttons (0-255):

```c
XBOX_BUTTON_A          0    // Also used for "boost" in racing games
XBOX_BUTTON_B          1
XBOX_BUTTON_X          2
XBOX_BUTTON_Y          3
XBOX_BUTTON_BLACK      4    // No equivalent on modern controllers
XBOX_BUTTON_WHITE      5    // No equivalent on modern controllers
XBOX_BUTTON_LTRIGGER   6    // Left trigger
XBOX_BUTTON_RTRIGGER   7    // Right trigger

XBOX_ANALOG_BUTTON_THRESHOLD  30   // Recommended press threshold
```

### Legacy direct-executable mapping (without profile overrides)

| Xbox Button | XInput Equivalent | Notes |
|-------------|------------------|-------|
| A | A | Green button |
| B | B | Red button |
| X | X | Blue button |
| Y | Y | Yellow button |
| Black | Left Bumper | Mapped to LB |
| White | Right Bumper | Mapped to RB |
| L Trigger | Left Trigger | Analog 0-255 |
| R Trigger | Right Trigger | Analog 0-255 |
| Start | Start/Menu | |
| Back | Back/View | |
| D-pad | D-pad | Digital only |
| L Stick | L Stick | Click = L3 |
| R Stick | R Stick | Click = R3 |

## Ports

```c
#define XBOX_MAX_CONTROLLERS  4   // Ports 0-3
```

The Xbox supports 4 controllers. Each port can have a controller with optional memory units and other accessories. This layer only handles the gamepad; memory unit emulation is not needed for recompiled games.
