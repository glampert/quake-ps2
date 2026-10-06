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

The port is brought up in phases: compile, headless boot, game data, 2D, input, 3D, then sound, music
and saves. Until its phase lands:

- QuakeSpasm files a phase will replace (`gl_draw.c`, `gl_texmgr.c`, `gl_vidsdl.c`, `in_sdl.c`, the GL
  world/model/sky files) stay in `src/quake` unbuilt, as reference.
- Backend files still written against Quake 2 stay in `src/ps2` outside the Makefile source lists.
- **Unused files are deleted, not parked.** Each phase deletes what it made unused. The pristine
  sources are in the first commit (and in the quake2-ps2 repo for the backend).

## Toolchain

- EE compiler: `mips64r5900el-ps2-elf-gcc`/`g++` (GCC 15). There is no `ee-gcc`/`ee-g++`, so
  don't go looking for them. VU tools: `openvcl`, `dvp-as`. Also `bin2c`.
- `$PS2DEV` = `~/ps2dev`, `$PS2SDK` = `~/ps2dev/ps2sdk` (EE headers in `ee/include`). The
  SDK's C sources, for checking what a library really does, are under
  `~/ps2dev/src/ps2dev/build/ps2sdk/ee/<lib>/src/`. gsKit is at `~/ps2dev/gsKit`.
- Submodules: `src/tools/vclpp` (VCL preprocessor, with its own nested `external/parse-utils`),
  `src/tools/vu-checker` (`check_vu_code.py`), `src/tools/miniz`. The build uses the pinned
  vclpp it builds into `build/tools/vclpp`, never one on `PATH`.

## Game data

`id1/` at the repo root (gitignored): the shareware `pak0.pak`, or the registered `pak0.pak` +
`pak1.pak`. `make run` symlinks `build/<config>/id1` to it, since PCSX2's `host:` is the ELF's
directory. Engine output carries a `[Q1]` prefix in the PCSX2 log.

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
- `PS2_QUAKE_DEBUG`, `PS2_QUAKE_ASSERTS` and `PS2_QUAKE_PROFILE` are always defined to 0 or 1.
  Test them with `#if`, never `#ifdef` (`-Wundef` is on).
- make does not track flag changes. After editing `CONFIG_DEFS` or other flags,
  `rm -rf build/<config>/src`.
- Source lists in the Makefile are explicit. QuakeSpasm's C goes in `ENGINE_C_SRC`. Add a new
  `.cpp` to `PS2_CXX_SRC`, and to `SIZE_OPT_CXX_SRC` (built `-Os`) if it is load-time or
  debug-only rather than per-frame. Run `make compiledb` after adding or removing files.
- Tests and runtime checks run in PCSX2 or in host harnesses: see
  [.claude/rules/testing-pcsx2.md](.claude/rules/testing-pcsx2.md).

## Git workflow

- One branch: commit directly on `main`. Don't create feature branches unless asked. Push
  only when asked.
- There is no GitHub remote yet. Once there is: `git fetch` first, since the user sometimes
  commits on GitHub directly, so fast-forward if behind.
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
recipe), record it in the matching rule file below, so it travels with the repo. Files marked
*(Q2)* still describe the Quake II port; revise them as their subsystem is ported.

| File | Loaded for | Covers |
| --- | --- | --- |
| [testing-pcsx2.md](.claude/rules/testing-pcsx2.md) | always | PCSX2 setup and logs, scripted sessions, crash triage, the known TLB flake, host harnesses *(scripting and test sections: Q2)* |
| [backend-cpp.md](.claude/rules/backend-cpp.md) | `src/ps2`, host tools | naming, types, file style, passing the strict `-Werror` set |
| [ps2-platform.md](.claude/rules/ps2-platform.md) | `src/ps2` | ps2sdk traps, EE FPU, SIF DMA, IOP modules, ROM FILEIO, memory card |
| [gs-renderer.md](.claude/rules/gs-renderer.md) | `src/ps2/renderer` | GS/libdraw facts, frame model, CLUTs, VRAM blocks, mipmaps, ST scaling |
| [vu-microprograms.md](.claude/rules/vu-microprograms.md) | VU sources, vu-checker | openvcl/dvp-as/vclpp traps, VU0 inline asm, VCL comment style, runtime probes |
| [performance.md](.claude/rules/performance.md) | `src/ps2`, frame-log scripts | EE codegen facts, what PCSX2 can measure, capture/A-B/asm-test recipes |
| [engine-c.md](.claude/rules/engine-c.md) | QuakeSpasm's C | editing rules, build mode, seams, QuakeSpasm quirks |
| [memory-budget.md](.claude/rules/memory-budget.md) | heap, renderer, zone.c, quakedef.h | the 32 MB picture, QuakeSpasm's limits, measured budgets |
| [audio.md](.claude/rules/audio.md) | `src/ps2/audio`, musenc | CD music format and pipeline decisions, costs *(Q2)* |
| [save-games.md](.claude/rules/save-games.md) | `src/ps2/save` | save design, format, icon, config.cfg policy *(Q2)* |
| [vclpp-submodule.md](.claude/rules/vclpp-submodule.md) | `src/tools/vclpp` | vclpp/parse-utils conventions, verification recipes, CI, MASP mode, tyra |
