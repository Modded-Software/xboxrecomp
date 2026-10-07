# Native NV2A-to-D3D11 backend

Implementation snapshot: **2026-10-03**.

This guide describes the **active Windows pushbuffer backend used by the
StarCraft: Ghost recompilation**, the implementation work behind its current
menu image, and the CPU-side defect that finally restored the missing scene.
It is not a claim of complete Xbox GPU emulation or verified gameplay.

## 1. Architecture: execute the game's commands, not a replacement scene

Ghost's recompiled game and Xbox D3D runtime still construct NV2A commands in
guest RAM. The host consumes those commands and translates their state and
geometry into D3D11 draws:

```text
Recompiled game / Xbox D3D runtime
    |
    | guest RAM commands, DMA_PUT
    v
NV2A memory/register servicing
    |
    v
Pushbuffer scanner: packets, jumps, calls, returns
    |
    v
Method executor: state capture, vertex fetch, topology expansion
    |
    | Nv2aGpuDraw + Nv2aGpuVertex + uint32 indices
    v
Native D3D11 backend
    |
    | VS / optional GS / PS, offscreen color and depth resources
    v
Completion + staging readback + guest-memory publication
    |
    v
Framebuffer presenter: displayed RGB image, F12 capture
```

There is **no host-authored Nova model, replacement room, injected logo glow,
or screenshot substitution**. The geometry, textures, matrices, light state,
combiners and blend settings come from the game.

The important distinction from a high-level D3D8 wrapper is that this backend
consumes **raw NV2A method state**. An Xbox API enum, host D3D9 default, or
high-level render-state conversion cannot automatically be substituted for a
hardware register encoding.

### Source map

All paths below are relative to the XboxRecomp repository.

| Component | Responsibility |
| --- | --- |
| [xbox_memory_layout.c](../../src/kernel/xbox_memory_layout.c) | Guest address mapping, NV2A register servicing and DMA consumption/completion ordering |
| [nv2a_pb_scan.c](../../src/kernel/nv2a_pb_scan.c) | Walks the submitted command stream and dispatches method parameters |
| [nv2a_pb_exec.c](../../src/kernel/nv2a_pb_exec.c) | Retains NV097 state, decodes vertices/textures, assembles native batches and reports unsupported methods |
| [nv2a_gpu.h](../../src/kernel/nv2a_gpu.h) | C/C++ interface between command execution and native rendering |
| [nv2a_gpu_d3d11.cpp](../../src/kernel/nv2a_gpu_d3d11.cpp) | D3D11 device, resources, caches, generated shaders, draw submission and readback |
| [nv2a_gpu_shader.h](../../src/kernel/nv2a_gpu_shader.h) | Shared embedded HLSL for fixed transforms, lighting, texture stages, combiners, clipping and depth |
| [nv2a_shader_cpu.h](../../src/d3d/nv2a_shader_cpu.h) | Shared NV2A descriptor definitions and numerical/decoding helpers; also used by the separate CPU path |
| [d3d8_swizzle.h](../../src/d3d/d3d8_swizzle.h) | Shared swizzle, packed-format and DXT decoding |
| [fb_present.c](../../src/video/fb_present.c) | Framebuffer window, RGB conversion and displayed-frame capture |
| [lifter.py](../../tools/recomp/lifter.py), [translator.py](../../tools/recomp/translator.py) | Native translation of the game's CPU instructions, including the camera-math flag repair |

The separate [nv2a_pgraph_d3d11.c](../../src/nv2a/nv2a_pgraph_d3d11.c)
translator is **not** Ghost's active rendering path. Likewise, the older
D3D8 combiner translator is not the pixel-shader generator described here.

The Windows backend is linked into `xbox_kernel` with C++17, `d3d11` and
`d3dcompiler`; see [kernel CMake configuration](../../src/kernel/CMakeLists.txt).
It requests a hardware D3D11 device and uses Shader Model 5.0 shaders. It does
not retry with WARP when hardware initialization or native rendering fails.

## 2. The draw interface

[nv2a_gpu.h](../../src/kernel/nv2a_gpu.h) deliberately separates three kinds of
data:

- **`Nv2aGpuVertex`**: position, diffuse/specular colors, four float4 texture
  coordinates, fog, normal, blend weights, and all 16 raw attributes.
- **`Nv2aGpuTexture`**: guest source bytes and layout, dimensions/pitch/format,
  cube/mip information, sampler controls, bump parameters, and decoder
  callbacks for pixels, faces and levels.
