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
this title reads a 64-element array of the table's handles, indexed by the light's set
number. A handle whose offset no index word gives also reads the table itself
(`HandleOp::TableWord`, Satsuma's "Texture handles read per invocation from a handle
table"), so the array is still bound when the index mechanism finds nothing. No texture is
bound for an element past the table. Handles that stay a single value keep the word the
search found, as before.

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
