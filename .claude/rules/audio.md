---
paths:
  - "src/ps2/audio/**"
  - "src/tools/host/musenc.cpp"
---

# Audio and CD music

## Sound (`snd.cpp`, `audsrv_device.*`, `mix_ring.*`)

The README's "Sound" section has the design. The facts behind it:

- **audsrv's IOP thread never checks its queue for data.** It takes a feed (512 samples at
  48 kHz: 235 frames, 10.7 ms, at 22050 Hz) every tick whether or not the EE wrote any, so a
  queue that runs dry replays the stale ring (ten feeds, 106 ms) in a loop: a buzz through every
  level load. The Quake II port fed it once a frame from the main thread and has that bug. Here
  the feeder thread pads with silence instead. Read `iop/sound/audsrv/src/audsrv.c` in the SDK
  sources before changing anything about the queue.
- **audsrv doesn't play until the first `audsrv_play_audio()`.** `audsrv_set_format` leaves
  the ring half full (1175 frames at 22050) with `playing` off, and the read cursor stays put
  until a play call turns it on. A feeder that only tops the queue up to 940 frames never made
  that call, and the game stayed silent. `AudsrvDevice::Init` sends one frame of silence.
- **audsrv's EE library is single-threaded:** every call goes through one static `sbuff` and
  one RPC client. Only the feeder calls it once `SNDDMA_Init` returns.
- **The R5900 has no LL/SC.** The feeder and the main thread share only single loads and
  stores, each word written by one thread only. No read-modify-write across threads. The words
  are `volatile`, as `music_stream.h`'s are: the EE has one core, so the only reordering to rule
  out is the compiler's. They were relaxed `std::atomic`s until 2026-10-13, which compiled to the
  same `lw`/`sw` (the objects differed only in scheduling), but an atomic invites a `fetch_add`,
  which GCC can only make a call to `__atomic_*`/`__sync_*`. C++20 deprecates `+=` and `++` on a
  `volatile` (`-Wvolatile`), so the one writer adds as `v = v + x`.
- **Timer alarms:** `SetTimerAlarm`'s handler runs inside ps2sdk's T2 interrupt handler, and
  returning a nonzero interval re-arms it (`timer.c`), so one call gives a periodic tick.
  Don't call `ExitHandler()` in the callback: the T2 handler does it once its alarm list is
  done (`DelayThread`'s callback does, which kernel.h's comment says not to).
- **Verified by trace (PCSX2, 2026-10-08):** the feeder woke every 5.008-5.012 ms, through a
  `map` load too, so a higher-priority thread woken from the alarm does preempt the main
  thread. The submit cursor advanced at 22050 frames a second (`soundinfo` twice, 10 s apart),
  and during a load the queue bottomed at 235 frames while silence was padded in.
- **PCSX2 can't dump its audio here.** `[SPU2/Debug] Log_WAVE_Output` writes nothing in the
  release PCSX2 build. Checking the sound itself needs the user's ears.
- **Rate (22050):** see the README. audsrv's upsamplers are sample-and-hold lookup tables
  (`upsamplers.c`), so a low output rate also images more on the way to 48 kHz.
- **Mid-level sound effects load in the background** (`sound_load.{h,cpp}`, 2026-11-01, from
  quake2-ps2 4b0ef5f): `S_LoadSound` outside `CL_ParseServerInfo`'s precache pass (`snd_precaching`,
  set by the `S_BeginPrecaching`/`S_EndPrecaching` stubs: a `map` or `playdemo` raises no plaque)
  and with the plaque down submits the WAV to `ps2::aio` at `Priority::Sound` into a 64-aligned
  staging buffer (tag `Aio`, 8 in flight at most), and `aio::Pump`, just before `Host_Frame`,
  finishes it (`S_FinishLoadSound`: GetWavinfo, `Cache_Alloc`, `ResampleSfx`) so that frame's
  `S_Update` mixes it. A `S_StartSound` for a sound still loading is kept (8 at most) and replayed
  when it lands if that is within 500 ms, else dropped; `S_PaintChannels` already skips a channel
  whose `S_LoadSound` is NULL, so an ambient that is still loading plays once it lands. `S_Shutdown`
  and every level change (`PS2_AsyncIoDrain` in `Mod_ClearAll`) drain first, dropping the deferred
  starts. In PCSX2 (e1m1, `menu_main`) `misc/menu2.wav` landed 14 ms after the menu asked for it,
  and nothing else of a level goes through it: the stock game precaches every sound a level plays.
  Without the precache gate the `map` command's precache sent 8 sounds to the loader, landing
  0.8 s later, since that path has no plaque.
