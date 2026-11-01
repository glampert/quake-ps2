---
paths:
  - "src/ps2/**"
---

# PS2 platform and ps2sdk facts

## Don't trust ps2sdk on sight

- **libc stubs.** libcglue stubs a number of libc functions to fail. `sysconf()` returns -1
  with `EINVAL` for *every* name, `_SC_PAGE_SIZE` included, and `tcgetattr`/`tcsetattr` etc.
  return -1/`ENOSYS`. The vendored dlmalloc took its page size from `sysconf`, got
  `(size_t)-1`, and the ELF black-screened. `dlmalloc.c` now pins
  `#define malloc_getpagesize ((size_t)4096)`. Before relying on a libc call, grep
  `~/ps2dev/src/ps2dev/build/ps2sdk/ee/libcglue/src/glue.c`. Prefer pinned constants to
  runtime libc queries.
- **Register-packing macros.** `VU_GS_PRIM`/`VU_GS_GIFTAG` (packet2_utils.h) don't
  parenthesize their parameters. `blended ? 1 : 0` as ABE parsed as `blended ? 1 : (0 << 6)`,
  which put the bit inside PRIM, where it was already set, so blending silently never
  happened. Use `GIF_SET_PRIM`/`GIF_SET_TAG` (gif_tags.h), which parenthesize and mask.
  `gs_gp.h` is not uniformly safe either. `GS_SET_DIMX` masks the 3-bit signed dither entries
  with `0x3`, which turns the matrix into a brightening bias, and libdraw's
  `draw_dither_matrix` inherits that. Pack DIMX by hand: `(u64)((u32)v & 0x7u) << (i * 4)`.
  Hoist computed arguments into named locals, read the macro body, and if a GS feature
  "isn't enabled", compile the macro standalone on the host and inspect the bits.
- `packet_init`/`packet2_create` each `calloc` a header and `memalign(64)` the buffer. Read the
  SDK source when internal behaviour matters.

## EE CPU and FPU

- **No IEEE Inf/NaN on COP1.** Divide by zero saturates to ±FLT_MAX, denormals flush to zero,
  and rounding is toward zero. `Normalize(zero)` gives `(0,0,0)` on target but NaN on a host.
  This once vetoed valid ears in polygon triangulation: 714 faces with holes on target versus
  4 on host. Guard degenerate vectors explicitly. Never rely on NaN propagation.
- The compiler folds `constexpr` float math in the *target's* round-toward-zero semantics.
  `math::kPI` folds to `0x40490FDA`, not `...DB`. A host harness won't reproduce shipped
  constant bytes exactly, so check the ELF's `.rodata` instead.
- **So `(int)(x * (1/c))` comes out one low for an exact multiple of c.** The folded reciprocal
  sits under the true one, the multiply truncates, and the cast truncates again. QuakeSpasm's
  `anglemod` (`a * (65536/360.0)`, a float under `-fsingle-precision-constant`, `0x43360B60`
  where round-to-nearest gives `...61`) took every angle already on its grid one step down, so
  the view turned clockwise by 360/65536 of a degree a frame, 5.5 degrees every 1000 frames,
  with nothing pressed (fixed 2026-10-13). alias.cpp's shade row (`yaw * (16/360)`) put a model
  facing any multiple of 22.5 degrees in the row before QuakeSpasm's (fixed the same day). Divide
  by c when c is exact in float (360/65536 and 22.5 are): a multiple of c then divides back to
  itself. A test ELF over every angle of the 16-bit grid, both signs, found the old `anglemod`
  wrong on all but zero and the old row on the 30 multiples, and the new ones on none, matching a
  PC build's double math.
- **No double-precision FPU.** `double` is libgcc soft-float. The engine C builds with
  `-fsingle-precision-constant` and `ps2/math/math_c.h`. Backend code uses `f` suffixes, and
  `-Wdouble-promotion` enforces them. Soft-float doubles were the cause of almost all
  dropped frames.
- `__builtin_ctz`/`__builtin_clz` are **libgcc calls** (the R5900 has no count-zeros
  instruction). Use `ps2::tex::Log2` (`plzcw`) or compares in hot paths.

## SIF DMA

