# Kirby Air Riders fixes (title `040077201BE50000`)

Three ports from [Satsuma](https://gitlab.com/FilDev/Satsuma) (FilDev, GPL-3.0), which
plays the game on its Fritchii base. Each one is on for this title only unless noted, and
each can be turned off:

| Switch | Default | Turns off |
|---|---|---|
| `EDEN_NX2_SSBO_WRITTEN_SIZE_WORD` | on for this title only | the size word of a written native compute pointer |
| `EDEN_NX2_TC_KEEP_NEWEST` | on for this title only | keeping the newest image on overlapped memory |
| `EDEN_NX2_MACRO_DMA_DRAIN` | off | the drain every macro DMA read makes after a macro whose front run failed (that drain is always on for such a macro) |

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