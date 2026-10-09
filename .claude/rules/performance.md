---
paths:
  - "src/ps2/**"
  - "src/tools/scripts/frame_log/**"
---

# Performance: EE codegen, measuring in PCSX2, recipes

## Where things stand

- Goal: a smooth 60 fps through the whole `ps2_perftest` capture, and on every level of the map
  cycle's perf pass (`ps2_testmaps 2`).
- **Quake 1 baseline (2026-10-08, debug build, sound and music on):** `ps2_perftest` over
  demo1-3 logged 13,170 frames. EE work (Frame - VSync) mean 4.58 ms, p50 3.93, p95 8.02,
  p99 9.97 ms, and at most 14 ms outside the three demo loads (0.9-1.3 s each). **No vsync was
  missed outside the loads:** no frame ran between 25 and 100 ms. About 150 frames read 17.5-21
  ms, and none of them is a miss (see "A Frame over a field" below).
- **After the single-precision math pass (2026-10-09, `q1-math.*`):** EE work outside the demo
  loads (the 4.58 above counts them) went from 4.35 to 3.89 ms mean (-10.5%), p95 8.01 → 7.57,
  p99 9.94 → 9.49 ms. View -3% (`AngleVectors`), Ui -19% (the 2D's `floor`), Sound -3%; the
  rest is in the engine's unprobed frame, where the view's bob, idle sway and gun angles called
  double `sin` every frame. Demos don't run the server, so no capture measures its QuakeC and
  physics math yet.
- **Effects' random numbers on `COM_FxRand` (2026-10-09, `q1-fxrand.*`):** the mean stayed put,
  and each of the 45 frames that spawn an explosion or a teleport splash lost 1.04 ms of EE work
  (10.64 → 9.60 ms): 1024 particles at seven draws each, at 12 cycles a draw against
  `rand()`'s 56. The capture's worst frame went from 13.37 to 12.37 ms. To find such frames in a
  log, look for a jump of 800 or more in the `particles` column.
- **With the engine probes (2026-10-09, `q1-probes.*`)**, steady-frame means: `ClParticles` 258
  µs, `ClScene` 60, `ClParse` 34, `Server` 0 (a demo runs no server), `FsIo` 0, and 321 µs left
  unattributed (`frame_budget.py`'s `rest`). `CL_RunParticles` is the engine's biggest phase
  after the mixer: each particle's `die < cl.time` compares a float with the double clock, a
  soft-float call per particle per frame - the first target for the double-time work. No file
  is opened mid-demo: every one of the 78 opens falls in a demo's start or map load (about 40
  per load: the BSP and its brush models, sprites, monster models, sounds, the track), which
  takes 0.83-1.26 s of `ClParse`, 0.18-0.24 s of it `FsIo`. No steady frame dropped; the
  worst took 12.5 ms. `Music` doubled to 280 µs against `q1-fxrand` because `id1/music` now
  holds the CD rip as 44.1 kHz WAVs with no `.adp` (`make music` encodes them).
- **Clock compares in float (2026-10-09, `q1-timers.*` against `q1-probes-adp.*`, both with
  the `.adp` soundtrack):** the soft-float counter (below) found the demos making ~1,450
  soft-float calls a frame, nearly all of them a float time compared with the double `cl.time`
  once per particle (475), per brush model per dynamic light (397, in view.cpp) and per light
  slot in three loops (130 each). Reading the clock into a float once per loop (engine-c.md
  lists the sites) left ~200. Steady-frame EE work: mean 3.90 → 3.46 ms (-11%), p95 7.58 →
  6.33, p99 9.39 → 7.97, max 12.36 → 11.18 ms; `ClParticles` 257 → 63 µs, `EntBrush` 448 →
  327, `SndMix` 472 → 433 (`CL_DecayLights` runs inside it).
- **Open: the server's collision.** On a live e1m1 the counter found ~9,100 soft-float calls a
  frame, 7,400 of them in `SV_HullPointContents` and `SV_RecursiveHullCheck` (`world.c`): the
  `DoublePrecisionDotProduct` QuakeSpasm uses against stuck-in-wall bugs, run per hull node of
  every trace and point test. No demo capture sees it (a demo runs no server). Going back to
  id's float trades that collision fix away, and the EE rounds toward zero where id's x87
  carried extra precision, so it needs a gameplay check, not just a capture. The map pass below
  measures what the server costs on every level.
- **Every level, live (2026-10-09, `q1-mapperf.*`):** the map cycle's perf pass over the
  registered data, 38 levels and 190 viewpoints, 30,441 steady frames in 10.8 minutes. 32 levels
  never dropped a steady frame, and every episode 1 and deathmatch level stayed under 15 ms of EE
  work. Six dropped 396 frames between them: e4m7 258 of its 801, e2m2 84, e2m3 48, e2m1 4, e2m7
  and e3m1 one each. EE work mean 6.17 ms, p95 12.6, p99 17.5, against the demos' 3.46 mean,
  because a live level runs the server: `Server` averages 3.9 ms a frame over the tours, and is
  14.1 of the 18.9 ms of the average over-budget frame (View 3.4). It grows with the monsters:
  dm4, with none, 0.5 ms; e2m2 10.9 and e4m7 13.1 ms at their means, with the monsters standing
  still under notarget (awake ones cost more). The one outlier on the rendering side is e2m3's
  viewpoint 2, the teleporter exit at (1144 1776 -61): 7,200 triangles, View 5.8 ms, EE p95 24.5
  ms. Nothing was opened mid-level.
- The rest of this section is the Quake II port's *(Q2)*. Reference
  `build/baselines/vwep.flog`: EE work mean 5.4 ms, p99 9.3 ms, max 11.6 ms (debug), 0 dropped
  frames.
- What it took: soft-float doubles in the engine C (`-fsingle-precision-constant` +
  `math_c.h`, 59 → 1 drops); cheaper effects (`Com_FxRand`, an incremental rail spiral,
  inline `V_AddParticle`); preloading mid-level assets (player weapon fire sounds, all view
  weapons in `CL_RegisterTEntModels`); and cheaper file I/O (hashed pak directory, unbuffered
  pak reads, file-sized stdio buffers). A mid-level asset load is the usual cause of a dropped
  frame.
- A release-build EE codegen audit already landed: per-draw chunk heads copied with lq/sq
  (EntShadow -46%, EntGeom -19%), world gather through `ReserveVerts`/`CommitVerts` plus a
  local cursor, a BSP walk that passes clip flags down the tree with an inline corner test
  instead of `BoxOnPlaneSide` (-32%), and the particle gather through a local cursor with
  `sq`. Release View went from 3405 to 2614 µs.
- **Parked: background loading** of mid-level models/skins (a loader thread doing I/O,
  decode, and a Loading state). It is the only fix left for DM player joins and dropped world
  weapons. Decode matters as much as I/O: v_rail's md2+skin cost ~8.9 ms, of which only
  ~1.7 ms was I/O. The design is in the user's plan file
  `~/.claude/plans/i-want-you-to-binary-spindle.md`.

## EE codegen facts

- **Count memory ops, not instructions.** ALU is the cheap half of this machine. A version
  that ran 9 *more* instructions per triangle but did fewer loads and stores was faster.
- **`-fno-strict-aliasing` makes gcc spill and reload pointers around stores.** A local
  `__restrict` copy *at the use site* fixes it
  (`vu1::LerpVertexBytes * const __restrict p = tri.pos;`). `__restrict` on struct/class
  members produces byte-identical code, so don't bother. It isn't a blanket sweep: check the
  disassembly per site, since it pays off where register pressure is high. Declare restrict
  locals *after* any flush call in the loop body. A cursor passed by reference has its
  address taken and stays in memory, so copy it into a local first.
- **ps2sdk `packet2_add_*` costs ~5 memory ops per word** (it reloads `packet->next` around
  every store). In hot emission, take a local `qword_t * __restrict q = pkt->next`, write whole
  qwords, and store `next` back once.
- **gcc never forms `lq`/`sq` itself.** Struct copies of `alignas(16)` types, `__int128`,
  `vector_size(16)`, `mode(TI)` and aligned `__builtin_memcpy` all lower to `ld`/`sd` pairs (a
  Mat4 is a two-trip loop). Use `ps2/qwords.h` (`CopyQwords`, `StoreQword`, `CopyDrawVertex`)
  at memory-to-memory sites. A transparent lq/sq `operator=` on Vec4/Mat4 was measured and
  rejected, because asm memory operands push register-promoted locals back to memory (+56%
  instructions in `DrawAliasMD2Entity`). The reasons are recorded in `vec_mat.h`'s header.
- `__builtin_ctz/clz` are libgcc calls on the EE. A per-batch ctz cost TexChains +9%.

## What a PCSX2 capture can and can't show

- **No EE cache emulation** (`EnableEECache = false`). Cache-locality work (prefetch, slimmer
  structs, UCAB/uncached buffers, reordering) reads as zero. Don't conclude it's worthless.
  To test it, turn the ini flag on (slow, edit with PCSX2 closed) or use hardware.
- **No GS-internal cost.** CLUT loads, fill rate, texture cache and overdraw are free. Only
  data moved (DMA/GIF qwords) and EE/VU1 instructions are charged. For GS-side changes,
  promise "no regression", never a measurable win.
- **PCSX2 charges roughly per instruction** (~2.5-3 cycles per EE instruction seemed to
  apply). Only a net instruction-count cut shows up. An MMI change that swapped 5 memory ops
  for 1 extra instruction measured *slower* (+3.1%) and was reverted, even though it should
  win on hardware.
- Same build, same capture: rendering stages reproduce within ~0.2 µs/frame, and demo1 ends on
  frame 4183 of 7911 every run. SndMix (~1 µs), Frame (~1 µs) and VSync (~6 µs) wander between
  identical runs. **Code layout alone shifts stages by ~1-2 µs** (0.5 KB of init-only code
  moved Particles by -1.9%). To attribute a delta under ~2%, compare against a same-build
  re-run, and if needed against a variant with HEAD's section sizes (`objdump -h`).
- Quote per-item ratios (e.g. EntGeom per triangle), not the EE mean. Run-to-run spread on
  the EE mean is about ±2%.
- **A Frame over a field is not a missed vsync.** The present is deferred: its vsync wait sits
  in `GL_BeginRendering` (`rs::BeginFrame`), in the middle of `Host_Frame`. So a row's Frame
  comes to one field plus however much the work after the wait (the view, the 2D, `S_Update`)
  grew since the last frame. A view turning onto a busier scene reads as an 18-20 ms frame,
  and turning away as a 14 ms one, with every present still on its vsync. A real miss reads
  about 33 ms. The Q1 baseline's ~150 frames over 17.5 ms (2026-10-08) were all of this kind:
  in 147 of the 148 outside the loads, View had stepped up from the frame before, and the
  other sits beside the e1m6 load. The frame log doesn't record the present clock's fields yet
  (`gs::PresentClock`, which the fps overlay reads), so count misses from Frame >= 25 ms.
- PCSX2 timing ≠ hardware for I/O and FPU latency either.

## Capture recipe (`ps2_perftest`, ~4 min, debug build)

1. Back up `id1/config.cfg` and `build/debug/history.txt`, and put `ps2_perftest 1` in
   `id1/autoexec.cfg` (or `ps2_perftest "1"` in the config). `make run`.
2. Wait for `PerfRun: complete` / `FLOG#end` in emulog, then copy `emulog.txt` into
   `build/baselines/<tag>.emulog.txt` before the next launch overwrites it.
3. Restore the config and the history, and delete the `autoexec.cfg`. The run sets
   `ps2_show_fps` and the like to 0, and those are `CVAR_ARCHIVE`, so the quit writes them
   back. Stop PCSX2: after the quit it sits in the BIOS menu.
4. `src/tools/scripts/frame_log/summarize_flog.py <emulog> --rows build/baselines/<tag>.flog`,
   then `compare_flog.py <before> <after>`. `frame_budget.py` realigns Server, ClParse and
   ClScene (they land one row early) and lists each mid-level `FLOG#open` load with its cost
   (`--all-opens` lists the level loads' too). The open notes are written in perf runs only
   (`FrameLogNoteOpens`), 64 per log batch.

- If the notes show `gfx/mainmenu.lmp`, `gfx/qplaque.lmp` or `gfx/pause.lmp` mid-demo, a stray
  host key reached PCSX2's USB keyboard and opened the menu. Re-run with focus away from PCSX2.
- **Running a capture without touching `id1/`:** the scratch-directory recipe in
  [testing-pcsx2.md](testing-pcsx2.md), with `ps2_perftest 1` in its `autoexec.cfg`. The run's
  quit writes `config.cfg` there, but with saves on the card (`ps2_savedevice mc`) it also
  rewrites the card's copy: back up and restore `memcards/Mcd001.ps2`.
- **Profiling a release build:** set `-DPS2_QUAKE_PROFILE=1` in the release `CONFIG_DEFS`,
  `rm -rf build/release/src`, `make release run` with `ps2_perftest 1`. Revert and rebuild
  clean afterwards.
- `build/baselines/` is untracked and local, holding captures, summaries and config backups.

## Every level: the map cycle's perf pass (`ps2_testmaps 2`, debug build)

The demos see three levels; this sees all of them. The registered data's 38 took 10.8 minutes
(about 17 s a level), and the shareware 9 take about 3.

1. Use the scratch-directory recipe, with `ps2_testmaps 2` in its `autoexec.cfg` (the cvar isn't
   archived, so the config can't arm it), and back up the card. Wait for `FLOG#end`, then stop
   PCSX2.
2. The pass sets up the capture as `ps2_perftest` does, then loads each level in the map cycle's
   order. In each it turns `god` and `notarget` on, then visits `ps2_testmaps_views` (5)
   viewpoints: the spawn first, then each time the intermission camera, deathmatch start or
   teleporter exit farthest from those already picked, moved to with `setpos`. At each it writes
   `FLOG#view,<row>,<n>,<kind>,<origin>` and turns a full circle, 2 degrees a frame.
3. `frame_budget.py <log>`: the per-map table names each level's slowest viewpoint, and the 20
   slowest viewpoints follow, with the View, World, Ent, Sky and tris means that say what made
   them slow. A viewpoint's first 15 frames (the teleport, and the textures it uploads) settle
   apart, as a map's first 30 do.

- **Two captures compare row for row** (`compare_flog.py`), as the demos do: a level's
  viewpoints come from its entities and are the same every run, and the turn steps per frame,
  not per second.
- **What it doesn't see:** fights (the demos have them; with notarget the monsters stand where
  the level put them), and anything between the viewpoints.
- Noclip still touches triggers. A viewpoint inside a trigger_teleport is never reached
  (`MapCycle: viewpoint <n> ... was never reached`): the turn happens where the trigger put the
  player. A viewpoint inside a trigger_changelevel ends the level early (`MapCycle: left ...`).
- The tour counts the frames `Host_Frame` ran (`host_framecount`), not the main loop's passes:
  `Host_FilterTime` skips most passes while a level settles, and the first build's turns were
  over in 40 ms.

## Codegen A/B across the backend (no tree edits)

- Baseline: `make release`, then snapshot `build/release/src/ps2/**/*.o` to the scratchpad.
- Take the exact compile line from `make -n <obj>` (or `make -n -W <file>`).
  `SIZE_OPT_CXX_SRC` objects get **`-Os` per object**, and an ad-hoc `-O3` shows dozens of
  spurious diffs. Always build a **control** set with unmodified headers first and require
  zero diffs against the baseline.
- To shadow a header, put the modified copy at `<dir>/ps2/math/vec_mat.h` and pass `-I<dir>`
  *before* `-Isrc`.
- To list every copy/assign site of a type, temporarily `T & operator=(const T &) = delete;` in
  the shadow and compile everything with `-fmax-errors=0`.
- Compare per function (instructions, loads, stores, lq/sq), normalizing branch targets.
  Static counts understate loops, so check loop bodies by hand.

## Counting soft-float calls per call site (~5 min, scratch only)

What a frame log can't show is which code pays for `double`: it runs as libgcc calls
(`__adddf3`, `__ltdf2`, `__extendsfdf2`, ...) inside whichever probe called them.

- Write a C file of wrappers, one per routine the ELF links (`nm` lists them: 20 on
  2026-10-09), e.g. `double __wrap___adddf3(double a, double b) { Count(0,
  __builtin_return_address(0)); return __real___adddf3(a, b); }`. `Count` bumps a hash table
  keyed by return address and kind, and an `atexit` handler prints it as `SFC,` lines.
- Take the debug link line (`make -n -B | grep -- "-o build/debug/quake_unstripped.elf"`), add
  the object and `-Wl,--wrap=__<name>` for every routine, link to the scratchpad and strip.
- Run it as the perf run (its quit runs the `atexit`) or as a live map that ends in `quit`.
- `addr2line -f -i` on each return address minus 8 (the `jal` and its delay slot) gives the
  call site; group by the outermost function and divide by the frame-log row count.
  `CalcSurfaceExtents` shows up large but runs only at map load.

## Proving an EE/VU0 asm rewrite on target (~6 s)

All of this happens in the scratchpad, with nothing added to the repo.

- The test `.cpp` includes the real header plus the HEAD version of the changed functions
  (`git show HEAD:<file>`) inside `namespace ref { ... }`. Qualify ref-internal calls
  (`ref::Foo`), since ADL on shared argument types makes them ambiguous.
- Use deterministic xorshift inputs, `memcmp` every output byte, and print `Tag: PASS/FAIL`.
  Call `SifInitRpc(0)` before `printf`, then `SleepThread()`.
- Build with the release flags. Link:
  `mips64r5900el-ps2-elf-g++ -T$PS2SDK/ee/startup/linkfile -O3 -o t.elf t.o <objs> -L$PS2SDK/ee/lib -Wl,-zmax-page-size=128 -lkernel`
  (add `-lgraph -ldma` if needed).
- Back up emulog, run `PCSX2 -batch -elf t.elf` in a background shell with an `until grep`
  loop on the log (the verdict, or `TLB Miss|EXCEPTION`, plus a deadline), then `kill` the PID
  you started, since SleepThread never exits. Restore emulog.
- The same harness measured vsync timing: an NTSC field is 9609.6 T2 ticks = 59.94 Hz
  (`gs::PresentClock`).
