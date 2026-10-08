/* ================================================================================================
 * File: mix_ring.cpp
 * Brief: The Quake mixer's paint buffer and the submit cursor over it. See mix_ring.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/audio/mix_ring.h"
#include <cstring>

namespace ps2::audio {

void MixRing::Reset()
{
    std::memset(m_buffer, 0, sizeof(m_buffer));
    m_submittedFrames.store(0, std::memory_order_relaxed);
}

int MixRing::PositionInSamples() const
{
    // Unsigned so the fold stays well defined whatever the cursor holds.
    const unsigned int frames  = static_cast<unsigned int>(m_submittedFrames.load(std::memory_order_relaxed));
    const unsigned int samples = frames * static_cast<unsigned int>(AudsrvDevice::kChannels);
    return static_cast<int>(samples & (static_cast<unsigned int>(kSamples) - 1u));
}

int MixRing::Drain(AudsrvDevice & device, const int paintedFrames, const int maxFrames)
{
    int submitted = m_submittedFrames.load(std::memory_order_relaxed);

    // GetSoundtime (snd_dma.c) chops paintedtime back to one ring's worth once it passes
    // 0x40000000 frames - 13.5 hours at 22050Hz - and counts the ring's wraps from zero
    // again, so its sound time restarts at the cursor's place in the ring. Follow it there:
    // the position read back stays continuous, and the ring's cleared tail goes out as
    // silence until painting catches up. Left behind, the cursor would sit a billion frames
    // ahead of anything painted, and the game would play no sound again.
    if (paintedFrames < submitted - kFrames)
    {
        submitted &= kFrames - 1;
        m_submittedFrames.store(submitted, std::memory_order_relaxed);
    }

    int pending = paintedFrames - submitted;
    if (pending > maxFrames)
    {
        pending = maxFrames;
    }

    int sentTotal = 0;
    while (pending > 0)
    {
        const int offsetFrames = submitted & (kFrames - 1);

        int chunkFrames = kFrames - offsetFrames; // up to the end of the ring
        if (chunkFrames > pending)
        {
            chunkFrames = pending;
        }

        const int chunkBytes = chunkFrames * AudsrvDevice::kFrameBytes;
        const int sentBytes  = device.Enqueue(m_buffer + (offsetFrames * AudsrvDevice::kFrameBytes), chunkBytes);
        if (sentBytes <= 0)
        {
            break;
        }

        // Published as it goes, so the main thread never sees the cursor run ahead of
        // what audsrv actually took.
        const int sentFrames = sentBytes / AudsrvDevice::kFrameBytes;
        submitted += sentFrames;
        m_submittedFrames.store(submitted, std::memory_order_relaxed);
        pending   -= sentFrames;
        sentTotal += sentFrames;

        // A short write means the IOP ring filled up despite the free space reading - the
        // rest was dropped, not queued. Stop here and pick it up on the next call.
        if (sentBytes != chunkBytes)
        {
            break;
        }
    }
    return sentTotal;
}

} // namespace ps2::audio
