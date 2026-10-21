# Quake for the PlayStation 2

QuakeSpasm 0.97.0 (id's Quake, single player) ported to the PS2 with only the free ps2dev SDK, on the
C++ backend of the Quake II port (quake2-ps2). Two halves:

- **`src/quake`**: QuakeSpasm's C (engine, client, server, QuakeC VM, and the renderer-named files that
  hold engine logic, e.g. `gl_model.c`, which the server needs too). Kept as close to untouched as
  possible. Every change is tagged `// [PS2_QUAKE]: <why>`.
- **`src/ps2`**: the console backend, all new C++20 (no exceptions, no RTTI, strict `-Werror`).
  QuakeSpasm has no renderer interface like Quake 2's `refexport_t`, so the backend implements the GL
  renderer's public surface itself: `draw.h`, `render.h`, `gl_texmgr.h`, `vid.h`,
  `GL_BeginRendering`/`GL_EndRendering`, `Sky_*`. Plus `Sys_*`, `IN_*`, `SNDDMA_*`, `CDAudio_*`/`BGM_*`,
  `PL_*` and the `net_drivers[]` table. [src/ps2/common.h](src/ps2/common.h) is the one header that
  bridges to the C engine.

[README.md](README.md) is the architecture document and says what works so far. Read the section you
need before changing a subsystem, and keep it current when behaviour changes. [CVARS.md](CVARS.md)
lists every backend cvar. Add new ones there, with their debug/release defaults and flags.

## Port status

The port was brought up in phases: compile, headless boot, game data, 2D, input, 3D, then sound,
music and saves. All of them have landed, and every file in `src/quake` and `src/ps2` is built, or
included by one that is. It plays on a real PS2: loading from USB and the HDD, saves on the memory
card, rumble and the CD music from ripped tracks all work there, and in PCSX2 from `host:`. What is
left is refinement: performance (moving the engine's time from double to float where the profile
probes point). GS fog was left out on purpose:
none of id's maps sets it, so it stays a future exercise for custom maps.

- **Unused files are deleted, not parked.** Whatever a change makes unused goes with it. The
  pristine sources are in the first commit (and in the quake2-ps2 repo for the backend).

## Toolchain

- EE compiler: `mips64r5900el-ps2-elf-gcc`/`g++` (GCC 15). There is no `ee-gcc`/`ee-g++`, so
  don't go looking for them. VU tools: `openvcl`, `dvp-as`. Also `bin2c`.
- `$PS2DEV` = `~/ps2dev`, `$PS2SDK` = `~/ps2dev/ps2sdk` (EE headers in `ee/include`). The
  SDK's C sources, for checking what a library really does, are under
  `~/ps2dev/src/ps2dev/build/ps2sdk/ee/<lib>/src/`. gsKit is at `~/ps2dev/gsKit`.
