# Per-title switches (`EDEN_NX2_<NAME>_<program id>`)

Several workarounds and optimisations are written for one game but are not wrong for others: a
rule about how a title reads texture handles, a storage binding larger than 8 MiB, the newest
image on reused memory. Rather than rebuild the emulator to try one in another game, each is a
switch (`Common::Nx2::GameSwitch`, `src/common/nx2_game_switch.h`) with three levels:

1. `EDEN_NX2_<NAME>_<program id>` — that title only. The program id is the 16 hex digits the
   game list and the log show, in upper case (`040077201BE50000`).
2. `EDEN_NX2_<NAME>` — every title.
3. The build's default, usually the title the switch was written for.

The per-title value wins over the global one. `0` (or an empty value) turns a switch off, anything
else turns it on, as the other `EDEN_NX2_*` switches read their value. The answer for a program id
is cached until another title runs, so the hot paths that ask (buffer binds, the texture
collector) pay one `getenv` per launch.

Lines go in `nx2_switches.txt`, which the GUI reads at startup and turns into the environment of
the emulator process (`%APPDATA%\lichii\nx2_switches.txt` or `$XDG_CONFIG_HOME/lichii/`, or the
copy next to the executable). A variable already in the real environment wins, so a launch script
can still override one run.

```
# Try Kirby Air Riders' written storage pool rule in Mario Kart World.
EDEN_NX2_SSBO_WRITTEN_SIZE_WORD_0400C3F00006E000=1

# Turn the baked-lighting rule off for Kirby itself.
EDEN_NX2_TC_KEEP_NEWEST_040077201BE50000=0
```

## The switches that exist today

| Switch | Default | What it does |
|---|---|---|
| `EDEN_NX2_SSBO_WRITTEN_SIZE_WORD` | Kirby Air Riders | A written native compute storage binding takes its size word, so a pool larger than 8 MiB is not cut (Kirby's 64 MiB particle pool). |
| `EDEN_NX2_TC_KEEP_NEWEST` | Kirby Air Riders | The newest image on memory that held other textures is downloaded (kept) instead of dropped without a write-back (Kirby's baked vertex lighting). |
| `EDEN_NX2_MACRO_DMA_DRAIN` | off | Every macro DMA read drains the split first. Also always on for a macro whose front run failed. |

A fix that turns out to help another title is enabled for it in the default function of its switch
(`OnForTitle`), not by another `if (program_id == ...)` at the call site, so this table stays the
one place to look.