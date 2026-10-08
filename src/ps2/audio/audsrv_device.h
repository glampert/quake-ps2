#pragma once
/* ================================================================================================
 * File: audsrv_device.h
 * Brief: PCM output device over the audsrv IOP driver. The AudsrvDevice class owns the
 *        IOP-side bring-up (the libsd + audsrv IRX modules) and the streaming session,
 *        exposing audsrv's queue as "how many bytes fit right now" plus "take these".
 *        It knows nothing about Quake: the sound seam (snd.cpp) drives a single static
 *        instance and MixRing (mix_ring.h) does the bookkeeping around it.
 *
 *        The SPU2 is only reachable from the IOP - there is no EE-side mapping of its
 *        registers - so every byte the mixer produces crosses SIF through audsrv. All
 *        of its entry points are blocking RPCs, and audsrv's EE library runs every one of
 *        them through a single static buffer, so only one thread may call into it: once
 *        Init has returned, that is snd.cpp's feeder thread.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <tamtypes.h>

namespace ps2::audio {

class AudsrvDevice final
{
public:
    // 16-bit stereo is the only sensible output format: audsrv's upsampler table has
    // no 22050/8/stereo entry, and it is what the mixer's fast path (S_TransferStereo16)
    // produces anyway. Everything downstream is upsampled to the SPU2's native 48kHz.
    static constexpr int kChannels   = 2;
    static constexpr int kSampleBits = 16;
    static constexpr int kFrameBytes = kChannels * (kSampleBits / 8); // one stereo pair

    // Starts libsd and audsrv on the IOP (once per run) and opens a stream at
    // sampleRateHz. Returns false when audio can't be brought up - a missing IOP
    // driver only costs us sound, so the caller is expected to carry on silently.
    bool Init(int sampleRateHz);
    void Shutdown();

    bool Available() const { return m_ready; }

    // What audsrv's IOP thread takes from the queue each time it wakes, 93.75 times a
    // second: 512 samples at 48kHz, in frames at the stream's rate. 235 at 22050Hz.
    int FeedFrames() const { return m_feedFrames; }

    // The most the queue can hold, in frames: ten feeds (106ms at any rate), less the
    // guard frame FreeBytes() holds back.
    int CapacityFrames() const { return (m_feedFrames * 10) - 1; }

    // Free space in audsrv's IOP ring buffer, rounded down to whole stereo frames and
    // with one frame held back (see the guard note in the .cpp). Zero when the queue
    // is full.
    int FreeBytes() const;

    // Appends to the IOP ring. sizeBytes must be a multiple of kFrameBytes and no
    // larger than the FreeBytes() reading it was derived from. Returns the number of
    // bytes actually accepted, which can be smaller; anything beyond that is dropped
    // by audsrv, so the caller must resume from there rather than assume it all went.
    int Enqueue(const u8 * data, int sizeBytes);

private:
    // libsd.irx -> audsrv.irx -> audsrv_init(). One-shot: re-running any of it would
    // fail the module load and leak the EE-side RPC thread audsrv_init() spawns.
    static bool StartIopSide();

    bool m_ready      = false;
    int  m_feedFrames = 0;
};

} // namespace ps2::audio
