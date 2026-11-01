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
- **The server's collision was double (until 2026-10-10).** On a live e1m1 the counter found
  ~9,100 soft-float calls a frame, 7,400 of them in `SV_HullPointContents` and
  `SV_RecursiveHullCheck` (`world.c`): the `DoublePrecisionDotProduct` QuakeSpasm uses against
  stuck-in-wall bugs, run per hull node of every trace and point test. No demo capture sees it
  (a demo runs no server). It is back to id's float now (engine-c.md); the EE rounds toward
  zero where id's x87 carried extra precision, so a stuck spot found in play gets fixed where
  it is.
- **e4m7, the worst level, split (2026-10-10, scratch builds, god and notarget, 300 frames at the
  spawn and 300 at the teleporter exit):** the server took 47% of the EE and held the game at
  29.8 fps, and 14.4 of its 15.7 ms a frame were collision. The largest part is
  `SV_CheckWaterTransition`, a point test per monster per frame (55 a frame at 148 µs each, 8.2
  ms); then QuakeC's traces and point tests (3.5 ms), the player's `SV_SetIdealPitch` traces
  inside `SV_SendClientMessages` (about 2 ms) and the rest of the engine's traces. QuakeC's own
  work was 0.5 ms. A point test walks about 30 hull nodes at 14 soft-float calls each: 822,000
  calls a second, 96% of them in `SV_HullPointContents` and `SV_RecursiveHullCheck`. Built with
  id's float `DotProduct` in its three places, a point test took 9.3 µs and a trace 21 (16
  times less), the server fell to 11% of the EE, and e4m7 ran at 59 fps in both windows, with
  ~1,000 soft-float calls a frame left.
- **GCC moved `SV_Physics`'s `svtime = sv.time` into the entity loop:** a `__truncdfsf2` per
  entity, 340 calls a frame at e4m7, where the clocks work meant one a frame. It keeps the
  double in a register across the loop's calls and redoes the conversion, which it costs as
  one instruction, at each use. `volatile` on `svtime` fixed it. The other once-a-frame clock
  reads (`CL_RunParticles`, `CL_DecayLights`, `R_PushDlights`, `CL_ReadFromServer`, view.cpp)
  convert once, before their loops (checked in the disassembly, 2026-10-10).
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
- **Every level with float collision (2026-10-10, `q1-mapfloat.*` against `q1-mapperf.*`, the
  same 34,235 rows):** dropped steady frames 396 → 25, EE work mean 6.17 → 3.20 ms, p95 12.6 →
  5.6, p99 17.5 → 7.2. `Server` mean 3.94 → 0.97 ms (-75%), p95 9.95 → 1.60; View, World and
  tris within 0.5%. 37 levels never drop a frame, and their slowest frame is 10.1 ms (e3m5).
  e4m7's p50 went from 14.3 to 3.3 ms, e2m2's from 12.9 to 3.3. The 25 drops left are all
  e2m3's viewpoint 2, now a rendering problem: View 17-18 ms (World 6.5) in its worst frames,
  facing the most of that area's geometry.
- **Chain overflows carry on in the other half (2026-10-13, from quake2-ps2 d9f31d0).** A frame
  that outgrew its 512 KB half used to kick, wait for the GS to draw it all and rebuild in the
  same half. *(Q2)* On a console that cost ~5 ms a frame, 80% of the dropped frames under NTSC;
  PCSX2 charges it ~2 ms. Now it kicks and carries on in the other half, and the halves are 768
  KB. Over the map pass (`q1-chain.*` against `q1-group1.*`, the same 34,235 rows): frames
  overflowing 620 → 90 (overflows 742 → 139), dropped steady frames 26 → 19, GsWait mean 11 → 6
  µs, every other column within noise. The overflows left are the heaviest views, e2m3's
  teleporter exit above all (frames building up to 2.2 MB).
