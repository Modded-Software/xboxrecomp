# xbox_input — Xbox Gamepad to XInput

Maps the Xbox controller API to Windows XInput. The original Xbox used `XInputGetState` with a slightly different structure layout than the XInput API on Windows. This layer translates between them.

## Hardware Input Integration

Games with statically linked Xbox XAPI drivers can access input through the
OHCI model rather than calling the host wrappers directly. Initialize the host
input layer and OHCI after the kernel bridge, and route trapped USB-register
accesses through the fault handler:

```text
guest XAPI -> OHCI registers/interrupts -> Controller S XID report -> host XInput
```

OHCI resolves descriptor/HCCA metadata through the contiguous-memory window.
Payloads can also occupy ordinary low RAM: `MmGetPhysicalAddress` records
translation provenance, and the DMA resolver selects the corresponding bank.
The banks are separate storage, not aliases. This is a bring-up translation
model, not a general solution for overlapping physical offsets.

Seeded function discovery can miss callbacks referenced by USB class and XID
type registries. Ensure initialization, AddDevice/RemoveDevice and report-parser
functions are translated and reachable through dispatch. If callback recovery
is needed, validate registry membership, instruction boundaries and external
tail-call targets. Preserve the caller's return stack rather than substituting
connected-device or input-processing stubs.

On Windows, Xbox ports enumerate the currently connected native XInput slots
in ascending order. A single USB Xbox One controller in native slot 1, 2 or 3
therefore supplies Xbox port 0. Every poll rediscovers connected devices;
capabilities, connectivity and direct API vibration use the same selection.
Adding/removing a controller can renumber this compact port list. The OHCI
model currently exposes one virtual gamepad, not four independent USB pads.

Use `RECOMP_USB=1` to enable the USB path. `RECOMP_USB_NDP` configures root-hub
ports, not controller count. `RECOMP_USB_PADS` is not consumed by the OHCI model.
For diagnostics, `RECOMP_USB_TRACE=1` traces enumeration and
`RECOMP_INPUT_DIAG=1` reports host-to-XID button delivery. With no physical
controller, `RECOMP_KEYBOARD=1` maps Enter to Start; this is a guest-path probe,
not proof of physical XInput acceptance.
The input diagnostic prints the actual keyboard environment value, including
`0`, rather than treating the presence of the variable as enablement.

## Input Configuration

Environment settings select automatic or fixed native slots, remap Xbox
buttons, configure radial deadzones and stick transforms, and enable optional
keyboard input. Set them before starting the game:

```powershell
$env:RECOMP_XINPUT_SLOT = 'auto'
$env:RECOMP_INPUT_MAP = 'BLACK=RB,WHITE=LB'
$env:RECOMP_INPUT_DEADZONE_LEFT = '8000'
```

The example swaps the legacy Black/White bumper bindings. Menu maps to Start
and View to Back. Face buttons and bumpers provide pressure 0 or 255, while
triggers retain 0-255. Xbox One hardware cannot reproduce pressure-sensitive
face buttons.

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
