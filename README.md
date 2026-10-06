
# Quake port for the PlayStation 2

## Overview

This is an unofficial fan-made port of id Software's Quake to the PlayStation 2 console. It derives
from [QuakeSpasm][link_quakespasm] 0.97.0, the modern, faithful Quake engine based on FitzQuake and
id's GPL source release: the engine, client, server and QuakeC virtual machine here are
QuakeSpasm's. id's original release notes for the Quake source code are in
[src/README.md](src/README.md).

Like the [Quake II port][link_q2_ps2] it builds on, it relies solely on the free
[PS2DEV SDK][link_ps2_dev], with no official Sony SDK and no proprietary libraries. The goal is a fully
functional and playable single-player Quake on the PS2, built entirely with freely available tools.

QuakeSpasm's C code under [src/quake/](src/quake/) is kept as close to untouched as possible; every change
is tagged `// [PS2_QUAKE]: <why>`. Everything specific to the console lives in [src/ps2/](src/ps2/) and is
written in modern C++ (C++20, no exceptions, no RTTI, warnings-as-errors). That backend comes from the
Quake II port: its renderer (GS front-end, VRAM texture heap, VU1 microprograms), input, audio, memory and
debugging code carry over, ported to QuakeSpasm's interfaces.

### Status

**Early bring-up.** QuakeSpasm boots on the PS2 and runs its game loop headless: in PCSX2 it
finds the game data on `host:`, plays the attract-mode demos through, and loads and runs every
shareware map, logging the console to stdout, with nothing drawn yet. Its PC-sized limits are
cut down to fit the PS2's 32 MB. The port is brought up in phases, each checked in PCSX2:

1. Compile QuakeSpasm with the EE toolchain. *Done.*
2. Link and boot, rendering nothing and logging to stdout. *Done.*
3. Game data and the game loop, headless. *Done.*
4. 2D: console, menus, HUD. *In progress: the GS and VU1 layer builds, not yet driven.*
5. Input: DualShock and USB keyboard.
6. 3D: world, lightmaps, water, sky, models, sprites, particles.
7. Sound, CD music, save games.

This section says what works as each phase lands.

---

## Getting started

### Prerequisites

1. **The ps2dev toolchain and PS2SDK.** Build/install from [ps2dev][link_ps2_dev]
   (`ps2toolchain` + `ps2sdk`). The build expects these on `PATH`:

   | Tool | Comes from | Typical location |
   | --- | --- | --- |
   | `mips64r5900el-ps2-elf-gcc` / `-g++` | ps2toolchain | `$PS2DEV/ee/bin` |
   | `openvcl` | openvcl | `$PS2DEV/bin` |
   | `dvp-as` | ps2toolchain (dvp) | `$PS2DEV/dvp/bin` |
   | `bin2c` | ps2sdk | `$PS2SDK/bin` |

   A `~/.zshenv` (or equivalent) along these lines is what the Makefile and the VSCode tasks expect:

   ```sh
   export PS2DEV=$HOME/ps2dev
   export PS2SDK=$PS2DEV/ps2sdk
   export PATH=$PS2DEV/bin:$PS2DEV/ee/bin:$PS2DEV/dvp/bin:$PS2SDK/bin:$PATH
   ```

2. **PCSX2**, to run it on the emulator, plus a PS2 BIOS image for it. The Makefile's `run` target
   defaults to `/Applications/PCSX2.app/Contents/MacOS/PCSX2`; override `PCSX2=` elsewhere.

3. **The Quake game data.** It is not included here. Place it in an `id1/` directory at the root of
   the repository (it is `.gitignore`d): either the shareware `pak0.pak` (episode 1), or the
   registered game's `pak0.pak` and `pak1.pak`.

   ```
   quake-ps2/
     id1/
       pak0.pak
       pak1.pak   (registered game only)
   ```

### Building

