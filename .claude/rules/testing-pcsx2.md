# Testing: PCSX2, scripted sessions, host harnesses

## PCSX2 setup and logs

- App: `/Applications/PCSX2.app/Contents/MacOS/PCSX2`, launched as `PCSX2 -batch -elf <elf>`
  (`make run`). `host:` maps to the ELF's directory, and `make run` symlinks
  `build/<config>/id1` → the repo's `id1/`.
- `~/Library/Application Support/PCSX2/inis/PCSX2.ini` needs `[EmuCore] HostFs = true`
  (otherwise "No game data found"). Game stdout needs `[Logging] EnableIOPConsole = true`,
  because ps2sdk stdout goes through IOP fio, plus `EnableFileLogging = true` to land in
  `~/Library/Application Support/PCSX2/logs/emulog.txt`. Engine lines carry a `[Q1]` prefix,
  one per line however QuakeSpasm split the print, and Quake's console glyphs come out as
  ASCII (the separator bar as `-----`).
- **Edit PCSX2.ini only while PCSX2 is closed.** It rewrites the file on exit.
- Every launch overwrites `emulog.txt`. Copy it out before the next launch if you need it.
- PCSX2 memory card slot 1: `~/Library/Application Support/PCSX2/memcards/Mcd001.ps2`.
  `Slot1_Enable = false` in the ini simulates "no card".
- `[USB1] Type = hidkbd` attaches a host-passthrough USB keyboard. It reports itself as JIS,
  and its boot `Missing host mapping for QKey` warnings are harmless. It sends HID usage
  `0x34` for the host's `` ` `` key, never `0x35`.
- `Pad: DS2 Config Finished ... VS: Normal - VL: Normal` in the log means the vibration motors
  were mapped (`padSetActAlign`).

## Working with the user's machine

- Claude can't screenshot PCSX2 (`screencapture` is denied) or send it keystrokes (`osascript`
  is denied), **but the game can screenshot itself**: the `screenshot` command reads the last
  finished frame out of GS VRAM into `id1/spasmNNNN.tga` (the first free number). Script it
  from `autoexec.cfg` behind enough `wait`s for the scene to have drawn, then
  `sips -s format png id1/spasm0000.tga --out <scratchpad>/shot.png` and Read the PNG.
  Delete the TGAs afterwards. A 16-bit framebuffer reads back at 5 bits a channel (palette
  grey 31 comes back as 24). Outside what the game draws (the PS2 BIOS, PCSX2's own UI),
  anything visual still needs the user's eyes.
- The user may be at the machine while a run is up, and may close it. Don't relaunch PCSX2
  after they close a run without a reason.
- Runs read and rewrite `id1/config.cfg` (gitignored; QuakeSpasm writes it on quit).
  **Back it up before a scripted or perf run and restore it after.** Start every run of a
  comparison from the same config.

## Scripting a session

- `quake.rc` runs `exec default.cfg`, `exec config.cfg`, `exec autoexec.cfg`, then `stuffcmds`
  (the command line's `+cmd` arguments). So `id1/autoexec.cfg` (gitignored, a loose file next
  to the pak) is the place for a scripted session, e.g. `map e1m1`. `startdemos` comes after
  it and does nothing once a map or a demo is running. Delete the file after the run.
- `timedemo demo1` plays demo1 as fast as the engine goes and prints `N frames S seconds F
  fps`. With nothing rendered it measured 970 fps (about 1 ms of EE time per frame) on
  2026-10-06; PCSX2 doesn't model the EE cache, so treat it as a rough baseline.
- `path` lists the search path and `mods` the directories next to the ELF (on `host:` that
  is `build/<config>/`: `id1`, `irx`, `miniz`, `src`), which shows `opendir` works.
- `make run RUN_ARGS="..."` puts arguments on the game's command line through PCSX2's
  `-gameargs`, e.g. `RUN_ARGS="-heapsize 20480"`. PCSX2 passes them without a program name in
  `argv[0]`; `main.cpp` adds one, since QuakeSpasm reads options from `argv[1]` on. For runs
  that shouldn't overwrite `emulog.txt`, launch PCSX2 directly with `-logfile <path>`.
- **`+commands` do nothing with the shareware data.** `stuffcmds` reads the `cmdline` cvar,
  and `COM_CheckRegistered` only fills it when `gfx/pop.lmp` (the registered pak1) is found,
  as in id's Quake. `RUN_ARGS="+map e1m1"` boots into the demo loop. Use `id1/autoexec.cfg`.
- `-dedicated` doesn't work: QuakeSpasm stops with "Network not available!" when no driver but
  loopback comes up. A normal run's `map e1m1` already exercises the server, QuakeC and
  physics, with the client attached.
- Record the traps here as they turn up. The Quake II port's (a 128-char `COM_Parse` overrun,
  `wait`s queued across a map load) came from Quake II's code and may not apply.

## Tests