- Updating it: `cd ~/ps2dev/src/ps2dev && PATH="$(brew --prefix gnu-sed)/libexec/gnubin:$PATH" ./build-all.sh`
  (`./build-all.sh 3 4 5` reruns only those steps: 1 toolchain, 2 ps2sdk, 3 ports,
  4 ps2-packer, 5 ps2client). ps2sdk-ports needs Homebrew's `pkgconf`, and its install runs
  `sed -i` on symlinked `.pc` files, which BSD sed refuses. Then `make clean`: make tracks
  neither the SDK's libraries nor its headers. Last rebuilt 2026-10-19: ps2sdk `329e2bb`
  (CI's pin), newlib `8881dc4`.
- Submodules: `src/tools/vclpp` (VCL preprocessor, with its own nested `external/parse-utils`),
  `src/tools/vu-checker` (`check_vu_code.py`), `src/tools/miniz`. The build uses the pinned
  vclpp it builds into `build/tools/vclpp`, never one on `PATH`.

## Game data

`id1/` at the repo root (gitignored): the shareware `pak0.pak`, or the registered `pak0.pak` +
`pak1.pak`. A `pakN.pkz` there (`make pkz`, a compressed copy) is loaded in its pak's place.
`make run` symlinks `build/<config>/id1` to it, since PCSX2's `host:` is the ELF's
directory. Engine output carries a `[Q1]` prefix in the PCSX2 log.

`id1/config.cfg` is the one tracked file there: the settings every run starts from. The game
rewrites it on quit, and an unchanged setup writes the same bytes, so `git status` shows when a run
changed a setting. Commit a changed default on purpose, not as fallout from a test.

## Build and verify

- **`make` is the compile check.** It is fast, uses the real flags (the full GCC-only warning
  set, `-Werror`) and links the ELF. Don't build host-side stub harnesses just to see whether
  something compiles. ps2sdk headers shadow host libc++ ones, and the GCC-only warnings
  (`-Wlogical-op`, `-Wduplicated-*`) would be missed.
- **Check `make`'s exit status. Grepping its output for `error:` is not enough.** openvcl's
  diagnostics don't match that pattern, and a failed VU build leaves the stale `.vsm`/`.o`
  from the previous run, which then gets linked.
- `make` = debug (`-O2`, asserts on) → `build/debug/quake.elf`. `make release` = `-O3`, no
  asserts, no debug-only code → `build/release/`. `make run` / `make release run` launch PCSX2.
  The symbols are in `quake_unstripped.elf` next to each stripped ELF.
- The host tools (`src/tools/host`) are a CMake project, which `make` configures into
  `build/tools/cmake/` and builds on every run. CMake tracks their dependencies, so the
  Makefile doesn't.
- CI: `.github/workflows/ps2.yml` runs `make` and `make release` on Linux, in the ps2dev
  `ps2sdk` image pinned by digest, for every change but docs. It keeps no ELF, since releases
  are packaged from local builds. `tools.yml` builds the host tools alone, with GCC, Apple
  clang and MSVC, and keeps the Windows executables as an artifact. See
  [.claude/rules/ci.md](.claude/rules/ci.md).
- `PS2_QUAKE_DEBUG`, `PS2_QUAKE_ASSERTS` and `PS2_QUAKE_PROFILE` are always defined to 0 or 1.
  Test them with `#if`, never `#ifdef` (`-Wundef` is on).
- make does not track flag changes. After editing `CONFIG_DEFS` or other flags,
  `rm -rf build/<config>/src`.
- **macOS's make (GNU Make 3.81) compares timestamps to the whole second.** A file edited for a
  scratch build and put back within the same second as that build looks unchanged, and its
  object keeps the edit. After undoing a temporary edit, `touch` the file before building, or
  clean (a hang test's wedge outlived its revert this way, 2026-10-13).
- Source lists in the Makefile are explicit. QuakeSpasm's C goes in `ENGINE_C_SRC`. Add a new
  `.cpp` to `PS2_CXX_SRC`, and to `SIZE_OPT_CXX_SRC` (built `-Os`) if it is load-time or
  debug-only rather than per-frame. Run `make compiledb` after adding or removing files.
- Tests and runtime checks run in PCSX2 or in host harnesses: see
  [.claude/rules/testing-pcsx2.md](.claude/rules/testing-pcsx2.md).

## Git workflow

- One branch: commit directly on `main`. Don't create feature branches unless asked.
- **Never push.** The remote is `origin` (github.com/glampert/quake-ps2); the user reviews the
  local commits and pushes them.
- `git fetch` first, since the user sometimes commits on GitHub directly, so fast-forward if
  behind.
- Push submodules before the repo that pins them: **parse-utils → vclpp → quake-ps2**. A
  gitlink that reaches GitHub before its submodule commit breaks recursive clones.
- Commit subjects are one sentence ending in a period, often prefixed with the area:
  `CD music: stream the soundtrack from loose SPU2 ADPCM files.`,
  `vclpp: back to C++17, so GCC 9 builds it; CI on GitHub.`

## Things that bite (details in the rules)

- The EE FPU has no Inf/NaN (1/0 = FLT_MAX), and double is soft-float. Host and target
  silently disagree on degenerate math. QuakeSpasm keeps time in `double` (`realtime`,
  `cl.time`), which is correct but slow.
- QuakeSpasm is sized for a PC. Its limits are cut to PS2 values, each tagged (see
  memory-budget.md); measure with `ps2_memstats` and `hunk_print` before raising one.
- ps2sdk stubs some libc calls to fail (`sysconf` → -1), and some of its register macros
  don't parenthesize their arguments. Verify before trusting either.
- SIF DMA target buffers need `alignas(64)`.
- The VU toolchain miscompiles silently. Only `check_vu_code.py` and the screen tell you.
- GS alpha 1.0 is `0x80`. Normalized ST spans the power-of-two TEX0 extent, not the image.
- PCSX2 models neither the EE cache nor GS-internal cost. A capture only gates regressions
  for those.

## Rules index (`.claude/rules/`)

When a finding is durable (a hardware fact, a toolchain trap, a measured baseline, a test
recipe), record it in the matching rule file below, so it travels with the repo. Facts marked
*(Q2)* inside a file were measured on the Quake II port, and still hold as far as is known.

| File | Loaded for | Covers |
| --- | --- | --- |
| [testing-pcsx2.md](.claude/rules/testing-pcsx2.md) | always | PCSX2 setup and logs, scripted sessions, the unattended tests, crash triage, the known TLB flake, host harnesses |
| [backend-cpp.md](.claude/rules/backend-cpp.md) | `src/ps2`, host tools | naming, types, file style, passing the strict `-Werror` set |
| [ps2-platform.md](.claude/rules/ps2-platform.md) | `src/ps2` | ps2sdk traps, EE FPU, SIF DMA, IOP modules, ROM FILEIO, memory card |
| [gs-renderer.md](.claude/rules/gs-renderer.md) | `src/ps2/renderer` | GS/libdraw facts, frame model, CLUTs, VRAM blocks, mipmaps, ST scaling |
| [vu-microprograms.md](.claude/rules/vu-microprograms.md) | VU sources, vu-checker | openvcl/dvp-as/vclpp traps, VU0 inline asm, VCL comment style, runtime probes |
| [performance.md](.claude/rules/performance.md) | `src/ps2`, frame-log scripts | EE codegen facts, what PCSX2 can measure, capture/A-B/asm-test recipes |
| [engine-c.md](.claude/rules/engine-c.md) | QuakeSpasm's C | editing rules, build mode, seams, QuakeSpasm quirks |
| [memory-budget.md](.claude/rules/memory-budget.md) | heap, renderer, zone.c, quakedef.h | the 32 MB picture, QuakeSpasm's limits, measured budgets |
| [audio.md](.claude/rules/audio.md) | `src/ps2/audio`, musenc | audsrv's traps and the sound feeder, CD music format and pipeline decisions, costs |
| [save-games.md](.claude/rules/save-games.md) | `src/ps2/save`, `host_cmd.c`, `menu.c` | save design and devices, archive format, the save notice, failure messages, icon, config.cfg policy, card and drive testing |
| [vclpp-submodule.md](.claude/rules/vclpp-submodule.md) | `src/tools/vclpp` | vclpp/parse-utils conventions, verification recipes, CI, MASP mode, tyra |
| [ci.md](.claude/rules/ci.md) | `.github` | the two workflows, the pinned ps2sdk image and moving it, checking a Linux build without Docker, runner traps |