- **`Nv2aGpuDraw`**: target memory/layout, surface and window clipping, texture
  shader modes, combiners, raster/depth/stencil/blend state, vertex microcode
  and constants, indices, and fixed-function lighting/transform state.

Target and texture pointers refer to mapped guest storage. They are not
already-created D3D resources. The backend resolves them into retained host
resources, checking layout and aliasing before use.

The fixed/pretransformed input upload excludes the raw-attribute tail;
programmable vertices upload the full structure. Input offsets for newer
attributes derive from `offsetof`, and input-layout element counts derive
from the descriptor array rather than a stale hardcoded count.

### Lifecycle entry points

| Function | Meaning |
| --- | --- |
| `nv2a_gpu_available` | Lazily attempts hardware-device creation |
| `nv2a_gpu_compile` | Compiles/creates the shared base shader pipeline |
| `nv2a_gpu_draw` | Validates and submits a native triangle-, line- or point-list batch |
| `nv2a_gpu_execute_state` | Executes a guest transform-state program through a compute shader and returns updated constants |
| `nv2a_gpu_sync` | Waits for pending work and publishes dirty render targets to guest memory |
| `nv2a_gpu_flush` | Synchronizes, then marks retained targets for guest-memory refresh before reuse |
| `nv2a_gpu_invalidate` | Synchronizes and drops retained color/depth target resources |
| `nv2a_gpu_report` | Reports cumulative execution, cache, upload, publication and timing counters |

On Windows, failure to render a submitted batch produces diagnostics and
terminates the executor instead of silently switching to CPU rasterization.
The non-Windows CPU path remains separate. CPU-side command decoding,
topology conversion and texture-format decoding still occur; "zero CPU
fallback" describes rasterization, not the absence of CPU work.

## 3. Command-stream correctness and complete geometry

The scanner retains incomplete method packets across consecutive submissions.
A parameter word must remain a parameter even when the producer splits a packet
at a submission boundary. It also implements incrementing/nonincrementing
methods, both supported jump encodings, a DMA subroutine call/return slot,
address checks and a bounded command walk. Discontinuous packets, invalid
addresses, nested DMA subroutines and returns without calls fail explicitly.
Consumption/completion is not fabricated by advancing GET before execution.

The executor retains state between methods and assembles primitives at draw
boundaries. Triangle lists, strips, fans/polygons, quads and quad strips become
ordered triangle lists. Strip winding alternates correctly; fan anchors and
the last provoking vertex are preserved.

Point lists remain points; independent lines, line strips and closed line
loops become ordered line lists. Chunks end only between complete primitives,
including the closing line-loop edge. Indexed, inline-array and immediate
paths accept the appropriate minimum vertex count. `Nv2aGpuDraw.topology`
defaults to triangles at zero, preserving existing zero-initialized callers.
Native IA topology and geometry shaders match the normalized primitive type.
Line flat shading uses the final endpoint; nonfinite positions are rejected
by the line/point geometry shaders. Polygon culling and polygon depth offsets
do not suppress or offset actual lines/points. NV097 point-size state is
retained, but only unit-size points (encoded size 8) currently render; other
sizes fail explicitly. Native line and point counts are reported separately
from triangle statistics.

Several geometry fixes were necessary:

- `ARRAY_ELEMENT16` (`0x1800`) contributes two 16-bit indices per word;
  `ARRAY_ELEMENT32` (`0x1808`) contributes one full-width index.
- Mixed commands share one ordered `uint32_t` index stream. `DRAW_ARRAYS`
  retains its full start index instead of narrowing it to 16 bits.
- Guest index storage grows geometrically. The old fixed 4,096-element
  capacity could truncate geometry; it is not used as a silent limit now.
- Larger primitives are expanded into ordered native chunks, each within
  65,536 vertices and 196,608 indices. These are **per-native-draw limits**,
  not a reason to discard the remainder of a guest primitive.
- Vertex remapping compares full-width source indices, not just their low
  16 bits. Native index buffers use `DXGI_FORMAT_R32_UINT`.
- Attribute addressing uses wide byte-offset arithmetic and checks the
  complete attribute footprint against the applicable mapped-memory extent.
- Immediate vertices snapshot all 16 current attributes when a position
  completes. Later normal/weight/color writes must not retroactively change
  every vertex in the batch.

