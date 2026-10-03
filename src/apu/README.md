# xbox_apu — MCPX APU Audio Emulation

Hardware emulation of the Xbox's MCPX APU (Audio Processing Unit), extracted from [xemu](https://github.com/xemu-project/xemu). The APU is a custom chip with three processors:

- **VP (Voice Processor)** — 256 hardware voices, ADPCM/PCM decode, pitch shifting, envelopes, HRTF 3D audio
- **GP (Global Processor)** — DSP for global effects (reverb, chorus). Currently **stubbed** — effects bypass.
- **EP (Encode Processor)** — DSP for AC3/DTS encoding. Currently **stubbed** — passthrough.

Audio output uses Windows XAudio2, with `waveOut` as a fallback, at 48kHz
stereo 16-bit.

## Output and DMA Integration

For separate memory banks, use `mcpx_apu_init_standalone_mapped` with the
contiguous-bank base and `xbox_DmaPhysicalPointer`. Voice/descriptor allocations
normally occupy that bank, but payloads can occupy ordinary low RAM. `MmGetPhysicalAddress` records
page provenance so the resolver selects the correct, separate storage for
physical reads/writes, streaming ADPCM copies and diagnostic acknowledgements.
Invalid or mixed-bank extents are reported instead of reading unrelated data.
This provenance model does not implement general physical aliasing when both
banks reuse the same numeric offset.

The legacy `mcpx_apu_init_standalone` constructor retains flat-RAM behavior
for hosts with a single linear physical bank. Other integrations must supply
their actual physical backing or a mapper, not an unrelated virtual mapping.

The VP produces 32 samples per tick. Eight DSP slices assemble one 256-frame
stereo packet, submitted once every 5.33ms. The monitor preserves that packet
and mixes software voices into it; it must not clear hardware output or
generate 1024/2048 replacement frames each tick. Muting clears the packet,
and the inactive-hardware path clears stale VP data before software mixing.

XAudio2 queue saturation applies backpressure while releasing the APU lock.
The waveOut fallback also waits before reusing an in-flight buffer. Submission
failures and two-second queue stalls are reported and stop host output,
rather than silently dropping packets or overwriting a playing buffer.
Windows requests 1ms timer resolution for this audio cadence and balances
the request on monitor shutdown.

Set `RECOMP_APU_DIAG=1` to log accepted output blocks, stereo frames, nonzero
scalar samples, nonzero hardware samples, peak amplitude, test-tone and mute
state. `RECOMP_APU_TRACE=1` traces MMIO. Nonzero samples alone do not establish
correct DMA mapping, effects fidelity or a complete soundtrack.
GP/EP remain passthrough stubs; the AC97-ready/DSP-ack bring-up overrides are
not DSP56300 emulation.

### Diagnostic DSP Command Completion

`RECOMP_APU_DSP_ACK` accepts comma-separated aligned physical addresses,
`gp:<byte offset>` or `ep:<byte offset>`. The processor-relative forms resolve
through the programmed scratch scatter/gather table (`GPSADDR`/`EPSADDR`),
with its SGE limit checked before accessing the selected page. These registers
point to tables, not directly to the command buffer.

An unset or empty `RECOMP_APU_DSP_ACK` disables acknowledgement. Command offsets are
title-specific; do not assume a universal mailbox address. Fixed guest
addresses can become stale when allocation order changes, while SG-relative
addressing follows the programmed allocation.

Diagnostic command acknowledgement runs in the regular APU frame loop after
throttling, independently of voice-front-end execution. Gating it on voice
processing can deadlock a guest audio worker polling the mailbox while holding
a lock after voices stop. Emulator pause still suspends frame servicing.
Acknowledgement scheduling does not add DSP instruction execution.

This mode explicitly logs **diagnostic passthrough** and clears command words
without executing their DSP programs. It does not implement effects or prove
correct command results. Validate sustained output, DMA mapping and DSP behavior
separately; audible samples alone do not establish complete audio compatibility.

## Files

| File | LOC | Purpose |
|------|-----|---------|
| `apu.h` | 70 | **Public API** — init, shutdown, MMIO, mixer voices |
| `apu_state.h` | 525 | APU state struct (VP, GP, EP, DSP core, 256 voices) |
| `apu_regs.h` | 366 | MCPX APU register definitions and field masks |
| `apu_core.c` | 762 | MMIO handlers, frame thread, waveOut output, test tone |
| `apu_vp.c` | 1,247 | Voice Processor — decode, resample, mix, envelopes, HRTF |
| `apu_dsp.c` | 90 | GP/EP stubs (mixbin 0/1 passthrough) |
| `apu_mmio_hook.c` | 253 | VEH x86-64 instruction decoder for 0xFE800000+ MMIO |
| `apu_shim.h` | 431 | QEMU compatibility (threads, atomics, clocks → Win32) |
| `apu_debug.h` | 104 | Debug logging macros |
| `fpconv.h` | 70 | IEEE 754 float/int conversion utilities |

## Quick Start

