<p align="center">
  <img src="dist/lichii.png" alt="TuttiFrutti" width="180">
</p>

<h1 align="center">TuttiFrutti</h1>

<p align="center">
  An experimental, open-source Nintendo Switch and Switch 2 emulator for Windows and Linux.
</p>

TuttiFrutti is a community build of the Lichii line (build 316) with the memory-management fixes and the shared JIT / pipeline-cache work from the TuttiFruchii experiments, rebranded and repackaged. It builds on Eglantine, Eden, yuzu and Sudachi, and is not affiliated with Nintendo.

Only use games and system files you have legally obtained. Games, keys and firmware are not included.

## Downloads

Get the latest build from the [releases page](https://github.com/elnekugm/TuttiFrutti/releases).

Both packages are x86-64 compiled for AVX2 (`x86-64-v3`) with profile-guided optimization (PGO) and ThinLTO.

| Platform | Package | Notes |
| --- | --- | --- |
| Windows x64 | `TuttiFrutti-windows-x86_64-pgo.zip` | Extract the whole zip and run `TuttiFrutti.exe` |
| Linux x64 | `TuttiFrutti-linux-x86_64-pgo.AppImage` | `chmod +x` and run; needs glibc 2.39+ (CachyOS, Arch, Ubuntu 24.04, Fedora 40, Debian 13) |

### Portable profile

The Windows zip ships an empty `user` folder beside `TuttiFrutti.exe`. While that folder exists, the emulator keeps its configuration, saves, NAND, shaders and logs there instead of in `%APPDATA%\lichii`, and the first start offers to import the profile of a previous emulator (Lichii, Eden, Yuzu and others). Delete or rename `user` to go back to the normal per-user profile.

The Linux AppImage uses the normal profile under `$XDG_DATA_HOME` (usually `~/.local/share/lichii`).

## What this build includes

- **Shared JIT code cache:** the CPU JITs share one copy of translated code between cores, which cuts repeated compilation and memory use (x64).
- **JIT warm-up:** precompiles the hot translation paths at start-up to reduce the first-minute stutter.
- **Pipeline cache sharing:** right-click a game and choose **Export Pipeline Cache** / **Import Pipeline Cache** to move shaders and Vulkan pipelines between machines. Imported pipelines are built on the next start and ones your GPU cannot use are skipped.
- **Memory fixes:** the stability v3 memory patch set from the Lichii line.
- No telemetry, no automatic uploads and no compatibility-report service: bug reports are written locally and only leave your machine if you send them yourself.

## Building

The build guide is in [docs/Build.md](docs/Build.md). The GUI target is `lichii`; its output is `TuttiFrutti.exe` on Windows and `TuttiFrutti` elsewhere.

For the release configuration used here:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DLICHII_TESTS=OFF \
  -DLICHII_BUILD_PRESET=v3 \
  -DLICHII_BUILD_PLATFORM=linux-x86_64-pgo
cmake --build build --target lichii lichii-cmd --parallel
```

The PGO profiles (`lichii-windows.profdata`, `eglantine.profdata`) are in the repository, and the [release workflow](.github/workflows/release-binaries.yml) runs the same configuration on GitHub Actions.

## Reporting a problem

Open an [issue](https://github.com/elnekugm/TuttiFrutti/issues) with the build version, game and version, CPU, GPU and driver. Bug reports captured with **Help → Report a Bug** (or `Ctrl+F12`) are saved as a zip next to the emulator and can contain the game's textures and shaders from one frame; check them before sharing and do not post them publicly.

## Credits and license

TuttiFrutti is free software under the [GPL-3.0-or-later](LICENSE). It is possible thanks to Eden, Eglantine, yuzu, Sudachi, Dynarmic and the many libraries they build on. The icon and wordmark are original artwork for this build.