- Any EE buffer that is a SIF DMA target (`SifRpcGetOtherData`, `sceSifSetDma`, memory card
  bounce buffers) must be `alignas(64)` and sized in whole 64-byte units. The DMAC ignores
  low address bits, and 64 bytes is the D-cache line, so a neighbour's dirty line can be
  written back over the DMA'd bytes. A plain `static char[128]` only gets 8-byte alignment.
  Check with `mips64r5900el-ps2-elf-nm build/debug/quake_unstripped.elf | grep <sym>`.
  `SyncDCache(buf, buf + size)` before the transfer is the matching half.
- **A file read needs no aligned buffer.** fileXio's `fileXioRead` (USB, HDD) and the ROM
  FILEIO's `fioRead` (`host:`) write the buffer's lines back, DMA only the whole cache lines,
  and copy the unaligned head and tail in their completion callback (`rests` in fileXio_rpc.c,
  `_fio_read_intr` in fileio.c). So `read()` and `fread()` may target any EE memory: the
  compressed packs read a stored file straight into the engine's buffer (fs/pkz_pack.cpp). Only
  their own input buffers are `alignas(64)`, which spares those copies. **Unless another thread
  runs meanwhile:** those head and tail copies land inside the completion interrupt, so a
  destination that shares a cache line with anything another thread touches is corrupted when
  that line is written back. The background loader (fs/async_io.h) therefore takes only 64-byte
  aligned destinations sized in whole 64-byte units, owning their lines (`aio::Submit` asserts
  it). Chunk boundaries inside one destination are safe: both halves of a shared line belong to
  the same request and land in sequence. PCSX2 has no data cache and never shows a violation.
- **`dma_channel_send_chain` writes back only the tag buffer** (`data` to `data + qwc`), never
  the data its REF tags point at. Anything the CPU wrote and a chain REFs needs its own
  `SyncDCache` first, or the DMAC reads stale RAM. PCSX2 has no data cache, so only a console
  shows it, and whether the lines happen to be written back in time varies from build to build.
  *(Q2)* `gs::UploadCluts` missed it until 2026-10-10: a console run drew only untextured fills,
  with everything palettized gone, loading screen and HUD included; with the flush a release
  build drew and played. This port took the fix on 2026-10-13. `UploadTexture` flushes on
  `dirtyPixels`, and the frame chain does a `FlushCache(0)` per kick. `SyncDCache` writes back
  and invalidates every line from `start` to `end` inclusive, so `end` may be one past the data.

## IOP modules, ROM FILEIO, memory card

The boot code (`system/iop_boot.cpp`) is the Quake II port's as of quake2-ps2 9417136
(2026-10-12), brought over on 2026-10-13.

- **The boot, in order.** The `host:` fast path (PCSX2) resets nothing: `sbv_patch_enable_lmb()`
  alone lets `SifExecModuleBuffer` load embedded IRX without disturbing open `host:` handles.
  **Every console boot resets the IOP** and loads the game's own drivers (since 2026-11-01, from
  quake2-ps2 f91aa14: on that port's console the launcher's usbhdfsd, wLaunchELF 4.43a's, froze
  the game after its first level load on every run that went through it, and the boot's behaviour
  hung on whatever a launcher left). From 2026-10-12 to then a console boot took the IOP as the
  launcher left it, and `-DPS2_QUAKE_ALWAYS_RESET_IOP=0` still does, for comparisons: the module
  list says what runs, each module the game needs is reused when one doing its job is there
  (`kModuleIdentities`), and the rest is loaded; the IOP is then reset only for a known conflict
  (`FindResidentConflict`), and as the fallback when a module won't start, when no game data turns
  up through the launcher's drivers, or when the largest free IOP block is under 384 KB. The game
  data is found before `Host_Init`: `host:`, then HDD/PFS, then USB, except that an ELF launched
  from `mass:` looks on USB only.
- **The boot report.** Every step is timed from `main()` (`ps2::sys::Milliseconds` is first
  called there) and kept as `IOP boot:` lines: what the launcher left running, with versions and
  the export libraries found, what was reused, started or reset and why, the HDD probe, the USB
  wait, and the IOP memory left. Each line goes to stdout as it happens, the whole report to the
  log file once the game data is found, and `ps2_dump_iop_mods` prints it again with the module
  list and the IOP memory free.