```c
#include "apu.h"

// Initialize APU with pointer to Xbox RAM
// (ram_ptr = native pointer to Xbox VA 0x00000000)
MCPXAPUState *apu = mcpx_apu_init_standalone(ram_ptr);

// Test: play a 440Hz sine tone
mcpx_apu_play_test_tone(apu);

// --- OR use the software mixer for direct PCM playback ---

// Allocate a mixer voice
int voice = apu_mixer_alloc_voice();
APUMixerVoice *v = apu_mixer_get_voice(voice);

// Configure it
v->pcm_data = my_pcm_buffer;     // 16-bit signed PCM
v->pcm_bytes = buffer_size;
v->num_channels = 2;              // 1=mono, 2=stereo
v->sample_rate = 44100;           // Source sample rate
v->volume = 0.8f;                 // 0.0 - 1.0

// Play
apu_mixer_play(voice, 1);  // 1 = looping

// Stop and free
apu_mixer_stop(voice);
apu_mixer_free_voice(voice);

// Shutdown
mcpx_apu_shutdown(apu);
```

## Architecture

```
┌─────────────────────────────────────────────────┐
│                    Xbox Game                     │
│  DirectSound API calls (IDirectSound8)           │
└────────────────────┬────────────────────────────┘
                     │ writes to MMIO registers
                     ▼
┌─────────────────────────────────────────────────┐
│              APU Register Space                  │
│  0xFE800000 - 0xFE87FFFF (512 KB)               │
│  Intercepted via VEH (apu_mmio_hook.c)           │
└────────┬───────────┬───────────┬────────────────┘
         │           │           │
         ▼           ▼           ▼
   ┌──────────┐ ┌─────────┐ ┌─────────┐
   │    VP    │ │   GP    │ │   EP    │
   │256 voices│ │  (stub) │ │  (stub) │
   │ ADPCM/  │ │  effects│ │  encode │
   │  PCM    │ │  bypass │ │  bypass │
   └────┬─────┘ └────┬────┘ └────┬────┘
        │             │           │
        └──────┬──────┘───────────┘
               │ mixed samples
               ▼
   ┌──────────────────────────────┐
   │ XAudio2 / waveOut, 48kHz     │
   │ 256-frame stereo packets    │
   └──────────────────────────────┘
```

## Two Audio Paths

### 1. Hardware APU Emulation (MMIO)

For games that program the APU directly. The game writes to MMIO registers at 0xFE800000+, which are intercepted by the VEH hook and routed to the APU emulation:

```c
// Game writes: voice 5, set buffer address
*(uint32_t*)(0xFE802000 + 5 * voice_stride + VOICE_BUF_OFFSET) = buffer_addr;

// VEH intercepts → mcpx_apu_mmio_write() → VP processes voice
```

The VP runs in a separate thread, mixing active voices at the hardware frame rate.

### 2. Software Mixer (Direct API)

For games where you've decoded the audio format yourself and just want to play PCM. The mixer provides 64 voice slots with:

- 16-bit signed PCM input (mono or stereo)
- 16.16 fixed-point resampling (any rate → 48kHz)
- Per-voice volume control
- Looping support

```c
int apu_mixer_alloc_voice(void);                    // Returns slot 0-63, or -1
void apu_mixer_free_voice(int slot);
APUMixerVoice *apu_mixer_get_voice(int slot);       // Configure voice params
void apu_mixer_play(int slot, int looping);          // Start playback
void apu_mixer_stop(int slot);                       // Stop playback
```

## MMIO Hook

The APU MMIO hook (`apu_mmio_hook.c`) uses Windows Vectored Exception Handling to intercept memory accesses to the APU register range:

```
Access to 0xFE800000+
  → Page fault (no physical memory there)
  → VEH handler catches EXCEPTION_ACCESS_VIOLATION
  → Decodes the faulting x86-64 instruction (MOV, ADD, XOR, etc.)
  → Routes to mcpx_apu_mmio_read() or mcpx_apu_mmio_write()
  → Advances RIP past the instruction
  → Resumes execution
```

This allows recompiled code to access APU registers using normal memory operations, exactly as the original Xbox code did.

## Voice Processor Details

The VP processes 256 hardware voices per frame:

- **ADPCM decode**: Xbox ADPCM (4-bit, 64-sample blocks)
- **PCM formats**: 8-bit unsigned, 16-bit signed, 24-bit
- **Pitch**: Log2 fixed-point (4.12 format). Pitch 0 = 48kHz native rate.
- **Envelopes**: Multi-segment amplitude envelopes (attack, decay, sustain, release)
- **HRTF**: Head-Related Transfer Function for 3D positional audio
- **Mixbins**: 32 output channels, voices route to any combination
- **Filters**: Per-voice low-pass/high-pass (IIR biquad)

## Register Map (Key Registers)

```
Offset      Name                   Description
──────────────────────────────────────────────────
0x1000      NV_PAPU_ISTS           Interrupt status
0x1100      NV_PAPU_FECTL          Frontend control
0x2000      NV_PAPU_SECTL          Synth engine control
0x202C      NV_PAPU_VPVADDR        VP voice address base
0x3000+     NV_PAPU_GP*            Global Processor registers
0x4000+     NV_PAPU_EP*            Encode Processor registers
```

Full register definitions in `apu_regs.h`.

## Dependencies

- `qemu_shim.h` (from `../nv2a/`) — QEMU type abstraction layer
- `kernel32.lib` — Win32 threading
- `winmm.lib` — waveOut audio output
- `xaudio2_8.lib`, `ole32.lib` — primary Windows audio output
