/* ================================================================================================
 * File: sound_load.h
 * Brief: Mid-level sound effects loaded through the background loader (fs/async_io.h), so a
 *        sound first asked for while a level runs (a menu's click, the talk beep, a sound the
 *        cache evicted and a channel wants again) no longer costs the frame a file read: on the
 *        Quake II port's console that was 58 ms from the HDD and 147 ms from a USB stick, in the
 *        frame that asked. S_LoadSound hands the file to this when the loading plaque is down; the
 *        read lands in a staging buffer, and the pump (fs/aio_pump.h) finishes it on the main
 *        thread - GetWavinfo, Cache_Alloc, ResampleSfx, the half of S_LoadSound that became
 *        S_FinishLoadSound - between frames, so the next frame's S_Update can mix it. A
 *        S_StartSound for a sound still loading is kept here and started when the sound lands,
 *        if that is within half a second, and dropped otherwise. Level loads stay synchronous
 *        under the plaque, where a stall is expected.
 *        NOTE: Shared header between C and C++.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */
#ifndef PS2_AUDIO_SOUND_LOAD_H
#define PS2_AUDIO_SOUND_LOAD_H

#include <tamtypes.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct sfx_s;

/* Queues the read of 'path' (the file S_LoadSound would load) for 'sfx'. 1 when queued: the
   sfx's cache arrives later, through the pump, and S_LoadSound returns NULL meanwhile. 0 when it
   can't be: the loader is off, the loading plaque is up, the file is missing or not locatable,
   memory or a table slot is short - the caller then loads synchronously as before, which also
   reports a missing file. */
int PS2_SoundLoadAsync(struct sfx_s * sfx, const char * path);

/* True while a queued read for 'sfx' has not been finished yet. */
int PS2_SoundLoadPending(const struct sfx_s * sfx);

/* S_StartSound's arguments for a sound still loading, kept until it lands: started then, if
   that is within half a second, and dropped otherwise (eight at most; more are dropped). */
void PS2_SoundStartDeferred(struct sfx_s * sfx, int entnum, int entchannel, const float * origin, float fvol,
                            float attenuation);

/* Finishes every queued read, waiting for the loader, and drops the deferred starts. Called
   where sfx caches may be freed or sound shut down: S_Shutdown, and every level change
   (PS2_AsyncIoDrain). */
void PS2_SoundLoadsDrain(void);

/* snd_mem.c: the part of S_LoadSound after the file read, on a WAV image in memory. 1 when the
   sfx now has a cache. The data stays the caller's. */
int PS2_SoundFinishLoad(struct sfx_s * sfx, unsigned char * data, int size);

#ifdef __cplusplus
}

namespace ps2::audio {

/* Finishes the reads that landed, oldest first, until 'budgetCycles' of ReadCycles() are spent
   (at least one when any has landed), and starts the sounds that waited for them. Returns how many
   were finished. Main thread, once a frame, from aio::Pump. */
int PumpLoads(u32 budgetCycles);

/* PS2_SoundLoadsDrain, for the C++ side (aio_pump.cpp). */
void DrainLoads();

} // namespace ps2::audio
#endif /* __cplusplus */

#endif /* PS2_AUDIO_SOUND_LOAD_H */