An earlier title replay exercised a 34,067-index guest batch and removed
`0x1808` from the unhandled-method inventory. This establishes that those
formerly missing commands are consumed, not that every high-index or chunk
boundary case has been visually verified.

## 4. Vertex processing and coordinate spaces

There are three distinct vertex paths:

1. **Pretransformed vertices** already describe guest screen-space positions.
2. **Fixed-function vertices** use the retained NV2A transform/light/texgen
   state in the shared vertex shader.
3. **Programmable vertices** supply all raw attributes to a generated HLSL
   translation of uploaded NV2A microcode.

### Fixed-function transforms

Dedicated matrix methods and generic constant uploads update the same
192-entry float4 constant bank. The shared shader uses these register ranges:

| Purpose | Constant rows |
| --- | --- |
| Composite projection | 0-3 |
| Model-view transform for slot `i` | `8 + i*8` through `11 + i*8` |
| Normal transform for slot `i` | `12 + i*8` through `15 + i*8` |
| Local-eye position | 56 |
| Viewport XY offset | 59 |
| Texgen planes for stage `s` | `64 + s*8` through `67 + s*8` |
| Texture matrix for stage `s` | `68 + s*8` through `71 + s*8` |

`fixed_matrix` explicitly computes the output components from the captured
constant vectors; it does not guess a host matrix storage convention.

With skinning disabled, the composite already includes model-view, so
**object-space position is projected directly**. Applying model-view a second
time was investigated and rejected. Eye-space position is still computed
separately for lighting, fog and coordinate generation.

Skin modes 1-6 select two, three or four model-view/normal transforms.
Odd-numbered modes derive the last weight as one minus the earlier weights;
even-numbered modes use explicit weights. The shader does not clamp or
renormalize the guest's weights. Normal normalization follows the guest
enable bit rather than being unconditional.

After projection, fixed vertices divide XYZ by W and add the programmed
viewport XY offset. The common bridge then reconstructs host homogeneous
coordinates from guest screen coordinates:

```text
clip.x = (2 * screen.x / targetWidth  - 1) * W
clip.y = (1 - 2 * screen.y / targetHeight) * W
clip.w = W
```

Z and W depth use different handling described below. Finite W magnitude is
bounded to the established reciprocal range while preserving its sign;
negative W is **not** converted to positive W.

### Programmable vertex and state programs

The backend validates uploaded 128-bit instructions and their end marker,
then generates HLSL with MAC/ILU operations, swizzles, negation, write masks,
temporary/output registers and relative constant addressing. Both halves
read pre-instruction operands before their results are written.

The generated VS bridges position, diffuse/specular, texture coordinates and
fog into the same downstream shader interface. A transform-state program
instead executes as a one-thread compute dispatch with writable constant
storage. Staging readback returns its updated 192 float4 constants to the
executor before later commands consume them.

Vertex/state shader cache identity includes the microcode itself. Constant
values remain dynamic, so changing constants does not require recompilation;
changing uploaded instructions cannot reuse a stale program. Vertex-program
constant writes remain unsupported; state-program writes are supported.
Complete physical-NV2A arithmetic equivalence is not established.

## 5. Lighting, fog and generated coordinates

The executor captures the correct lighting method, `0x0314`. The initial
implementation mistakenly treated `0x0310` (dithering) as lighting; successful
submission counters alone did not reveal that error.

Fixed-function lighting carries eight lights, with directional, local and
spot types, ambient/diffuse/specular colors, positions/ranges, attenuation,
directions, half-vectors and spot parameters. It also carries material/source
selection, scene ambient, alpha, normalization and specular controls.

Per-vertex evaluation includes the diffuse normal/light dot product, range
checks and local attenuation:

```text
attenuation = 1 / (constant + linear * distance + quadratic * distance^2)
```

Half-vector selection respects local-eye state; disabled specular and
separate/nonseparate specular follow the captured controls. Vertex colors
clamp before interpolation. These operations are gated to the fixed-function
path: a guest vertex program implements its own lighting.

Each texture stage independently supports disabled, object-linear and
eye-linear S/T/R/Q generation, plus legal sphere/normal/reflection components.
An enabled texture matrix transforms the resulting float4, retaining Q for
projective sampling. Lighting, eye-space fog and texgen share the weighted
eye-space transform.

Fog retains either the programmable fog output or the fixed source/generation
mode. Linear, exponential, exponential-squared and absolute variants produce
an interpolated factor. The pixel shader exposes it through the **combiner
fog register**; fog is not an unconditional blend pasted onto the final image.

