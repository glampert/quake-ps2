/* ================================================================================================
 * File: snd.cpp
 * Brief: QuakeSpasm's SNDDMA_* sound output seam (q_sound.h), on top of an AudsrvDevice (the
 *        audsrv IOP driver, see audsrv_device.h) and a MixRing (the paint buffer and its submit
 *        cursor, see mix_ring.h).
 *
 *        The mixing itself is QuakeSpasm's, portable and unchanged (snd_dma.c, snd_mix.c,
 *        snd_mem.c): every frame S_Update_ paints _snd_mixahead seconds ahead into shm->buffer.
 *        What this file adds is the feeder, which plays the part a sound card's DMA plays on a
 *        PC: a thread above the main thread's priority, woken every 5ms by a timer alarm,
 *        that keeps audsrv's queue 43ms deep from what the mixer has painted.
 *
 *        It is a thread rather than a call at the end of each frame because of the stalls: a
 *        level load, a save, a modal message. audsrv never notices its queue running dry and
 *        replays its last 106ms over and over (see audsrv_device.cpp), so a frame that took
 *        a second would buzz for a second. The feeder carries on through a stall: it sends
 *        what is left of the painted audio - QuakeSpasm clears it to silence before the
 *        stalls it knows about - and then pads the queue with silence. The padding never
 *        moves the cursor the engine reads back, so once the main thread returns the mix
 *        carries on from where it stopped. Doing the IOP round trips on this thread also
 *        keeps them off the main thread's frame.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/audio/audsrv_device.h"
#include "ps2/audio/mix_ring.h"
#include "ps2/common.h"
#include "ps2/renderer/profile.h"

#include <kernel.h>
#include <timer.h>
#include <timer_alarm.h>

#include <atomic>
#include <cstring>

namespace {

using ps2::audio::AudsrvDevice;
using ps2::audio::MixRing;

static AudsrvDevice s_device;
static MixRing s_mixRing;

// The output rate is snd_mixspeed's, as on QuakeSpasm's SDL backend, but only the rates audsrv
// takes at 16-bit stereo: its upsamplers to the SPU2's 48kHz are a fixed table, and a rate
// outside it fails audsrv_set_format. 22050 is snd_mixspeed's PS2 default (see snd_dma.c).
constexpr int kOutputRatesHz[] = { 11025, 12000, 22050, 24000, 32000, 44100, 48000 };
constexpr int kDefaultRateHz   = 22050;

// How often the feeder wakes. audsrv's IOP thread takes a feed (10.7ms of audio) at a time,
// so every wake finds at most one feed gone.
constexpr u32 kFeedPeriodUsec = 5000;

// How deep the feeder keeps audsrv's queue, in feeds: 43ms. Every millisecond queued is a
// millisecond of latency on top of _snd_mixahead, and the full 106ms the queue could hold
// only bought cover for a late main thread, which the feeder no longer depends on.
constexpr int kQueueTargetFeeds = 4;

// With the painted audio used up and the queue below this many feeds (21ms), the main thread
// is stalled, and the feeder pads the queue with silence.
constexpr int kQueueLowFeeds = 2;

alignas(64) static const u8 s_silence[2048] = {};
constexpr int kSilenceFrames = static_cast<int>(sizeof(s_silence)) / AudsrvDevice::kFrameBytes;

// The feeder thread. Only it calls into audsrv once SNDDMA_Init has started it.
constexpr int kFeederStackBytes = 4 * 1024;
alignas(16) static u8 s_feederStack[kFeederStackBytes];

static int s_feederThread = -1;
static int s_tickSema     = -1; // signalled by the alarm, every kFeedPeriodUsec
static int s_exitSema     = -1; // signalled by the feeder as it leaves
static int s_tickAlarm    = -1;
static u64 s_tickBusClocks = 0;

static std::atomic<bool> s_feederQuit{ false };

#if PS2_QUAKE_PROFILE
// Cycles the feeder has spent awake, wall time, round trips to the IOP included. Only the
// feeder writes it; SNDDMA_Submit folds what is new since its last look into the Sound event.
static std::atomic<u32> s_feederCycles{ 0 };
static u32 s_feederCyclesSeen = 0;
#endif // PS2_QUAKE_PROFILE

int PickSampleRate()
{
    const int requested = static_cast<int>(snd_mixspeed.value);
    for (const int rate : kOutputRatesHz)
    {
        if (rate == requested)
        {
            return rate;
        }
    }

    Con_Printf("snd_mixspeed %d is not a rate audsrv takes, using %d.\n", requested, kDefaultRateHz);
    Cvar_SetValueQuick(&snd_mixspeed, static_cast<float>(kDefaultRateHz));
    return kDefaultRateHz;
}

// One pass of the feeder: tops audsrv's queue up to the target from what is painted, and pads
// it with silence when there is too little of that to keep it from running dry.
void Feed()
{
    const int feed   = s_device.FeedFrames();
    const int queued = s_device.CapacityFrames() - (s_device.FreeBytes() / AudsrvDevice::kFrameBytes);

    const int room = (kQueueTargetFeeds * feed) - queued;
    if (room <= 0)
    {
        return;
    }

    // `paintedtime` is the mixer's, in snd_dma.c: the main thread writes it, after the samples.
    const int painted = __atomic_load_n(&paintedtime, __ATOMIC_RELAXED);
    int depth = queued + s_mixRing.Drain(s_device, painted, room);

    const int lowWater = kQueueLowFeeds * feed;
    while (depth < lowWater)
    {
        const int frames = ((lowWater - depth) < kSilenceFrames) ? (lowWater - depth) : kSilenceFrames;
        const int sent   = s_device.Enqueue(s_silence, frames * AudsrvDevice::kFrameBytes);
        if (sent <= 0)
        {
            break;
        }
        depth += sent / AudsrvDevice::kFrameBytes;
    }
}

// Runs inside the timer interrupt (ps2sdk's T2 handler, timer.c), which does the ExitHandler()
// itself once its alarms have run: one here would re-enable interrupts while it is still
// working through its alarm list.
u64 FeederTick(s32, u64, u64, void *, void *)
{
    iSignalSema(s_tickSema);
    return s_tickBusClocks; // and again one period on
}

void FeederMain(void *)
{
    for (;;)
    {
        WaitSema(s_tickSema);
        if (s_feederQuit.load(std::memory_order_relaxed))
        {
            break;
        }

#if PS2_QUAKE_PROFILE
        const ps2::debug::CpuCycles start = ps2::debug::ReadCycles();
        Feed();
        const u32 total = s_feederCycles.load(std::memory_order_relaxed);
        s_feederCycles.store(total + (ps2::debug::ReadCycles() - start), std::memory_order_relaxed);
#else  // PS2_QUAKE_PROFILE
        Feed();
#endif // PS2_QUAKE_PROFILE
    }

    SignalSema(s_exitSema);
    ExitThread();
}

// The feeder runs one priority level above the main thread, so the alarm's wake preempts
// whatever the main thread is doing, a level load included.
bool StartFeeder()
{
    ee_thread_status_t mainStatus = {};
    if (ReferThreadStatus(GetThreadId(), &mainStatus) < 0)
    {
        Con_Printf("WARNING: sound: can't read the main thread's priority.\n");
        return false;
    }

    ee_sema_t sema  = {};
    sema.init_count = 0;
    sema.max_count  = 1;
    s_tickSema      = CreateSema(&sema);
    s_exitSema      = CreateSema(&sema);
    if (s_tickSema < 0 || s_exitSema < 0)
    {
        Con_Printf("WARNING: sound: can't create the feeder's semaphores.\n");
        return false;
    }

    ee_thread_t thread      = {};
    thread.func             = reinterpret_cast<void *>(&FeederMain);
    thread.stack            = s_feederStack;
    thread.stack_size       = kFeederStackBytes;
    thread.gp_reg           = &_gp;
    thread.initial_priority = (mainStatus.current_priority > 0) ? (mainStatus.current_priority - 1) : 0;

    s_feederThread = CreateThread(&thread);
    if (s_feederThread < 0 || StartThread(s_feederThread, nullptr) < 0)
    {
        Con_Printf("WARNING: sound: can't start the feeder thread.\n");
        return false;
    }

    s_tickBusClocks = TimerUSec2BusClock(0, kFeedPeriodUsec);
    s_tickAlarm     = SetTimerAlarm(s_tickBusClocks, &FeederTick, nullptr);
    if (s_tickAlarm < 0)
    {
        Con_Printf("WARNING: sound: can't set the feeder's timer alarm.\n");
        return false;
    }
    return true;
}

// Stops the alarm, then the thread, waiting for it to let go of audsrv. The semaphores stay:
// this runs once, on the way out of the game.
void StopFeeder()
{
    if (s_tickAlarm >= 0)
    {
        ReleaseTimerAlarm(s_tickAlarm);
        s_tickAlarm = -1;
    }

    if (s_feederThread >= 0)
    {
        s_feederQuit.store(true, std::memory_order_relaxed);
        SignalSema(s_tickSema);
        WaitSema(s_exitSema);
        DeleteThread(s_feederThread);
        s_feederThread = -1;
    }
}

} // namespace

// ------------------------------------------------------------------------------------------------
// SNDDMA_*
// ------------------------------------------------------------------------------------------------

extern "C" {

qboolean SNDDMA_Init(dma_t * dma)
{
    const int rateHz = PickSampleRate();

    if (!s_device.Init(rateHz))
    {
        Con_Printf("The audsrv sound driver didn't come up - running silent.\n");
        return false;
    }

    s_mixRing.Reset();

    if (!StartFeeder())
    {
        StopFeeder();
        s_device.Shutdown();
        Con_Printf("No sound feeder - running silent.\n");
        return false;
    }

    std::memset(dma, 0, sizeof(*dma));
    dma->channels         = AudsrvDevice::kChannels;
    dma->samples          = MixRing::kSamples;
    dma->submission_chunk = 1; // audsrv takes any number of whole stereo frames
    dma->samplepos        = 0;
    dma->samplebits       = AudsrvDevice::kSampleBits;
    dma->speed            = rateHz;
    dma->buffer           = s_mixRing.Buffer();

    // The backend publishes the device, as snd_sdl.c did; S_Shutdown clears it.
    shm = dma;
    return true;
}

void SNDDMA_Shutdown()
{
    StopFeeder();
    s_device.Shutdown();
}

int SNDDMA_GetDMAPos()
{
    // For soundinfo; GetSoundtime works from the return value.
    shm->samplepos = s_mixRing.PositionInSamples();
    return shm->samplepos;
}

void SNDDMA_LockBuffer()
{
    // Nothing to lock or map: the mixer paints straight into the MixRing's buffer, which
    // stays put for the whole run.
}

void SNDDMA_Submit()
{
    // The feeder sends what was painted on its next wake. All this does is put the time it
    // spent feeding since the last frame into the profile: wall time, which includes the
    // waits on the IOP that the main thread ran through.
#if PS2_QUAKE_PROFILE
    const u32 total = s_feederCycles.load(std::memory_order_relaxed);
    ps2::debug::ProfileAccumulate(&ps2::prof_evt::Sound, total - s_feederCyclesSeen);
    s_feederCyclesSeen = total;
#endif // PS2_QUAKE_PROFILE
}

// QuakeSpasm blocks sound while its window is out of focus (S_BlockSound). A console has no
// focus to lose, and nothing else calls these.
void SNDDMA_BlockSound() {}
void SNDDMA_UnblockSound() {}

} // extern "C"
