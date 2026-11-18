# Changelog

All notable changes to Eglantine are documented here. Each release's notes are taken from its
`## [X.Y.Z]` section (release candidates use the section of the version they are a candidate for).
New entries go under `## [Unreleased]`; `tools/ci/bump-version.sh` turns them into a version section.

## [1.2] - 2026-11-18

### Performance

- Fast draws now re-read texture handles after handle writes or binds and resolve only the views
  that changed, including handles computed at draw time.
- A draw of another pipeline can recover that pipeline's own last draw when memory, mappings,
  command buffer, render pass and texture pool registers are unchanged.
- Memory-serial fast draws: register-only 3D changes no longer invalidate uniform keeps, and
  inline constant buffer writes that write the same words are skipped.
- Ocarina of Time no longer re-uploads its 64 KiB native uniform bank every frame (~1.5 FPS).
- Query cache: bank references and freed queries use separate cache lines, frees are batched,
  and small ranges look up the offsets a query can start at.
- Shader caches with 100,000 or more pipelines can be compiled on the loading screen before the
  game starts, with a Skip button. Each pipeline is built, handed to the driver cache and dropped,
  so memory stays flat; the driver cache is saved before play.

### Fixed

- Shader cache v2 logs past 2 GiB load in chunks instead of aborting a single read.
- Multi-range buffers retire after 60 unused frames and drain once per batch.
- Static pipeline caches are written atomically.
- Pipelines with the same bindings share pipeline and set layouts.
- The command list page is copied into its open tick's copy again.
- Shader job waiters run the driver stage themselves after the CPU stage.

### Builds

- Version 1.2. Four official packages: Windows x64 and Linux AppImage, each for x86-64-v3 and
  AMD znver3, built with Release, PGO and ThinLTO.

## [Unreleased]

- Fix unit-test linking when the OpenGL shader backends are disabled.

## [0.1.0]

- Initial release candidate