Important limitations include approximate specular-exponent reconstruction,
incomplete spot-falloff coefficients, missing two-sided/back-light evaluation,
the uninterpreted alpha-from-material-specular flag, and unsupported
infinite-viewer reflection/sphere texgen. These are not made complete by the
successful menu capture.

## 6. Textures, texture shaders and register combiners

### Resource layout and sampling

The executor resolves texture layout and supplies existing shared decoders.
Supported resources include ordinary linear/swizzled 2D images, six-face cube
maps, declared mip chains and DXT1/3/5 storage. Suitable DXT uploads use native
BC1/2/3 resources; layouts unsuitable for that direct path use decoding.
Mip levels are uploaded from guest data, not generated by the host.

Packed/replicated formats include luminance/alpha, AL8, A8L8, G8B8/R8B8 and
additional 32-bit channel orders. Format conversion must preserve the Xbox's
channel replication, not merely choose a similarly sized DXGI format.

Samplers retain U/V/W addressing, border color, min/mag/mip filtering, LOD
bias/clamps and supported anisotropic combinations. Linear textures use
their dimensions to convert pixel-like coordinates into sampling coordinates.

FILTER's A/R/G/B sign flags convert selected sampled UNORM channels to
`2*x - 1` in the shader. This is not a different uploaded resource format.
Sampling modes also respect CONTROL0's texture-enable state; an explicitly
disabled texture needed by an active sampling mode rejects rather than
inventing a white/black/stale result.

Texture caches include layout and full source-byte snapshots, including
mips/faces. Reusing a guest address after its contents change must not reuse
the previous texture.

### Texture shader stages

Four NV2A texture stages run before register combiners. Current paths include
projective 2D sampling, cube sampling/projection, passthrough, sign-based clip
planes, bump mapping/luminance, dependent AR/GB reads, DOTPRODUCT/DOT_ST,
selected paired diffuse/specular reflection, cube-direction lookup and
constant-eye reflection.

Stage dependencies are validated: forward/self references, missing dot
predecessors and unsupported resource/mode combinations reject. Signed dot
channel encodings and unsigned HILO reconstruction are explicit conversions.
Selected hemisphere mappings, volume/shadow sampling, BRDF and DOT_ZW remain
unsupported.

Texture alpha kill discards zero-alpha fetches before combining and target
writes. Color-key processing is separate from final alpha testing. The
uncombined fast path cannot bypass required clip-plane, alpha-kill or
fragment-kill color-key behavior.

### Register combiners

Up to eight combiner stages evaluate independent RGB and alpha inputs,
component selections, input mappings, products/dots, sum/MUX selection,
output mappings and destination writes. A final combiner produces the output
color. Constants, diffuse/specular, texture results and fog are inputs to
this register machine.

Pixel shader variants specialize structural state: combiner words, texture
shader program, W-depth/depth enable, effective texture sign masks and window
clip mode. Color constants and other draw uniforms remain dynamic. This
allows unused stages/branches to compile away without replacing guest
material behavior with a universal `texture * diffuse` rule.

The logo/glow, textured room and armor are visible with this machinery in
place, but the capture does not isolate which individual stage produced
every pixel. A drawn stage counter is not proof of exact filtering or
combiner precision.

## 7. Rasterization, depth, stencil and clipping

The backend maps supported culling, front-face orientation, solid/wireframe
fill, blending, blend constants, color masks and depth/stencil state to
cached D3D11 state objects. Stencil operations/masks/reference are supported
on Z24S8 storage.

The native draw interface currently requires 32-bit color targets with valid
row pitch and dimensions no larger than 4096 per axis. Depth storage supports
integer Z16 and Z24S8 through D16/D24S8 host resources; floating-depth storage
is not supported. The presenter's ability to convert other framebuffer
formats does not imply the native draw path renders to all of them.

The optional geometry shader preserves the last provoking vertex for flat
colors, rejects nonfinite positions and computes polygon-offset depth slope.
It does **not** discard an entire finite triangle merely because one vertex
has negative W. D3D11 homogeneous clipping retains its visible portion.

Host near/far depth clipping is disabled. The pixel shader applies the
guest's clip/clamp range to interpolated guest Z or perspective W, applies
enabled face-specific polygon offset, and quantizes depth to the guest
integer storage range. W-depth uses its own shader variant and guard for
invalid/nonpositive depth; it is not treated as normalized Z.

