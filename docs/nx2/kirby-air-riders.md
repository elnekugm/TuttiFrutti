# Kirby Air Riders fixes (title `040077201BE50000`)

## Device loss in races (a corrupted list, and warp-synchronous shared memory)

A few minutes into a race the GPU faults: the log's device fault carries an
`instruction pointer (fault)` and an `invalid read`. This is upstream issue
[Lichii#1693]. A compute program (`1175573eed7481a4`) walks a linked list in GPU
memory; when a node's next index is wrong or the end marker is lost, the loop
never ends. The loop safety counter cuts it off after 8192 iterations, the
records it never wrote keep vertex-like data, and the game's 3D macros (`0x1d2`,
`0x1e7`) read those words as draw counts: draws of about two billion vertices,
which the GPU runs until it faults. On AMD the same corruption shows as geometry
drawn from the wrong place (Orbitals' aurora loses a quarter of its sort keys).

The list is corrupted because guest code reads shared-memory words that other
threads of its warp just stored, with no barrier in between (warp-synchronous
scans and reductions): the console's warp runs in step, Vulkan orders such
accesses only through a barrier. `WarpSharedOrderPass` puts one before every
shared load a store can reach with no guest barrier on the way, and those
programs run whole guest warps. Ported from [Lichii#2082] (open upstream).

The barrier's form is a switch, because the PR's memory barrier does not make a
subgroup's lanes rendezvous and NVIDIA's independent thread scheduling can drift
them apart:

| `EDEN_NX2_WARP_SHARED_BARRIER` | Emitted | Needs |
|---|---|---|
| unset or `0` | `OpMemoryBarrier` (subgroup, acquire-release on shared) | nothing: legal wherever the load is |
| `1` | `OpControlBarrier` subgroup (execution and memory) | the subgroup's active lanes reach it, as the guest assumes |
| `2` | `OpControlBarrier` workgroup | every thread of the workgroup reaches it |

### The barrier is not what stops the device loss (tested)

Raced until the fault on the owner's Windows machine with an NVIDIA GPU (build
`136534fd58`, 2026-11-23), once per form:

| `EDEN_NX2_WARP_SHARED_BARRIER` | Fault | Visual corruption before it |
|---|---|---|
| unset | device loss at 93 s | the most |
| `1` | device loss at 98 s | less |
| `2` | device loss at 107 s | less |

All three carry the same device fault: `instruction pointer (fault)` plus an
`invalid read` at a 2 MiB-aligned address (`0x4a9600000`, `0x4a8400000`,
`0x4aae00000`). So the missing barrier is a real defect — the control barrier
visibly reduces the corruption, and #2082 stays worth having — but it is **not**
the source of the corrupted list: with all three forms in place the list is still
corrupted and the GPU still faults. [Lichii#1693] is still open for that, and the
reports there carry the same signature on build 386 (an RTX 4070 Ti log closed as
a duplicate on 2026-11-23; Nsight Aftermath dumps of an MMU fault inside the loop
shader, also at 2 MiB-aligned addresses). What is left there is a GPU trace: the
bad records are written by one of the six compute dispatches that fill those
pages, and only the memory itself names which.

## Dark riders and a dim menu: constant-buffer handle tables

The scene was uniformly about 1.5x darker than Satsuma's and had lost its
specular highlights (the menu's cards went grey). Not a texture, not the probes,
not the shader translation:

- the 155 native programs both builds compiled for that menu were **byte for
  byte identical**;
- the same passes and the same 23 compute dispatches ran in both;
- the 33x33x33 light-probe volumes had identical statistics (mean 0.5001).

What differed was the value a handle resolved to. Kirby Air Riders' **vertex
light bake** picks its light volumes from a table in a **constant buffer** whose
offset is a storage-buffer word times 16, **kept in workgroup memory**. Our tree
had Satsuma Build 93's *storage-buffer* handle tables (the compute light probes),
but not the constant-buffer ones: the handle fell back to the sampler-bits word
alone, which is no texture, and the bake wrote zero light to every vertex. Satsuma
says exactly that in `TryCbufTable`'s comment.

Ported from Satsuma Build 93 (`be9b8f6ea0`): `TableOffsetShift` (recovers the
shift by scanning the slot's `WriteSharedU32` when the offset arrives as a
`LoadSharedU32`), `TryCbufTable`, and `ConstBufferAddr::dynamic_shift`. Where our
tree differs: `MakeInst` is one block instead of two, so `Op::Index` and
`Op::TableWord` win when they already resolve; a handle that depends on a load is
resolved even on a host without non-uniform sampled-image array indexing (a cbuf
table needs no handle program); and a table does not replace an array the earlier
search already found in the same buffer. `EDEN_NX2_CBUF_TABLES=0` and
`EDEN_NX2_HANDLE_TABLES=0` turn them off for an A/B.

Also ruled out on the way, with data, so nobody repeats it: the scaling filter
and anti-aliasing, `EDEN_NX2_TC_KEEP_NEWEST`, and the local-memory index clamp
(a real SPIR-V UB, kept, but not this). Two comparisons that *looked* like
findings were artifacts and are not evidence: matching TIC indices across two
runs (the table is filled in a different order), and comparing a UTF-16 dump of
Satsuma's header against our UTF-8 one.

**Still open:** the device loss.

## A DMA copy's whole destination is GPU-written

`BufferCache::DMACopy` rewrites the destination through the copy engine, but it
only recorded the ranges whose *source* was GPU-modified: the destination was
subtracted from `gpu_modified_ranges` (the subtraction overlapping copies need)
and only the mirrored source ranges were added back. A copy from plain
(CPU-written) bytes therefore left the destination recorded as not GPU-written at
all, while its buffer did hold the copy's bytes.

Kirby Air Riders copies its command lists that way. With `EDEN_NX2_READ_CHECK`
the check found the page milliseconds before the device loss, and the log line
`Read download check ... differs from a synchronous flush ... gpu-modified now
true` showed the range was reached but not marked where the flush looks. On a
Switch 2 title the destination is also what `DmaPusher::IsGpuWrittenCommands`
asks about: unmarked, the front parsed a copied list page out of guest memory
instead of the GPU's copy, and its macros read the page's pre-copy words as draw
counts.

The fix marks the whole destination (`gpu_modified_ranges`, plus
`Tegra::Nx2MarkGpuWritten` for a Switch 2 title) and keeps a downloaded copy of
it, as `MarkWrittenBuffer` does for a shader store. The source's per-range
downloads still come from the mirrored ranges. Pending in-game confirmation
(build `1a3d7adcf3`).

### A fence download of it queues nothing on a Switch 2 title

`QueueFenceDownload` has two paths. `AsyncBufferDownloads()` queues the whole
range; that path is off for a Switch 2 title (native tiling), by design, because
downloading every GPU write at its fence copied 250-330 MB/s back. The other path
queues only the parts already marked Preflushable and arms read traps for the
rest. So `QueueFenceDownload(*cpu_dest_address, amount)` alone delivers nothing
on Kirby: `EDEN_NX2_BUFFER_DOWNLOADS=1` forces the first path, which proved the
mechanism but ran out of host memory at 53 s (`No memory for a 85541824 byte
staging buffer`) because it turns on downloads for *every* GPU write.

## The split and stale downloads are both exonerated

Two runs on top of the destination fix (`1a3d7adcf3`), both ending in the same
device fault (`instruction pointer (fault)` plus an `invalid read` at a 2 MiB
aligned `0x4a7600000`):

- `EDEN_NX2_SPLIT=2`, the default for a Switch 2 title, macros on the front: the
  read check was clean for the whole run (1821 downloads, 33430 KiB compared, 0
  bytes differ, 0 GPU-modified bytes no download brought, no outdated read), and
  the GPU was lost at 56.7 s. Guest memory was *correct* everywhere that tool
  looks, so what faults the shader is GPU-side.
- `EDEN_NX2_SPLIT=1`, macros on the back: lost at 37.6 s, and the check named the
  other half of the race — `an earlier read's download brought it, and the GPU
  wrote it since without taking it out of that download` over `0xa1a1000`, with
  `gpu-modified now false`: a GPU write to a page an early download had landed,
  which no path took out of that download.

So neither the split nor a stale read download produces it. What faults is a
shader reading an address built from GPU-written records, which is the
linked-list walk of [Lichii#1693]. What is left there is a GPU trace of the
frame, kept on the player's PC: it holds game data and shaders.

## The loop safety counter is set again where its loop exits

The SPIR-V emitter gives every loop a private counter that starts at 8192 and is
decremented at each back-edge, so a broken list truncates instead of hanging the
GPU. Nothing set it back when the loop exited: a loop entered again — a loop
nested in another one is entered on every outer iteration — got only what was
left of the count over the whole dispatch. Once that ran out, the loop stopped
early, the records it never wrote kept vertex-like data, and the macros read
those words as draw counts: the chain at the top of this page. Running with the
checks off does not fix it, because then the walk never ends at all (it hangs
instead of faulting), which is why that switch makes this game worse.

`Traverse` now stores the whole count back into a loop's counter at the block the
loop merges to, so every entry of the loop starts with the full count. Ported
from Lichii `4ad3a9d801` (zortos293), by way of Satsuma and Nemyu 0.1.8: neither
our base nor upstream `nx2` had it. `src/tests/video_core/loop_safety_counter.cpp`
checks that each of two loops emits its decrement and its reset.

## Light probes read per invocation (dark riders)

Ports from [Satsuma](https://gitlab.com/FilDev/Satsuma) (FilDev, GPL-3.0), which
plays the game on its Fritchii base. Each one is on for this title only unless noted, and
each can be turned off:

| Switch | Default | Turns off |
|---|---|---|
| `EDEN_NX2_SSBO_WRITTEN_SIZE_WORD` | on for this title only | the size word of a written native compute pointer |
| `EDEN_NX2_TC_KEEP_NEWEST` | on for this title only | keeping the newest image on overlapped memory |
| `EDEN_NX2_MACRO_DMA_DRAIN` | off | the drain every macro DMA read makes after a macro whose front run failed (that drain is always on for such a macro) |

## Light probes read per invocation (dark riders)

The riders are lit by light probes a compute pass fills each frame. That pass picks each
thread's three textures from a 6-word handle table in memory, by a set number it reads per
thread (`set * 12 + 0/4/8`), with the sampler bits ORed in from a constant buffer word.

The constant buffer search that names the texture of a bindless handle skips memory loads,
so it found that sampler word alone and bound it as the whole handle (texture 0): the pass
found no light, wrote black probes, and the ambient light came out black (Fil's RenderDoc
capture, frame 1470). The handle program builder had already made the earlier port of
jpkto's descriptor arrays (Ocarina of Time's light tables, `bd0171f7b9`) able to turn such
a table word into an array, but the search's word won before the builder ran.

Now a handle that depends on a memory load but tracks to a constant buffer word is computed
instead, and a computed *array* (a descriptor the shader indexes per invocation) is kept:
this title reads the table's handles indexed by the light's set number. A handle whose offset
no index word gives also reads the table itself (`HandleOp::TableWord`, Satsuma's "Texture
handles read per invocation from a handle table"), so the array is still bound when the index
mechanism finds nothing. No texture is bound for an element past the table. Handles that stay a
single value keep the word the search found, as before.

That array is 256 elements (`HANDLE_TABLE_MAX`), not the number of words the table had while
the shader was translated. The game keeps the shader and hands it a longer table on a bigger
map: 8 sets of 3 light volume textures in the menus, 26 sets on Waveflow Waters. Sized from
the short table, every element past the array's end read the array's last texture — a distance
texture as irradiance — and objects glowed white (Satsuma's fix: the arrays "go up to 256
entries now"). Each draw fills only the words the table has; the others get no texture.

## Written native compute storage bindings take their size word

The game's particle pool is 64 MiB of 1 KiB pages that its emitter update shaders write.
A Switch 2 compute storage buffer written by the shader was capped at 8 MiB, so every write
past page 8192 was dropped and its reads came back zero: the emitters freed page 0 every
frame, the pool's free list filled with junk, and the game showed missing UI, streaks and
neon green polygons, then wedged in a geometry shader (a GPU hang).

`BufferCache::StorageBufferBinding` now reads the size word after the pointer for a written
native compute pointer on this title, as it already did for read-only ones (Mario Kart
World's lighting). The write mark of such a binding grows with it, so the range is marked
GPU-modified as a whole.

## The newest image on overlapped memory is kept

The game bakes each mesh's vertex lighting once, at level load, into small render targets
(R11G11B10, 64 or 4096 wide) in memory that held other textures before, and its vertex
shaders sample them whenever the mesh is drawn. The older images on that memory made each
one a bad overlap, so the collector deleted it without writing it back, and its next use
read the old textures' bytes from guest memory as vertex colours: fences, planks and walls
glowing in random colours.

`TextureCache::IsNewestWritten` returns true for a GPU-modified image with no overlapping
image created after it (`ImageBase::birth_tick`, a counter raised at construction), and
`CanEvict` downloads (and therefore keeps) such an image instead of dropping it.

## A failed macro's DMA reads wait for the GPU

The game froze in Waveflow Waters and once took the whole PC: the GPU split's front ran the
3D macro `0x1e7` with a count its DMA read took before the back had written it, looped a
million steps, and the half-run macro's draws hung the GPU (the frame's fence never came).

A macro whose front run fails is remembered (`FrontDrainedMacros`), and from then on its
DMA reads drain the split first, which is what `EDEN_NX2_MACRO_DMA_DRAIN=1` does for every
read. Nothing changes for a macro that runs: the reads still wait only for the queued work
that can write the words they read.

## The sky wedge is a NaN from one effect draw (open)

In races, some frames show a huge black wedge in the sky (rarely magenta) that blinks
frame to frame. Trace `trace-1795827473.lxtrace` (recorded on the owner's PC) replays it
on an NVIDIA RTX 5060 too, so it is not a driver difference. The frame dump of frame 15
puts it in the 1920x1080 light target `0x81079a0000`: the pass before the effect draws
(pass 16, draws 67-88) reads back `nan:0`, and after them (pass 19, draws 101-124) it
reads back `nan:250282`. The NaN is what the target stores, and the present turns it
black; the dump's PNG shows it as magenta.

Bisected with `EDEN_NX2_REJECT_HASH` on the replay (black-pixel count over the wedge):
rejecting either stage of draws 107-108 (`1608b9179183e4fd` vertex,
`a93e8dde125713f5` fragment, pipeline `9e35795fa4ccbeea`) removes it; rejecting anything
else in the pass does not. Both stages are needed, so the draw itself produces the NaN.

It is data-dependent: the same pipeline, at the same vertex-stream addresses, draws the
same 96-vertex instanced mesh in the clean frame 16 without a NaN. Ruled out so far:

- the constant buffers (uniform 0 carries the same single NaN at offset 0x510 in both a
  clean sibling and this draw; 3, 4, 5 and 6 are identical across the group);
- the vertex streams (binding 0 is the unit mesh, binding 1 half-float UVs, binding 2 is
  `0xFFFFFFFF` in both frames, a packed attribute, not float NaN);
- the constant-buffer handle tables (`EDEN_NX2_CBUF_TABLES=0` and
  `EDEN_NX2_HANDLE_TABLES=0` leave the artifact);
- a 4 MiB null buffer (`BufferCacheRuntime::NullBufferSize`) for the null storage
  bindings this draw reads (`ssbo0`-`ssbo7` in the vertex shader, `ssbo0` in the
  fragment shader): unchanged;
- pending CPU writes into GPU-written memory (`EDEN_NX2_PENDING_WRITE_TRACE`): none.

Left to check: the fragment shader's `fs_tex2_168` sample and the null storage buffers
themselves. The sampled-texture frame dump (`EDEN_NX2_FRAME_DUMP_SAMPLED`) cannot reach
this draw: it loses the device before it. The loss is not tied to one image — with a
2 MiB cap on sampled textures (`EDEN_NX2_FRAME_DUMP_SAMPLED_MAX`) it moves to an earlier
pass, and `invalid write at 0x37b400000` (4 KiB at a 2 MiB boundary) repeats across runs.
The image the effect most likely samples, pass 11's `0x8111e10000`, is dumped as a render
target and reads back 0 NaN (values 0-1), and the NaN appears only in passes 19, 31, 32
and 33 (all at or after the effect draws). Whether that read-back loss is the same bug as
the game's own device loss is still open; it needs its own capture set to reproduce
without the dump.

### What the fragment shader does

Its SPIR-V (captured with `EDEN_NX2_FRAME_DUMP_SHADERS=1`) samples `fs_tex2_168` at
`attr0.xy * FragCoord.w / FragCoord.w` and then runs an absolute-log/exp tone map over the
result (`Log2(|x|) * (1/c0_derived)` then `Exp2`). The `FragCoord.w` multiply/divide
cancels, so it cannot be the NaN; the tone map turns Infinity into Infinity or zero but
not into NaN. The sampled colour is the only input left that can be NaN. The effect's
constants (one NaN at `uniform[0]+0x510`, identical in clean siblings), its vertex streams
(binding 2 is the packed `0xFFFFFFFF` attribute in both frames) and the null storage
bindings are ruled out, and graphics pipelines with dropped unmapped slots
(`cache_records.py dump --unbound`) do not include this one.

The NaN spreads through the bloom chain, which is why the wedge is far larger than the
draw and has stepped edges at mip resolution. Its size varies from run to run while the
frames that show it stay the same (15, 18, 22, 23, 25-28 in `trace-1795827473`), so frame
dumps of two runs are not directly comparable pixel for pixel.

### The NaN chain, measured with RenderDoc

A RenderDoc capture of the replay frame (see below) gives the fragments' inputs and the blend:

- The effect's instanced draws read an attribute declared `R32_FLOAT` from vertex stream 2,
  and that stream holds `0xFFFFFFFF` for every vertex — a NaN. The guest's own
  `vertex_attrib_format` asks for `R32_FLOAT` there (`type=7 size=0x12`), so the emulator
  decodes the format correctly; the data itself is a NaN.
- The vertex shader's outputs carry that NaN: 720 of 756 vertices of a 20-instance draw,
  and the position's `w` spans about ±3.5e10 (a unit billboard behind the camera should be
  around -1.2e3). The clean frame has the same degenerate output, so the geometry is broken
  in both; the wedge appears when one of those nonsense triangles happens to cover the sky.
- At a wedged pixel the fragment shader's inputs include `in_attr0 = (-7.9e8, -9.07e9, ...)`
  and `in_attr6 = (1, NaN, 1, 0)`. The draw blends `src*srcAlpha + dst*(1-srcAlpha)` with
  the shader's alpha 0, and RenderDoc reads the destination as finite (5.56, 8.63, 2.0) and
  the result as NaN: `Inf * 0`. The Infinity comes from the absolute-log/exp tone map over
  those huge attributes.

So the chain is: NaN vertex attribute -> degenerate position and attributes -> tone map
Infinity -> `Inf * 0` in the blend -> NaN in the light target -> bloom spreads it -> black
wedge. Nothing in the emulator invents the NaN: `0x1180a00340` holds `0xFFFFFFFF` in the
recorded guest memory itself, and no emulator path fills guest memory with `0xFF`.

### Capturing a replay frame with RenderDoc

`EDEN_NX2_RENDERDOC_FRAME=<present index>` brackets that frame with `StartFrameCapture` /
`EndFrameCapture` (`Tools::RenderdocAPI` now looks the DLL up lazily, because the Vulkan
layer loads it after the API object is built). Run the CLI with the layer enabled, no
injection needed:

```sh
VK_LAYER_PATH=<RenderDoc> VK_INSTANCE_LAYERS=VK_LAYER_RENDERDOC_Capture \
ENABLE_VULKAN_RENDERDOC_CAPTURE=1 EDEN_NX2_RENDERDOC_FRAME=15 \
TuttiFrutti-cli.exe --replay <trace> --out <dir> --frames 17
```

`EDEN_NX2_DESCRIPTOR_BUFFER=0` switches the device to descriptor sets. The descriptor-buffer
path allocates at opaque capture addresses, which RenderDoc cannot replay
(`VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS`), so a capture meant to be opened in RenderDoc
needs the switch — but that path binds no storage buffer to the vertex stage (RenderDoc
reports 0 read-only resources for every effect draw while the SPIR-V declares `ssbo0`-`ssbo7`),
so its shader output is not representative of a normal run.

RenderDoc's Python API is only available through `qrenderdoc --python <script>` (Python 3.8
inside the package): `OpenCaptureFile`, `OpenCapture`, `SetFrameEvent`, `PixelHistory`,
`DebugPixel`/`DebugVertex`, `GetPostVSData` and `GetBufferData` are enough to follow this
chain without the GUI.

### The null sampled image is now cleared to zero

Devices without `VK_EXT_robustness2` (this NVIDIA one, so `HasNullDescriptor()` is false)
replace a null sampled image with a freshly created 1x1 image whose contents were never
initialised: a sampler of it returned whatever the driver's memory held. A load through a
null descriptor returns zero, so `ImageView`'s fallback now clears the image once at
creation, like the storage fallback in `c336dc6fa0` does. This does not change the sky
wedge (frame 15 is identical), so it is a correctness fix, not this bug's fix.

### The effect samples one texture, and it cannot hold a NaN

`EDEN_NX2_FRAME_DUMP_GUEST_ONLY=1` keeps every sampled texture's guest bytes without a
single GPU read-back, so it cannot lose the device (1207 samples in frame 15, exit 0). With
the fragment stage's hash on each entry, draws 107-108 (`a93e8dde125713f5`) sample exactly
one texture: TIC 28974, `ASTC_2D_8X8_UNORM`, 4x512, at `0x1286ebc000`. An ASTC/UNORM
texture normalises to [0, 1] and cannot produce a NaN. `EDEN_NX2_FRAME_DUMP_SAMPLED_TIC`
reads one texture back on its own (that capture completes), and the host copy of this one
has content the guest bytes did not show, so the decode is doing something.

What is left is the arithmetic on finite values. The tone map is
`Exp2(Log2(|c|) * (1 / divisor))`; `divisor` is a product of two values the shader derives
(not a constant), so a zero divisor gives Infinity, and `Log2(|c|)` is exactly zero for a
channel of 1.0 (common in a UNORM sample), which makes `0 * Inf = NaN`.

`FMZ` and `f16` are ruled out: the captured fragment SPIR-V declares only `float` (no
`OpTypeFloat 16`, no `OpFConvert`), carries no `OpSelect` at all (so no flush-multiply-zero
select was lost), and its capabilities are `Shader`, `ImageQuery`, `SampledBuffer`,
`ImageGatherExtended` and `SignedZeroInfNanPreserve`. Plain IEEE f32 gives the same `NaN`
on the console.

### The frames are not the same draw after all: the vertex storage buffer is null

**Retracted.** A first comparison of the buffer dump's entries for `1608b9179183e4fd`
suggested the effect's vertex stage had eight null storage buffers in the wedged frame and
the pool in the clean one. Tracing `NoteBuffers` showed all eight are bound in both frames
(4 MiB each, `gpu_modified`), and the manifest simply had no room left: the frame's compute
dispatches already recorded ~1.8 GiB of storage buffers, so the dump's 2 GiB cap
(`truncated`) dropped the later draws' storage entries while their uniforms still fitted. A
frame dump's missing entries are not evidence of a missing binding.

With the bindings confirmed, every input to the draw is deterministic again: same constants,
same vertices, same pool, one UNORM texture that cannot hold a NaN, plain f32 arithmetic.
The remaining candidate is therefore the *content* of the pool the shader reads: it is
GPU-written, and the copies the dump records carry tens of thousands of `0xFFFFFFFF` words.
Whether those are the game's data, or regions the host buffer never had written
(uninitialised device memory reading as NaN), is what to check next - a host buffer that
starts undefined and is only partly written by a compute program would give exactly a
frame-to-frame blink as the allocator reuses it.

Diagnostics added for this round: `EDEN_NX2_RENDER_TRACE_FRAME=<n>` picks the one frame
the render trace describes (instead of every 600th), `EDEN_NX2_FRAME_DUMP_SHADERS=1` and
`EDEN_NX2_FRAME_DUMP_BUFFERS=1` write the frame's SPIR-V, pipeline keys, native programs
and bound buffers, `EDEN_NX2_FRAME_DUMP_SAMPLED_MAX=<bytes>` bounds which sampled
textures a frame dump reads back, and `EDEN_NX2_VERTEX_DUMP=<vertex shader hash>` prints
the enabled vertex streams of that shader's draws.

## Not ported

Satsuma also reads Kirby's shaders for its warm-up from the game's asset catalog
(`Asset/assets.cat`, PMAP shader maps in the `assets_*.dat` frames) and builds vertex and
fragment entries from them. Our shader precache (`docs/nx2/shader-precompile.md`) already
reads those frames and builds the compute programs; the graphics half stays as recorded
pipelines, because a graphics stage cannot be translated without its draw state.
[Lichii#1693]: https://github.com/zortos293/Lichii/issues/1693
[Lichii#2082]: https://github.com/zortos293/Lichii/pull/2082