Three dependencies come in as git submodules: [vclpp](https://github.com/glampert/vclpp), the
preprocessor the VU microprograms go through, at [src/tools/vclpp/](src/tools/vclpp/) (with a submodule
of its own), [vu-checker](https://github.com/glampert/vu-checker), the checks every VU build runs over
the toolchain's output, at [src/tools/vu-checker/](src/tools/vu-checker/), and
[miniz](https://github.com/richgel999/miniz), the deflate codec for save games, at
[src/tools/miniz/](src/tools/miniz/). Clone with `--recursive`, or run
`git submodule update --init --recursive` in an existing clone.

```sh
make            # debug build   -> build/debug/quake.elf (+ host tools)
make release    # optimized     -> build/release/quake.elf
make run        # build, then launch it in PCSX2
```

| | `make` (debug) | `make release` |
| --- | --- | --- |
| Optimization | `-O2` | `-O3` |
| Debug info | `-gdwarf-2 -gz` | none |
| `PS2_Assert` / `PS2_AssertMsg` | on | compiled out |
| `PS2_QUAKE_DEBUG` (tests, debug-only code) | in | compiled out |

Both configs strip the ELF that runs; the symbols stay next to it in `quake_unstripped.elf`, which is
the one to feed `addr2line` or `build/tools/symbolize`. `make compiledb` regenerates
`compile_commands.json` for IntelliSense/clangd, and `make clean` removes all build output.

### Running in PCSX2

`make run` symlinks `build/<config>/id1` to the repo's `id1/` and launches
`PCSX2 -batch -elf build/<config>/quake.elf`. PCSX2 needs a few settings that are **off by default**.
Edit `~/Library/Application Support/PCSX2/inis/PCSX2.ini` (macOS) **while PCSX2 is closed**, since it
rewrites the file on exit:

```ini
[EmuCore]
HostFs = true             ; host: maps to the ELF's directory; without it there is no game data

[Logging]
EnableIOPConsole = true   ; the game's stdout goes through the IOP
EnableFileLogging = true  ; ...and into logs/emulog.txt

[USB1]
Type = hidkbd             ; optional: a USB keyboard that passes host keystrokes through
```

The engine's console output is prefixed with `[Q1]` in the log:

```sh
tail -f ~/Library/Application\ Support/PCSX2/logs/emulog.txt | grep '\[Q1\]'
```

`make run RUN_ARGS="-heapsize 20480"` puts arguments on the game's command line (PCSX2's
`-gameargs`). `+commands` there do nothing with the shareware data: like id's Quake,
QuakeSpasm only runs them for the registered version. An `id1/autoexec.cfg` runs at boot
either way, as on the desktop, and is the way to script a session.

### Debugging commands

| Command | What it prints |
| --- | --- |
| `ps2_memstats` | The backend's memory tags, and dlmalloc's arena, in-use, untagged-malloc and free totals. |
| `hunk_print` | QuakeSpasm's own: the hunk by block, with what remains. |
| `ps2_dump_iop_mods` | The IOP modules currently loaded. |

---

## Source layout

```
src/
  quake/                QuakeSpasm's C: engine, client, server, QuakeC VM
  ps2/                  the PS2 backend - all new C++ code
    system/             main() entry point, Sys_* seam, IOP boot, dlmalloc heap
    renderer/           GS front-end, VRAM heap, textures, models, VU1 path
      vu1progs/         VU1 microprograms (.vcl)
    audio/              SNDDMA_* seam, audsrv device, mix ring, CD music streaming
    input/              IN_* seam, DualShock pad and rumble, USB keyboard
    math/               vector/matrix math for the renderer
    net/                the network driver table (loopback only)
    save/               save games on the memory card
    debug/              on-screen error printing, stack traces, EE exception handling
    tests/              standalone bring-up scenes and tests
  tools/
    host/               host-side command line tools (unpak, musenc)
    scripts/            Python helpers (symbolize, compile_commands.json, frame logs)
    vclpp/              VCL preprocessor for the VU microprograms (git submodule)
    vu-checker/         openvcl/dvp-as output checks run by every VU build (git submodule)
    miniz/              deflate codec for the save games (git submodule)
    vscode_extensions/  VCL/VU assembly syntax highlighting for VSCode
```

Much of `src/ps2` is still the Quake II port's code, waiting for the phase that ports it.

---

## Credits

- [QuakeSpasm][link_quakespasm], which this port derives from: everything under [src/quake/](src/quake/)
  is QuakeSpasm 0.97.0's code, by the QuakeSpasm developers, changed only where tagged `[PS2_QUAKE]`.
  QuakeSpasm is itself based on John Fitzgibbons' FitzQuake.
- id Software's Quake, released under the GPL in 1999 ([source][link_id_repo]; the original release
  notes are in [src/README.md](src/README.md)).
- The PS2 backend under [src/ps2/](src/ps2/) comes from the [Quake II port][link_q2_ps2].

---

## License

Quake was released by id Software under the GNU General Public License, and QuakeSpasm is
distributed under the same terms. New code written for the PS2 port and any changes made to the
original source code are also released under the GNU General Public License version 2. See the
accompanying LICENSE file for details.

[link_quakespasm]: https://github.com/sezero/quakespasm
[link_id_repo]: https://github.com/id-Software/Quake
[link_q2_ps2]: https://github.com/glampert/quake2-ps2
[link_ps2_dev]: https://github.com/ps2dev