- **Cost (PCSX2, debug, demo1, frame log):** `SndMix` (all of `S_Update` on the main thread)
  635 µs a frame when the main thread submitted, 415 µs with the feeder. `Sound` is now the
  feeder's wall time per frame, ~450 µs, mostly spent blocked on the IOP. demo1's sounds take
  3.26 MB of the hunk's cache (122 sounds, all but one 8-bit).

## CD music (`cd_audio.cpp`, `music_stream.*`, `half_band.h`, `spu_adpcm.h`, musenc)

The README's "Sound" section has the design. The streaming, decoding and rate conversion came
over from the Quake II port unchanged; the facts marked *(Q2)* were measured there.

- **Format (the user's choice, on the Quake II port):** SPU2 ADPCM, 22050 Hz stereo (= the
  mixing rate, so no resampling), 2048-byte chunk interleave so the files stay
  SPU2-voice-streamable later, and a 2 KB `"Q2MU"` v1 header, kept so either port's files play
  in both. Files are `id1/music/trackNN.adp`, always lowercase. musenc does the exact 2:1
  decimation itself (255-tap Kaiser sinc). `spu_adpcm.h` is shared by game and encoder, so it
  uses `<cstdint>`. *(Q2)* Quake II's 10 WAVs (289 MB) encoded to 41.36 MB, at 26-42 dB SNR
  (dense tracks lowest).