- **All IOP module loading and every sbv patch live in `iop_boot.cpp`.** Subsystems ask for a
  driver (`StartPadDriver`, `StartMemoryCardDriver`, `StartSoundDriver`, `StartKeyboardDriver`)
  and set up only their EE library; keep new drivers that way. Each is brought up once, and each
  module in it reuses a running one that does the job, the boot's or the launcher's, so the
  keyboard asks for usbd whatever the boot started.
- *(Q2)* **Where a console's boot time goes** (wLaunchELF 4.43a, HDD attached, 2026-10-12): the
  IOP reset takes 175 ms; starting `ps2hdd` 3.4 s, most of it the drive answering
  `sceAtaInit`. A PFS mount pays twice for the disk's layout: opening a partition by name walks
  the APA chain from the start of the disk, a header read per partition ahead of it, and the
  mount reads the partition's free-space bitmap, which grows with its size (a 7.3 GB partition
  mounted in 4.07 s, on a drive holding ~90 other partitions). A USB launch searching every
  partition first took 24.6 s, which is why a `mass:` launch now leaves the HDD alone: with the
  HDD attached it went from 29.0 s to 80 ms, reusing the launcher's usbd and usbhdfsd. The reset
  was never the cost; the HDD is.
- *(Q2)* **What launchers leave on the IOP** (their sources, read 2026-10-12). wLaunchELF before
  2026-05-26 resets the IOP only for an ELF on `hdd0:`. For `mass:`/`mc:` it leaves iomanX,
  fileXio, the ROM's SIO2MAN and PADMAN (ports closed), ps2sdk's `mcman-old`/`mcserv-old`, cdfs,
  usbd and BDM (usbhdfsd before 2022), and the keyboard, HDD and network drivers if it used them.
  4.43a's usbd registers as `usbd`, not ps2sdk's `USB_driver`, so it is recognised by its export
  library. Newer wLaunchELF launches through ps2sdk's elf-loader2, which resets and loads the
  ROM's SIO2MAN, MCMAN and MCSERV; a `LoadExecPS2` launch gets EELOAD's set, with LOADFILE 2.1.
- **Telling modules apart:** `smod_get_next_mod`, plus the name read by SIF DMA, gives each
  module's IRX_ID name and version. The ROM's SIO2MAN, PADMAN, MCMAN and MCSERV are 1.x; the X
  modules and ps2sdk's are 2.x or 3.x. ps2sdk's iomanX has no IRX_ID, so it lists unnamed (`???`
  in `ps2_dump_iop_mods`): find it by its export library, `slib_get_exp_lib("iomanx")` after
  `slib_exp_lib_list()`. `smod_get_mod_by_name` and `slib_get_exp_lib` both match a prefix.
- **`sbv_patch_enable_lmb()` returns 1 without patching** for a ROM LOADFILE already patched,
  and for any LOADFILE it doesn't recognise (2.x has the RPC natively). An LMB call to a
  LOADFILE without the RPC hands back the request buffer, which reads as a module started. So
  after a 1, the boot checks the next module it loads from a buffer shows up in the list.
- **libpad and libmc find the server themselves:** `padInit` binds the old or the new PADMAN
  RPC, whichever exists, and `mcInit` tries XMCSERV's init first. So a resident pad or card
  driver of either generation can be reused, while loading the ROM's next to it can't work: its
  library or RPC server is taken. The ROM's PADMAN and MCMAN link only against SIO2MAN 1.x.
- *(Q2)* **A pad port a launcher left open keeps being written:** PADMAN DMAs the port's state
  into the EE buffer it was opened with. In PCSX2 a test launcher's buffer landed in the game's
  `.text` until the boot closed the port (`StartPadDriver`). LaunchELF closes its ports first.
- **IOP memory for the later drivers:** the sound, pad and card drivers and the keyboard took
  324 KB of the largest free IOP block in PCSX2 (1284 KB after a USB boot, 960 KB with all of
  them up; debug build, 2026-10-13; 312 KB on the Quake II port). The 64 KB fileXio read buffer
  left 1220 KB after the boot. No RPC reports free IOP memory; `LargestFreeIopBlockBytes()`
  halves its way there with `SifAllocIopHeap`.
