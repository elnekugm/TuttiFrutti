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

## Not ported

Satsuma also reads Kirby's shaders for its warm-up from the game's asset catalog
(`Asset/assets.cat`, PMAP shader maps in the `assets_*.dat` frames) and builds vertex and
fragment entries from them. Our shader precache (`docs/nx2/shader-precompile.md`) already
reads those frames and builds the compute programs; the graphics half stays as recorded
pipelines, because a graphics stage cannot be translated without its draw state.
[Lichii#1693]: https://github.com/zortos293/Lichii/issues/1693
[Lichii#2082]: https://github.com/zortos293/Lichii/pull/2082