- **Pipeline (the user's choice): decode on the EE**, not IOP → SPU2 voices. ps2snd.irx is
  buggy: it re-reads the header as ADPCM, `DeleteThread`s itself at EOF, and doesn't loop. And
  audsrv owns SPU2 core 1 and zeroes core 0's MVOL. A hardware-voice path means a custom IRX,
  which is deferred.
- **Mechanics:** two read buffers from `TryAllocAligned(64)` under `MemTag::Music`, allocated
  when a track opens and freed when it closes, each filled by a background-loader request
  (fs/async_io.h) at `Priority::Stream`, submitted the moment the decode side releases it and
  polled when decode comes back round (2026-11-01, from quake2-ps2 df349b3; a reader thread of
  its own at the main thread's priority before, which yielded with `RotateThreadReadyQueue`).
  ADPCM requests are 32 KB, one loader chunk, ~1.3 s: the 2 KB header leaves every stereo group
  2 KB off a 4 KB boundary, so each request crosses one 32 KB boundary of the file and takes two
  reads, against five per 32 KB for the old 8 KB requests. Requests are marked `sequential`, so
  the loader skips the lseek between them. A request the loader declines (`ps2_aio 0`, every slot
  taken) is read on the main thread when decode needs it: `ps2_aio 0` then `music track04` played
  (PCSX2, 2026-11-01). `map` and `disconnect` close the stream (`CL_Disconnect`); a `changelevel`
  doesn't (`Host_Reconnect_f`), so the old track and its 64 KB stay open through the load until
  the new map's track replaces it. `BGM_Update`
  (in the main loop just before `S_Update`) tops `s_rawsamples` up to `paintedtime + 7680`,
  the raw-sample channel `bgmusic.c` used, so it costs no SIF bandwidth of its own.
- **A main-thread file call waits out a music read.** The SDK file client and the IOP's file
  server take one request at a time, so anything the main thread reads or writes while the
  loader has a read out waits for it. *(Q2)* On a console USB drive that took ~300 ms: the USB
  keyboard poll, a `read()` on `usbkbd:dev` every frame, stalled a frame every 640 ms in a
  2026-10-10 console capture (83 s of 171 s of gameplay), and a run with the keyboard off, then
  one with the fix, confirmed it. Here too (2026-10-13), `Keyboard::NextEvent` skips while
  `aio::Pending()` (`audio::MusicReadPending()` until the music moved onto the loader) and the
  driver keeps the keys queued; `Keyboard::PollDue` reads
  only four times a second unless a key arrived in the last 10 s or is held, and `in_keyboard`
  is off by default in release builds. Keep any other per-frame file I/O off the main thread the
  same way. PCSX2's `host:` reads are too quick to show it. In PCSX2 (debug, e1m1, 2026-10-13)
  a poll costs ~90 µs, on one frame in 14 when idle: the `Input` column (the pad's polls too)
  averaged 70 µs a frame with the keyboard on, against 63 with it off.
- **QuakeSpasm's music semantics, kept:** `BGM_PlayCDtrack` stops the music and tries
  `CDAudio_Play` first (it always declines here); the music holds still while `bgmvolume` is
  0 or the game is paused (`svc_setpause`), with the queued ~350 ms playing out; `BGM_Stop`
  drops what is queued. `bgmloop` (`music_loop`) decides looping: a track opened looping
  never finishes, `music_loop 0` mid-track ends it after the pass (`StopLooping`), and
  `music_loop 1` while a once-only track plays reopens it when it ends. Not kept:
  `music_jump` (tracker orders) and the `cd` command (no disc). Added: `bgm_extmusic` acts at
  once. "Couldn't find a cdrip for track N" prints once per track, not on every map.
- **Holding the music through a stall (`cd_audio.h`, for the save notice):** a card save or
  load stops the game drawing and mixing for a second or more, and the music used to stop dead
  there. `HoldMusic` fades out what is already queued in `s_rawsamples` in place, ahead of the
  mixer (about 330 ms of it: the pump keeps the ring 7680 frames ahead), so nothing decoded is
  dropped and `ReleaseMusic` picks up where the fade ended. The fade is 500 ms, or as long as
  what is queued if that is longer: past the queued frames the pump decodes on, the ramp laid
  over what it stores, and stores nothing past the fade's end. HoldMusic then paints the mix
  itself (`S_ExtraUpdate`, every 5 ms) until `soundtime` is 50 ms past the end (audsrv's queue is
  43 ms deep), so the card isn't touched until the fade has been heard out. With
  `snd_noextraupdate 1` nothing paints: it gives up a second late, and the music is cut as
  before. `ReleaseMusic` arms a 1 s fade-in, laid on the first frames stored after it; closing
  the track drops it, so a load's new track starts at full volume. Sound effects aren't faded:
  they play on through the fade, then stop when the feeder runs out of painted audio, as at any
  stall.
- **What the mixer has painted ahead (`_snd_mixahead`, 0.1 s) is all that covers the main
  thread before the fade begins.** In PCSX2 the card probe and the notice's frame took 98 ms
  together (about 200 on the session's first card access, which starts MCMAN and MCSERV), and
  the save text written out before the notice took over 400 ms: each time the music ran out of
  painted audio before its fade. So the notice goes up before the engine writes the save, and
  `BeginSaveNotice` tops the mix up (`S_ExtraUpdate`) before the probe and before the frame.
  HoldMusic's wait is the fade, the 50 ms and that lead, so a wait of about 550 ms for a 500 ms
  fade means the lead had run out: 532-551 ms before the top-ups, 608-622 ms after them, over two
  saves and a load (58-72 ms left; PCSX2, debug, e1m1 with track 6, 2026-10-20). One stall the
  top-ups can't split remains: the first card access of a session starts the driver inside the
  probe, about 190 ms. Through the menus that happens when the Save or Load menu lists the slots,
  so only a console `save` as the session's first card access meets it.
- **The fade's gain** is Q14, applied as `((v >> 8) * g) >> 6`: a ring sample is 16 bits times
  up to 256 (`kUnityGain`), so the product stays under 2^29. A host harness (UBSan with
  `-fsanitize=integer`) checked 180 cases, full-scale samples of both signs and fades laid in one
  pass or two, across the ring's wrap: within the gain's 1/16384 (2 LSB at full scale),
  monotonic, ending 1/8192 from silence.
- **WAV fallback (the user's request, for whoever skips `make music`):** per game directory,
  `trackNN.adp`, then `trackNN.wav`, then `Track%02d.wav` (as `make music` accepts; FAT and
  macOS hosts ignore case anyway); every name in one directory before the next, so a mod's
  track wins whatever its format, as in `bgmusic.c`. Loose files only, never from a pak. The
  format comes from the header (`"Q2MU"` or RIFF/WAVE), not the extension. Only a *missing*
  file falls through: one that is malformed or can't get its buffers (`OpenResult::Unusable`)
  is reported once and leaves the track silent. WAV is 16-bit PCM, mono or stereo, 4-48 kHz,
  walked chunk by chunk (LIST etc. skipped, `WAVE_FORMAT_EXTENSIBLE` read, data length clamped
  to the file size). Buffers grow with the byte rate: ~3/8 s each in 4 KB pages, 8-64 KB, so a
  44.1 kHz stereo rip holds 2 × 64 KB (vs 2 × 32 KB for ADPCM).
- **Rate conversion** (in `cd_audio.cpp`, for any format): equal rates copy straight in;
  exactly double goes through `half_band.h`; anything else through the linear resampler. The
  half-band is 23 taps, Kaiser beta 6.8, Q14 `{5103, -1425, 591, -232, 72, -13}` around a 0.5
  centre: -0.14 dB at 8 kHz, -6 dB at the new Nyquist (inherent), ≥67 dB down for anything
  that folds below 7 kHz. Shorter filters measured: 11 taps -40 dB, 15 taps -50 dB above
  15 kHz. **Codegen:** both channels in one loop spilled (46 live samples, 113 instructions
  per output frame); one channel at a time with the taps spelled out (`-O2` leaves a loop over
  them rolled, reloading each tap) is ~49 per channel. A host harness (ASan + UBSan, `-O1`,
  `-O3` and `-fsanitize=implicit-conversion,integer`) fuzzes the API with the uncommitted input
  poisoned, checks every output bit-exact against an int64 convolution, and drives the
  accumulator to its peak: ±755,749,620, 35% of int32.
- **Cost (PCSX2, debug, demo1, 2026-10-08):** the Music event (`BGM_Update`) 164 µs a frame
  mean with an `.adp` track; mixing the music in raised `SndMix` by ~80 µs. A track's open is
  blocking: 5.7 ms when `bgm_extmusic 1` restarted one mid-game. *(Q2)* A 44.1 kHz WAV about
  274 µs mean, the decimator being most of it. The ADPCM speed came from keeping the history
  in locals (to dodge `-fno-strict-aliasing` spills) and unrolling per byte. Static footprint
  ~12 KB with the reader thread; its 8 KB static stack went with the move to the loader.
- **Verified (PCSX2, 2026-10-08), with the Quake II port's encoded tracks linked into
  `id1/music/` and two generated 1.5 s WAVs:** demo1's track 2 streamed as ADPCM with the raw
  ring held a full 7680 frames ahead and filled at 22050 frames a second; a 44.1 kHz WAV
  through the decimator, 44100 source frames a second in; an 11025 Hz stereo WAV through the
  linear resampler and a 22050 Hz mono one, looping every 1.5 s; all three `music_loop` cases
  above; `bgm_extmusic 0`/`1` stopping and restarting the map's track. *(Q2, by hash:)* ADPCM
  wraps bit-exact; an extensible mono 22 kHz WAV with an oversized data length; a forced
  allocation failure (warning, silence, no leak); `.adp` winning over a `.wav` beside it.
- Sound bring-up failure is not fatal (`-nosound` turns it off on purpose). The libsd/audsrv
  IRX load is one-shot.

**After music changes:** check the Music event in a frame log over demo1 with a track playing
(above), and run a map cycle with music on. Scripted music tests hit the traps in
`testing-pcsx2.md`.