- **`fileXioSetRWBufferSize` frees the old buffer before allocating the new one**, and on
  failure keeps a null buffer and the new size (fileXio_iop.c), so every later read would DMA
  from IOP address 0. On a nonzero result `SetFileXioBuffer` (iop_boot.cpp) sets the stock
  16 KB again. Read sizes per layer are in [performance.md](performance.md).
- **HDD drivers and limits:** ps2dev9 → ps2atad → ps2hdd → ps2fs (the standard `ps2hdd.irx`
  already uses iomanX; `ps2hdd-iomanx.irx` is a virtual-disk variant). Missing hardware or a
  failed HDD driver falls through to USB: with PCSX2's HDD off, ps2dev9 doesn't start (result 1)
  and the boot goes on to USB 2 ms later. *(Q2)* **Don't skip them on a console that has an
  HDD:** a test build that went straight to the USB chain hung on a black screen at boot on the
  user's console with an HDD attached (2026-10-11), and the same code with the probe back booted
  from USB. Not diagnosed; PCSX2 with an HDD image didn't reproduce it. To test USB there, move
  the HDD's copy of the game aside. APA's default single open handle can't list
  partitions while PFS has one mounted, so the boot passes `-o 4 -n 20`; PFS's default two open
  files can't cover the paks, the music, the log and a load, so it passes `-m 1 -o 16 -n 40`
  (NUL-separated). PFS's cache must be at least `2 * maxOpen + 8`.
- **HDD selection:** the launch partition is kept from `argv[0]` before any reset
  (`hdd0:+PART:pfs:/dir/quake.elf` or the browser's `hdd0:/+PART/dir/quake.elf`) and mounted
  read/write at `pfs0:`, so the log can be written. Other partitions are listed from
  `hdd0:`/`hdd1:` (`HDIOC_STATUS == 0`, `APA_TYPE_PFS`, no `APA_FLAG_SUB`); the launch folder on
  every partition before any root. The winning mount stays up for the whole run.
- **PFS close does not always persist metadata.** With the read/write mount flag 0, a close
  leaves dirty inode and directory metadata cached (`PFS_MT_ROBUST`, 0x02, would flush it).
  `fileXioSync("pfs0:", 0)` does: `SyncGameDataDevice()` runs it after each log write, and
  returns 0 without an RPC on `host:` and USB.
- **IOP thread priorities decide who waits on a file read.** The IOP kernel has no time slicing:
  a thread runs until it blocks or a higher-priority one (a lower number) becomes ready, and an
  interrupt's `iSignalSema`/`iWakeupThread` switches at its exit. fileXio's RPC thread is 40, and
  a HDD read keeps it running (dev9's `SpdDmaTransfer` spins on the DMA with interrupts on, by
  design), so any other server at 40 waits for the whole read; ps2sdk's audsrv was one (its RPC
  thread 40, its play thread 39). The game's audsrv runs its RPC thread at 39 (audio.md). Check a
  module's `CreateThread` priority before routing per-frame RPCs through it.
- **`mass:` is `mass0:`.** iomanX reads a missing unit number as 0. bdmfs_fatfs serves the N-th
  FAT/exFAT volume it mounted as `massN:` (N < 10), one per partition, in mount order;
  usbmass_bd drives two USB drives at once. The boot probes all ten.
- **Writing a file on USB: only `close()` puts it on the drive.** FatFs updates a file's size in
  its directory entry only on sync or close, bdmfs_fatfs's `sync` returns EIO and libcglue's
  `fsync()` is ENOSYS. `debug/log_file.cpp` opens, appends and closes on every write for this
  reason. *(Q2)* Over USB on a console that cost ~23 ms per logged line, so a load timed with
  the log on mostly measures the log; PCSX2's USB image took 12 to 30 ms a line. A successful
  `close()` on USB or the HDD returns the iomanX slot, not 0 (libcglue passes iomanX's result
  through); only a negative one is a failure. `host:` returns 0, which hid that in PCSX2.