- **`ps2_testcube 1`** (debug builds) draws the VU1 test cube over every frame: the GS, VIF1,
  VU1 microprogram and VRAM upload smoke test. With a `screenshot` it checks all of that
  without anyone watching. Its variants are in [CVARS.md](../../CVARS.md).

The rest are scripted runs:

- **Map cycle**: an `autoexec.cfg` that, per map, runs `map <name>`, about 150 `wait`s (the
  client finishes connecting), `echo CYCLE <name>`, `hunk_print` and `ps2_memstats`, ending
  with `echo CYCLE_DONE`. All nine shareware maps (start, e1m1-e1m8) loaded clean on
  2026-10-06; the numbers are in [memory-budget.md](memory-budget.md).
- **Config write**: `quit` runs `Host_Shutdown`, which writes `id1/config.cfg` through
  `host:`. Then PCSX2 boots into the PS2 BIOS menu; stop it there.
- **Stop every PCSX2 you launch once its log is read.** A run stays open after the game halts,
  quits or idles, and the next launch starts another instance beside it.

## Quiet map for renderer work

`nomonsters 1` before `map e1m1` (QuakeSpasm filters monsters at spawn), or `notarget`,
`god` and `noclip` once in. Unverified on the PS2 until the 3D phase.

## Crash triage

- Debug builds print an EE exception report (cause, EPC, BadVAddr, stack). Resolve addresses
  against **the same build's** `quake_unstripped.elf`:
  `mips64r5900el-ps2-elf-addr2line -f -C -e build/debug/quake_unstripped.elf <addr>` or
  `build/tools/symbolize < emulog.txt`.
- **Known flake, never game code:** a `TLB Miss` in `_request_end` (ps2sdk `sifrpc.c`,
  `SIF_CMD_RPC_END`) during `host:` file I/O, usually right after a pak file is opened. The
  signature is one to three `TLB Miss, pc=<same> addr=0x10|0x18 [load|store]` lines:
  `cd->hdr.pkt_addr` is already null, one RPC completed twice under PCSX2's faked IOP HostFs.
  A second face of it is a wild `pc` below `.text` (0x100000), e.g. `pc=0x82000 addr=0x0`,
  which is usually fatal. The pc moves per build, so resolve it:
  ```sh
  mips64r5900el-ps2-elf-objdump -d build/debug/quake_unstripped.elf > q1.dis
  L=$(grep -n "^  1ba87c:" q1.dis | cut -d: -f1)
  awk -v n="$L" 'NR<=n && /^[0-9a-f]+ </ {f=$0} NR==n {print f}' q1.dis | c++filt
  ```
  **Re-run before investigating.** On the Quake II port, across three identical 39-map
  cycles, one died, one was clean, and one logged it and still finished.

## Host harnesses (runtime logic only; `make` stays the compile check)

- **Renderer sources:** in the scratchpad, make a `fakeinc/` with four headers and compile
  with `-Ifakeinc -I<repo>/src` (quoted includes miss the source's own dir, so `-I` order
  decides): `ps2/common.h` (`MAX_QPATH`, `PS2_QUAKE_DEBUG`, `Con_Printf`/`Con_DPrintf`/
  `Sys_Error` decls, `PS2_Assert`/`PS2_AssertMsg`), `tamtypes.h`, `gs_psm.h` (copy the
  `GS_PSM_*` values), `draw_buffers.h` (a `texbuffer_t`). Don't fake `texture.h`/`vram.h`.
  `#include` the **.cpp** so the test can walk anonymous-namespace statics. Stub `Sys_Error`
  as a counter (fatal paths `return` after it) and make the assert stub `exit(1)`. Use clang
  with ASan/UBSan.
- **Client-side sources** compile against the *real* QuakeSpasm headers on host clang in C++
  mode. Make the fake `ps2/common.h` wrap `#include "quake/quakedef.h"` in `extern "C"`,
  define the globals the source touches, and fake only the hardware class.
- **Emulate the EE FPU** in any math harness: `1/sqrt(0)` must give FLT_MAX, not inf (see
  [ps2-platform.md](ps2-platform.md)). Otherwise target-only bugs won't reproduce.
- **ASan can't see an overrun that stays inside one object** (a member array reading into
  the next member). Heap-allocate the object under test, so writes past its last member hit
  the redzone, and poison what must not be read with `ASAN_POISON_MEMORY_REGION` from
  `<sanitizer/asan_interface.h>` (it handles a partial first granule), unpoisoning after the
  call. Give outputs exact-size heap buffers with canaries. Then prove the harness by seeding
  defects into a shadow copy of the header (`-I<mutant dir>` first): the `half_band.h`
  harness caught 7 of 7 (off-by-ones, an over-long memmove, a wrong tap, a 32-bit overflow).
- Code with EE/VU0 inline asm can't run on the host. Use the standalone test ELF recipe in
  [performance.md](performance.md).