Window clipping retains all eight hardware rectangles:

- Inclusive mode accepts their union.
- Exclusive mode rejects pixels inside the union.
- Rectangles use the hardware inclusive endpoints converted to half-open
  host bounds.
- Simple inclusive coverage can reduce to a native scissor; disjoint unions
  and exclusion use a scissor bound plus pixel-shader membership checks.

Using only the bounding box for a disjoint union would incorrectly fill its
holes. Using only the first rectangle would drop valid geometry.

## 8. Guest-memory coherence and presentation

The D3D11 device renders to offscreen textures, **not a title swap chain**.
The guest still expects framebuffers, clears, CPU memory accesses and command
completion to behave like guest-memory operations. The window displays the
published framebuffer through the existing RGB/GDI presenter.

Retained color/depth surfaces therefore have:

- host render resources and staging resources;
- guest memory identity and layout;
- dirty/refresh flags;
- a published source snapshot;
- conservative possible-write bounds and, where needed, disjoint scissor
  coverage masks.

Synchronization copies/maps dirty resources and publishes covered spans back
to guest memory with pitch and depth/stencil packing handled explicitly.
Cold snapshots initialize the full surface. Retained color readback can copy
a smaller bounding region; depth uses full resource copies. Publication
coverage represents possible writes, not an exact record of fragments that
passed every pixel test.

Refresh compares guest memory with the retained snapshot. Unchanged targets
avoid unnecessary upload; color mutations can produce a boxed upload.
Completion still requires an event query when pending draws have no color
or depth readback, for example output-masked work. Skipping readback must not
skip the GPU completion guarantee.

Sampling the current color target as a texture is rejected. Compatible
sampling of another retained color target can reuse its SRV; other overlaps
require synchronization. Incompatible target layouts invalidate retained
surfaces after publication. General arbitrary CPU/GPU concurrent-memory
coherence and feedback rendering are not solved by this model.

This correctness-first publication path has significant cost: resource
copies, blocking maps, depth conversion, guest-memory comparisons and
publication. Dynamic buffers are bucketed/reused; shaders, samplers and
render states are cached. The original scene-repair replay measured about
**5.4 FPS**, not 30 FPS. Reported host timing scopes are not a pure GPU
timestamp profile or an isolated optimization benchmark.

## 9. Why the scene was still missing after the backend work

The final blocker was **CPU instruction translation**, not a missing
renderer effect.

1. A displayed-frame capture reproduced logo/Press Start and faint floor
   fragments, without Nova and most of the room. The logo glow was already
   present in this baseline.
2. Temporary draw tracing found negative homogeneous W for scene draws.
3. Tracing the uploaded transform back through Xbox D3D found that the
   original guest camera/view matrix was already wrong.
4. Camera/vector math used SSE and x87 compare-to-EFLAGS instructions,
   followed by `LAHF`, `TEST AH, 0x44` and parity branches.
5. The lifter emitted a comment instead of `LAHF`. AH therefore contained
   stale register bits, changing normalization/look-at decisions.
6. Restoring the SSE flag transfers made Nova and the room visible, but
   upside down. Restoring the x87 transfers in the camera look-at path made
   the scene upright.

`LAHF` copies SF/ZF/AF/PF/CF into AH while preserving the rest of EAX. The
generator now materializes tracked flags before that copy, recognizes it as
a carry consumer, and preserves floating unordered/NaN behavior. Floating
comparisons clear AF; CF/PF/ZF must represent unordered values correctly,
not turn NaN into an ordinary comparison.

Targeted refreshes updated 1,157 floating-point flag-transfer sites in
Ghost's active candidate and 1,388 in the canonical generated tree without
replacing surrounding functions or earlier manual repairs.

No absolute-W workaround, guessed Z negation, second model-view transform,
unconditional lighting rewrite or completion bypass was needed. Fixing the
bad input state restored the scene using the existing backend.

## 10. Evidence, reproduction and remaining boundaries

### Verified result

The cleaned Debug title was replayed for 75 seconds and captured at 60 seconds.
The 640x480 displayed frame shows Nova upright on the left, the room/floor,
background figures and green monitors, cyan armor details, logo glow and
Press Start. It was visually compared with the user-supplied Cxbx image.
Cxbx itself was not run locally, and this is not an exact animated-frame or
physical-Xbox pixel comparison.

