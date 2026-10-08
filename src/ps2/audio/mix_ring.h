#pragma once
/* ================================================================================================
 * File: mix_ring.h
 * Brief: The buffer QuakeSpasm's mixer paints into (shm->buffer), and the bookkeeping that
 *        turns it into something audsrv can be fed.
 *
 *        The engine wants a ring it can write anywhere in and a play cursor it can read
 *        back; audsrv wants a queue it can be handed chunks of. MixRing bridges the two
 *        the way id's own waveOut backend (WinQuake's snd_win.c) did: the position
 *        reported to the engine is how far we have *submitted*, not where the SPU2
 *        actually is. Because submission is paced by the device's queue, that cursor
 *        advances at the playback rate, and - unlike a real play position - it is
 *        monotonic by construction. GetSoundtime() requires exactly that (it infers buffer
 *        wraps from the value decreasing). The cost is latency: _snd_mixahead plus the
 *        queue depth, as on the waveOut path.
 *
 *        Two threads share it (see snd.cpp): the main thread paints ahead of the cursor
 *        and reads it back, and the sound feeder thread alone submits and moves it. They
 *        never touch the same frames: the feeder sends only what lies between the cursor
 *        and paintedtime, and the mixer paints only from paintedtime on, never more than
 *        one ring past the cursor.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/audio/audsrv_device.h"

#include <atomic>

namespace ps2::audio {

class MixRing final
{
public:
    // 16384 stereo frames: 743ms at 22050Hz, comfortably more than the 0.1s _snd_mixahead
    // default asks for. Must stay a power of two - the mixer masks with dma.samples - 1
    // to find its write offset.
    static constexpr int kFrames    = 16384;
    static constexpr int kSamples   = kFrames * AudsrvDevice::kChannels;   // -> dma.samples
    static constexpr int kSizeBytes = kFrames * AudsrvDevice::kFrameBytes; // 64KB

    // Clears the buffer and rewinds the submit cursor. Call before handing Buffer()
    // to the engine, and before the feeder starts.
    void Reset();

    // The mixer writes here directly; this is shm->buffer and stays valid for the
    // lifetime of the program.
    u8 * Buffer() { return m_buffer; }

    // SNDDMA_GetDMAPos: the submit cursor wrapped into the ring, counted in mono
    // samples (a stereo pair counts as two), which is the unit dma.samples is in.
    // Main thread.
    int PositionInSamples() const;

    // Hands the device what is painted but not yet submitted, up to maxFrames, splitting
    // the copy where the ring wraps. paintedFrames is the engine's `paintedtime`. Returns
    // the frames sent; whatever doesn't go stays put for the next call. Follows
    // paintedtime when the engine winds it back (see the .cpp). Feeder thread.
    int Drain(AudsrvDevice & device, int paintedFrames, int maxFrames);

private:
    // Total frames handed to the device since Reset(). Free-running; the mask in
    // PositionInSamples() is what folds it back into the ring. The feeder writes it and
    // the main thread reads it; a 32-bit load or store is a single instruction, so
    // relaxed order is all either needs.
    std::atomic<int> m_submittedFrames{ 0 };

    alignas(64) u8 m_buffer[kSizeBytes] = {};
};

} // namespace ps2::audio