- **A deliberate kick after the world pass (2026-10-13, from quake2-ps2 d0b53b4):**
  `rs::KickBuilt` in `view.cpp`'s `RenderView` sends the sky and the world while the EE builds
  the entities, the water, the particles and the 2D. It never waits mid-frame: it's skipped
  while anything is in flight. *(Q2)* On a PAL console forced to NTSC it took the perf test from
  56.7 to 59.9 fps over gameplay, dropped frames from 5.8% to 0.4%, and GsWait from 5.3 to 3.4
  ms a frame. PCSX2 charges the GS nothing, so here it shows only its cost: over the map pass
  (`q1-kick.*` against `q1-chain.*`) two kicks a frame instead of one, DmaSend + DmaFlush ~11 µs
  more a frame, dropped frames unchanged (19).
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
- **Background loading (2026-11-01, from quake2-ps2 3873e6f..d8add53):** the loader thread
  `ps2::aio` (fs/async_io.cpp), `COM_LocateFile` and the `ps2_testaio` self-test, with the sleeping
  vsync wait (gs.cpp) that gives it the frame's idle time. The Quake II port's console captures
  (its performance.md) are the measurements behind the design: mid-level loads there were 95% I/O
  (41-58 ms from the HDD, 91-147 ms from USB for a model, a skin or a sound), the drive 5 ms per
  SCSI command plus 900 KB/s over USB 1.1, 1.35 or 6-8 MB/s from the HDD, and a 32 KB
  cluster-aligned chunk one command. This port's self-test in PCSX2 from the pkz: 24 comparisons,
  0 failures, 2.1 MB landed in 634 ms as the drive is and 3.1 s throttled to 900 KB/s, the
  main-thread probe read 2 ms at worst beside 139 chunks in flight (`emulog`, 2026-11-01). The pak
  and stored-pkz kinds want a run without the pkz.

## Loading