The last periodic report recorded 101,645 hardware batches, zero CPU fallback,
47,815 fixed-function draws and 707 flips. Builds and all five embedded
shader compilation variants passed. The standalone renderer executable was
compiled/linked, **not executed**; tests were not run for this continuation.
Gameplay, every implemented feature and full NV2A precision remain unverified.

### Build and capture

From the parent `recomp` workspace in PowerShell:

```powershell
cmake --build .\Ghost\build\candidate-msvc --config Debug --target Ghost --parallel 1
py -3 .\xboxrecomp\tools\compile_nv2a_hlsl.py
```

The shader utility compiles VS, GS, main PS, uncombined PS and a W-depth PS
variant. It concatenates all embedded HLSL raw-string fragments; checking
only the first fragment would omit later shader stages. These checks do not
compile every possible generated guest VS/CS or pixel specialization.

The active CMake cache selects `Ghost\build\recomp-seeded-candidate`, not
`Ghost\src\recomp\gen`. Verify that source selection before diagnosing or
regenerating the running title.

```powershell
# Close an earlier Ghost instance before launching or replacing its logs.
$env:RECOMP_WATCHDOG_SECS = '75'
$env:RECOMP_FB_CAPTURE = 'C:\captures\ghost-menu.bmp'
$game = (Resolve-Path .\Ghost\build\candidate-msvc\Debug).Path
Start-Process -FilePath "$game\Ghost.exe" -WorkingDirectory $game -Wait
```

Create the capture directory first. Press **F12 in the framebuffer window**
after the menu appears. F12 captures the presenter's displayed RGB buffer;
without `RECOMP_FB_CAPTURE`, it writes `framebuffer.bmp` in the working
directory. Allocation/write/close failures are checked and reported.
The watchdog's exit code 3 at the configured limit is intentional.

Ghost's [startup defaults](../../../Ghost/src/main.c) enable pushbuffer/input
bring-up and retain the AC97-ready/DSP-ack diagnostic overrides. Existing
environment values win. These audio overrides are not completed audio
emulation and should not be mistaken for renderer requirements.

### Input and sound integration

Ghost initializes host XInput and OHCI after its kernel bridge, and routes
trapped USB-register accesses through its fault handler. Its real Xbox XAPI
driver accesses a virtual Controller S with 20-byte XID reports. Eight omitted
SDK USB initialization/attachment/removal and XID report-processing callbacks
are recovered as bounded original guest functions and registered through
Ghost's manual dispatch, including verified external tail-call behavior.
The user confirmed physical-controller input works after report-parser recovery.
Native XInput slots are compacted into connected Xbox ports on each poll,
so player one does not require native slot zero. See the
[input integration guide](../../src/input/README.md) for mappings and diagnostics.

USB descriptor/HCCA metadata uses the contiguous physical window, which is
separate from low XBE RAM. Payloads and APU DMA use a mapper with page
provenance recorded by `MmGetPhysicalAddress`; ordinary-RAM buffers must not
be blindly redirected into the contiguous bank. This does not establish
general alias/coherence compatibility for numerically overlapping banks.
The monitor now preserves the VP/DSP's 256-frame stereo packet and
submits it at its 48kHz cadence, with queue backpressure and explicit errors.
The audio monitor requests balanced 1ms Windows timer resolution; this also
affects the existing short waits in GPU servicing, without skipping work,
publication or completion. See the [audio guide](../../src/apu/README.md).
Ghost's diagnostic DSP acknowledgement now resolves `gp:0x810` through the
programmed scratch SG table. Recovering USB initialization changed allocation
order, making its previous fixed mailbox stale and blocking rendering behind
audio initialization. This is still passthrough, not actual DSP execution.
The user confirmed intro-movie sound; sustained menu/gameplay audio is unverified.

Diagnostic acknowledgements now run independently of voice-front-end activity.
The earlier voice-gated scheduling deadlocked level loading when an audio
worker held the XACT lock while polling the DSP mailbox after voice processing
halted. The repair remains diagnostic passthrough, not DSP instruction execution.

### Level-entry bring-up

The second mission, Scattered Forces (`1_2_1_Miners_Bunker`), exposed an
explicit executor exit on NV2A primitive 4, a line strip. Native point/line
submission replaces that unsupported path without skipping its geometry.
The subsequent Debug replay reached the mission briefing and reported 10,571
native lines and 216 points; the user confirmed the game can run.

