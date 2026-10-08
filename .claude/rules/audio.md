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
  stores (`std::atomic` with relaxed order compiles to plain `lw`/`sw`; check `nm` for
  `__atomic`/`__sync` calls after changing them). No read-modify-write across threads.
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
- **Mechanics:** a reader thread at the main thread's priority (so it never preempts; the
  sound feeder sits above both), two read buffers from `TryAllocAligned(64)` under
  `MemTag::Music`, allocated when a track opens and freed when it closes. `map` and
  `disconnect` close it (`CL_Disconnect`); a `changelevel` doesn't (`Host_Reconnect_f`), so the
  old track and its 16 KB stay open through the load until the new map's track replaces it. The main
  thread yields with `RotateThreadReadyQueue` after queueing: its vsync wait
  (`graph_wait_vsync`) spins, so that and file I/O are the reader's only chances. `BGM_Update`
  (in the main loop just before `S_Update`) tops `s_rawsamples` up to `paintedtime + 7680`,
  the raw-sample channel `bgmusic.c` used, so it costs no SIF bandwidth of its own.
- **QuakeSpasm's music semantics, kept:** `BGM_PlayCDtrack` stops the music and tries
  `CDAudio_Play` first (it always declines here); the music holds still while `bgmvolume` is
  0 or the game is paused (`svc_setpause`), with the queued ~350 ms playing out; `BGM_Stop`
  drops what is queued. `bgmloop` (`music_loop`) decides looping: a track opened looping
  never finishes, `music_loop 0` mid-track ends it after the pass (`StopLooping`), and
  `music_loop 1` while a once-only track plays reopens it when it ends. Not kept:
  `music_jump` (tracker orders) and the `cd` command (no disc). Added: `bgm_extmusic` acts at
  once. "Couldn't find a cdrip for track N" prints once per track, not on every map.
- **WAV fallback (the user's request, for whoever skips `make music`):** per game directory,
  `trackNN.adp`, then `trackNN.wav`, then `Track%02d.wav` (as `make music` accepts; FAT and
  macOS hosts ignore case anyway); every name in one directory before the next, so a mod's
  track wins whatever its format, as in `bgmusic.c`. Loose files only, never from a pak. The
  format comes from the header (`"Q2MU"` or RIFF/WAVE), not the extension. Only a *missing*
  file falls through: one that is malformed or can't get its buffers (`OpenResult::Unusable`)
  is reported once and leaves the track silent. WAV is 16-bit PCM, mono or stereo, 4-48 kHz,
  walked chunk by chunk (LIST etc. skipped, `WAVE_FORMAT_EXTENSIBLE` read, data length clamped
  to the file size). Buffers grow with the byte rate: ~3/8 s each in 4 KB pages, 8-64 KB, so a
  44.1 kHz stereo rip holds 2 × 64 KB (vs 2 × 8 KB for ADPCM).
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
  ~12 KB (the 8 KB reader stack is a static member, so it stays in .bss).
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