- **What sizes a drive read** (the IOP side of every `read()` on `mass:`/`pfs:`):
  - fileXio serves it in pieces of its IOP buffer, each an `iomanX_read` plus a DMA to the EE,
    waited out before the next. Stock size 16 KB; the boot sets 64 KB
    (`PS2_QUAKE_FILEXIO_BUFFER_KB`, iop_boot.cpp) once the drive's modules are loaded.
  - PFS reads a whole run of contiguous zones in one transfer, up to the piece size
    (`fileTransfer`, pfs_fio.c). So on the HDD the buffer sets the ATA transfer size.
  - FatFs (bdmfs_fatfs, R0.16) ends every direct read at a cluster's end (`f_read`'s "Clip at
    cluster boundary"), and usbmass_bd caps a SCSI command at 128 sectors (64 KB). So a USB read
    is at most one cluster, whatever the buffer. macOS formats a 16 GB stick with 8 KB clusters;
    `newfs_msdos -F 32 -c 64` makes them 32 KB (`diskutil` can't choose).
  - BDM caches reads under 8 sectors (`bd_cache.c`, 32 × 4 KB), so FAT walks for seeks come
    mostly from cache. FatFs has `FF_USE_FASTSEEK 0` and `FF_FS_TINY 1`.
- **stdio buffers 1 KB here, so the engine's file handles are unbuffered** (2026-10-18). This
  newlib is built without `HAVE_BLKSIZE` (its `mips*-ps2-*` entry in configure.host), so stdio
  ignores `st_blksize` and gives every `fopen`ed file a `BUFSIZ` buffer, 1024 bytes, and a
  buffered `fread` goes through it a KB at a time: loading e1m2.bsp (1.3 MB) from the pak was
  about 1,300 file server round trips. `Sys_FileOpenRead` now makes its handles unbuffered (they
  only ever read a whole file, or a pak's header and directory), so a pak file is one seek and one
  read, straight into its memory, as *(Q2)* the Quake II port's are. In PCSX2 (release + I/O log,
  three runs) the e1m1 to e1m2 change went from 1.60 to 1.23 s, its file calls from 0.39 to
  0.02 s. The streams `COM_FOpenFile` opens (a demo, QuakeSpasm's `FS_*` readers) keep the 1 KB
  buffer. The compressed packs read their own descriptor raw, a whole stored file or 32 KB of
  compressed input at a time.
- **Compressed packs in PCSX2** (2026-10-18, release + I/O log, the e1m1 to e1m2 change, three
  runs each, every one the same to 0.01 s): from `pak0.pkz` + `pak1.pkz` the load read 1.30 MB for
  the 2.77 MB it delivered and took 1.50 s, 0.29 s of it in file calls, 0.27 s of those
  inflating; from the paks, with the unbuffered handles above, 1.23 s, 0.02 s in file calls. In
  PCSX2 a read costs next to nothing and inflating doesn't, so the pak is the faster there, as
  *(Q2)* in the Quake II port. Inflating ran at about 10 MB/s of output, as PCSX2 runs the EE (no
  cache); *(Q2)* a console spent about 1.2 s of base2's load inflating. A debug build also checks
  each pkz file's CRC. PCSX2 models neither drive, so none of this is a console's figure.
- *(Q2)* **What the buffer and the clusters bought** (2026-10-11/12, a PAL console, release + I/O
  log, base2 from the pak): over USB with 8 KB clusters and the 16 KB buffer 58.6 s; with 32 KB
  clusters and the 64 KB buffer 40.4 s; from the HDD (PFS, 64 KB buffer) 15.8 s. Fitting the USB
  reads to *time = commands × c + bytes / B* gives c ≈ 5 ms a command and B ≈ 900 KB/s, near USB
  1.1's ceiling; small files still cost ~4-5 ms of command overhead each. A large read off the
  stick ran 460-580 KB/s with 8 KB clusters and 650-790 KB/s with 32 KB. The HDD read at ~1.35
  MB/s or 6-8 MB/s, the fast runs following a read of the bytes just before them (the drive's
  read-ahead).
- **Timing loads on a console:** a release build with the I/O log,
  `rm -rf build/release/src && make release CONFIG_DEFS='-DPS2_QUAKE_DEBUG=0 -DPS2_QUAKE_ASSERTS=0 -DPS2_QUAKE_PROFILE=0 -DNDEBUG -DPS2_QUAKE_IO_LOG=1'`,
  then the same `rm -rf` and a plain `make release` afterwards. Start each load by a level change
  or from the load menu: a `map` (the console's, or New Game) raises no plaque and isn't logged.
  The file is written when a load ends, and the in-game console says where. Keep `ps2_logfile`
  off: its writes would land in the load. A console whose HDD holds the game data loads from it;
  move that copy aside to time USB (don't skip the HDD drivers: ps2-platform.md). PCSX2 models
  neither drive's speed: there the e1m1 to e1m2 change spent 0.39 s of 1.70 s in file calls from
  `host:`, and 3.41 s of 4.86 s from its USB image (2026-10-13), neither a console's figure.

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
- *(Q2)* **What a console showed against PCSX2, on the same debug ELF** (the first console
  capture, 2026-10-10): EE work (Frame - VSync - GsWait) was 1.56x PCSX2's, 7.2 against 4.6 ms
  (TexChains 1.95x, World 1.6x, LmChains 1.46x, EntGeom 1.3x, Sound and SndMix 1.65-2x): the
  cache. GsWait was 5.4 ms a frame against 0.05: the GS and VU1 took ~12-13 ms a frame for ~4K
  triangles, which PCSX2 charges nothing. An emergency chain drain cost ~8.6 ms a frame there,
  against ~2 ms emulated. The guard band's width cost the GS nothing measurable (GsWait 5.39 ms
  at 0.25, 5.42 ms at 0.8).
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

1. Put `ps2_perftest 1` in `id1/autoexec.cfg` (or `ps2_perftest "1"` in the config). `make run`.
2. Wait for `PerfRun: complete` / `FLOG#end` in emulog, then copy `emulog.txt` into
   `build/baselines/<tag>.emulog.txt` before the next launch overwrites it.
3. Delete the `autoexec.cfg`. The run turns the `ps2_show_*` panels off and puts them back before
   its quit, so the config comes out as it went in (checked 2026-10-12); armed from the config,
   `ps2_perftest` comes back as 0. Stop PCSX2: after the quit it sits in the BIOS menu.
4. `src/tools/scripts/frame_log/summarize_flog.py <emulog> --rows build/baselines/<tag>.flog`,
   then `compare_flog.py <before> <after>`. `frame_budget.py` realigns Server, ClParse,
   ClScene and Input (they land one row early) and lists each mid-level `FLOG#open` load with its cost
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
- **Capturing on a console:** the frame log goes to stdout, which nobody reads on a console.
  Build with `PS2_QUAKE_FRAME_LOG_FILE=1` (profile.h, shipped as 0) and each 64-frame dump is
  also appended to `id1/frame_log.txt` with one open, write and close, in the frame the log
  already drops. A profiling release for a console capture:
  `rm -rf build/release/src && make release CONFIG_DEFS='-DPS2_QUAKE_DEBUG=0 -DPS2_QUAKE_ASSERTS=0 -DPS2_QUAKE_PROFILE=1 -DPS2_QUAKE_FRAME_LOG_FILE=1 -DNDEBUG'`,
  then the same `rm -rf` and a plain `make release` afterwards. Keep `ps2_logfile` off for a
  capture: every console print through it is a USB write inside the frames being measured. The
  perf test and the map cycle allow 240 s for a level load, a console's over USB.
- **A PAL console runs at 50 Hz** with `ps2_video_mode auto`, the console's own standard, so its
  frames have 20 ms: `frame_budget.py --pal` budgets against that field, where NTSC's would call
  every one of them over. PCSX2's BIOS here is NTSC; `ps2_video_mode pal` in the config
  reproduces a PAL console's pacing and its 512 lines. *(Q2)* The PAL console forced to NTSC ran
  the perf test at 56.1 fps over gameplay, with 6.8% of steady frames dropped against 0.77% at
  50 Hz: the same work per frame, a smaller budget.
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
   apart, as a map's first 30 do. `vramUsedKB` is the texture heap's committed VRAM as each frame
   ended: its maximum over a level is the level's VRAM peak, which the `MapCycle` lines print too
   (testing-pcsx2.md), and `summarize_flog.py` shows the run's. `vramEvicts` is the frame's LRU
   evictions: a level with none fit the heap whole, and its peak is all it needed.

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
- **To count a window rather than the whole run**, also wrap `Con_Printf` (format into a buffer,
  pass it on as `"%s"`), and have it start and stop the counting on a marker the script
  `echo`es after the level has settled.

## Splitting the server's time (scratch only)

- Wrap `PR_ExecuteProgram`, `SV_Move`, `SV_PointContents`, `SV_TruePointContents`,
  `SV_RunClients`, `SV_Physics` and `SV_SendClientMessages`, timing each with `mfc0 $9`, and
  book the traces and point tests made while QuakeC runs apart from the engine's (a depth count
  in the `PR_ExecuteProgram` wrapper; time only its outermost call). Windows as above.
- `--wrap` misses calls made inside the defining file: world.c's `SV_TouchLinks` runs QuakeC
  and `SV_TestEntityPosition` traces. Link a scratch copy of world.c that calls the wrappers
  there; the same copy with `DoublePrecisionDotProduct` turned into `DotProduct` is the
  float A/B.
- Compare builds per call and per game second, not per frame: a build held to 30 fps does twice
  the QuakeC thinks a frame, while the per-frame work (the water checks) stays the same.

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