Ghost's omitted material-factory callback `0x00210FA0` is also recovered from
the original XBE and registered for indirect dispatch. The persistent
[recovery utility](../../../Ghost/tools/recover_render_callbacks.py) validates
the static registration, exact function boundaries and the explicitly approved
constructor tail target `0x002100A0`. It shares bounded translation with USB
callback recovery, and writes both active and canonical generated sources.
The missing loading-background image still requires visual verification;
factory recovery alone is not evidence that it is repaired.

The same replay later terminated explicitly on texture shader stage program
`0x2`, with a quad batch of 48 vertices. This is a separate compatibility gap,
not the repaired line-strip failure or the diagnostic watchdog. Stable extended
gameplay is therefore not yet verified. Debug and RelWithDebInfo builds and all
seven embedded shader compilation variants succeeded; tests were not run.

### Texture resource reuse

Changed content at an unchanged complete layout updates existing native
texture subresources rather than recreating the texture and SRV. Every
face/mip is decoded/uploaded; full source snapshots and byte comparisons
remain the invalidation authority. Same-address textures with different
pitch, mip count, cube layout or source extent retain separate cache entries.
FIFO eviction uses 1024 entries and a 256MiB resource-plus-snapshot threshold;
a single oversized entry may exceed the byte threshold.

The comparable Debug replay reduced texture creation from 17,096 to 82,
retained 82 entries/25.7MiB without eviction, and performed 627 content updates.
Warm menu intervals were approximately 5.79 FPS versus the preceding
5.49-5.59 FPS run. A 60-second displayed-frame capture retained Nova and the
room. This is a modest measured improvement, not evidence of playable speed.
No fences, surface publication, effects or draw work were skipped.

The RelWithDebInfo runtime has been rebuilt with the mailbox and report-parser
fixes. Select `Ghost/build/candidate-msvc/RelWithDebInfo/Ghost.exe` using the
launcher's `-ExecutablePath`; the launcher's default remains Debug. The earlier
black optimized replay used the stale mailbox, so it is not a performance
baseline. Post-repair optimized scene and frame-rate verification is outstanding.

### Open compatibility work

- Volume/depth/shadow textures, BRDF, DOT_ZW and selected dot mappings.
- Full specular/spot coefficients, back-face lighting and some texgen modes.
- Non-unit point sizes, point polygon fill and visible mixed front/back fill.
- Exact NV2A arithmetic, W-buffer slope behavior and filtering footprints.
- General framebuffer feedback and CPU/GPU memory coherence.
- Complete gameplay verification and sustained frame-rate improvements.

For chronological build/replay measurements and remaining limitations, see
the parent [Ghost progress record](../../../PROGRESS.md).

## 11. Reference strategy and extension checklist

Cxbx's fixed-function, vertex/pixel and packed-format implementations were
used as semantic references. Its working Ghost screenshot supplied a useful
visual target. Low-level NV2A method/constant/shader behavior was cross-checked
against xemu where high-level API state was insufficient. Reference
implementation bodies were not copied into this backend.

Useful reference entry points:

- [Cxbx fixed-function vertex shader](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/core/hle/D3D8/Direct3D9/FixedFunctionVertexShader.hlsl)
- [Cxbx fixed-function state](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/core/hle/D3D8/FixedFunctionState.cpp)
- [Cxbx pixel-shader template](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/core/hle/D3D8/Direct3D9/CxbxPixelShaderTemplate.hlsl)
- [xemu PGRAPH methods](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/pgraph.c)
- [xemu fixed-function shader generation](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/glsl/vsh-ff.c)
- [NVIDIA texture-shader specification](https://registry.khronos.org/OpenGL/extensions/NV/NV_texture_shader.txt)

For a new backend feature:

1. Establish the raw method encoding and retained state, not just an API name.
2. Wire executor capture, the C interface, C++ constants and HLSL together.
3. Update input layouts, shader cache identity and resource requirements where
   the feature changes them; constant-only changes should remain dynamic.
4. Validate combinations explicitly instead of substituting success-shaped
   defaults for unsupported behavior.
5. Preserve target publication, completion, winding and provoking semantics.
6. Distinguish compile success, submitted draws, exercised state and verified
   pixels. A framebuffer/reference comparison is necessary for a visual fix.
7. Inspect the guest-produced state when valid renderer math yields an
   impossible result: the camera repair demonstrates why GPU-only debugging
   can otherwise chase the wrong layer.