- **A fatal report is written with `ps2_logfile` off too** (2026-11-01, from quake2-ps2 25d0d57:
  its 2026-10-27 out-of-memory screen on a console left no log because the cvar was off).
  `LogFileWriteFatal` forces the write, and the 16 KB backlog the log keeps while off (the boot
  report, the last lines printed) goes out ahead of it, so `quake.log` after a crash holds the
  context and the report.
- **On a console, `quake.log` is the only report.** Fatal reports (the pipeline hang dump,
  stack traces, the out-of-memory stats) go through `ps2::debug::DumpPrintf` (log_file.h), to
  stdout and the log. Anything new printed on a fatal path must use it, not `printf`. *(Q2)* The
  first hardware pipeline hang (2026-10-07) logged only `Sys_Error: Render pipeline hang: ... See
  the pipeline dump above.`, because the dump went to stdout alone.
- *(Q2)* **A hardware pipeline hang ended on a solid green screen**, with no `Sys_Error` text
  drawn, though the log had the `Sys_Error` line. Likely, but unconfirmed: `ScrInit` resets the
  GS, then its PATH3 transfers can't get through a GIF still holding PATH1/PATH2 open, so
  `DmaWaitGif` spins. `ScrInit` now runs `ResetGraphicsPaths` first (scr_print.cpp), in libgs's
  `GsResetPath` order: stop DMA channels 1 and 2 under a DMAC suspend, reset VIF1, VU1 (if the
  thread has COP2), then the GIF. PCSX2 still draws the error screen after it, but can't wedge
  the GIF, so whether it cures the green screen needs the next hardware hang.
- The console boot finds the ELF's folder from `argv[0]`, whatever the loader's spelling
  (`mass:/dir/quake.elf`, `mass0:dir/quake.elf`, `hdd0:__common:pfs:/...`), and tries it on every
  `massN:` unit before the roots. PCSX2 passes `host:` and the ELF's absolute host path, even
  with `HostFs` off, so a PCSX2 run never takes the `mass:` launch's USB-only path.
- `rom0:FILEIO`'s `remove()` RPC handler lacks a `break`: every `remove(path)` on the fio
  backend also runs `mkdir(path)`. `sbv_patch_fileio()` fixes it (applied on the host: path;
  `ps2::sys::FileIoRemovePatched()` reports it). fio has no `rename()` (`ENOSYS`). The USB
  boot uses fileXio, which has neither problem.
- The pad uses rom0 SIO2MAN + PADMAN, so cards use **rom0 MCMAN + MCSERV** (same family).
  ps2sdk's own mcman.irx needs its own sio2man and won't link (its `mcman-old.irx`, which
  wLaunchELF leaves running, does link against the ROM's). iop_boot's `LoadRomModuleOnce` loads
  ROM modules, since SIO2MAN must not load twice; its driver bring-ups are one-shot.
- **A camera that turns on its own can be a worn stick.** *(Q2)* A worn DualShock 2 can rest
  more than 0.15 of full deflection off centre: the user's old pad turned the camera with the
  stick released on a console (2026-10-11), which PCSX2 with a modern host pad never shows, and
  wider dead zones (0.25) fixed it. Here QuakeSpasm's circular dead zones went from 0.175 to 0.25
  for it (input.cpp, 2026-10-13). A failed `padRead` (no fresh data) keeps the last state,
  sticks included, until a read succeeds (pad.cpp). `joy_enable 0` takes the sticks out: a turn
  that goes on then (and with a menu up, which the sticks never steer) is the engine's, as
  `anglemod`'s was (above).
- **`mcInit` spins forever if MCSERV isn't running** (a SifBindRpc loop). Call it only after
  every module load succeeded.
- libmc with the old MCSERV: no `mcRename`. "Unformatted" comes from `mcGetInfo`'s result
  (-2), and the first `mcGetInfo` after insertion returns -1. `mcOpen` takes **IOP flags**
  (`FIO_O_RDONLY` 1, `WRONLY` 2, create `0x200`), not newlib `O_*`. There is no `O_TRUNC`, so
  delete then create. At most 3 open handles.
- The PS2 BIOS browser ignores the icon texture's alpha bit: transparent texels draw black.
