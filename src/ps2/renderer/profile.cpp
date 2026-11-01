/* ================================================================================================
 * File: profile.cpp
 * Brief: Profile events shared by more than one source file, and the CSV frame log.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/profile.h"
#include "ps2/debug/engine_profile.h"
#include "ps2/renderer/render_system.h"
#include "ps2/renderer/cmd_buffer.h"
#include "ps2/renderer/vu1.h"
#include "ps2/renderer/vram.h"
#include "ps2/renderer/gs.h" // GetPresentClock
#include "ps2/fs/async_io.h"  // TakeFrameStats
#include "ps2/audio/snd.h"    // TakeFeederRpcCycles

#include <cstdarg>
#include <cstdio>

#if PS2_QUAKE_FRAME_LOG_FILE
#include "ps2/system/iop_boot.h" // SyncGameDataDevice
#include <fcntl.h>
#include <unistd.h>
#endif // PS2_QUAKE_FRAME_LOG_FILE

namespace ps2::prof_evt {

PS2_PROFILE_DEFINE_EVENT(Frame,       "Frame",       kScreenOverlay, 0);
PS2_PROFILE_DEFINE_EVENT(VSync,       "VSync",       kScreenOverlay, 1);
PS2_PROFILE_DEFINE_EVENT(GsWait,      "GsWait",      kScreenOverlay, 2);
PS2_PROFILE_DEFINE_EVENT(DmaSend,     "DmaSend",     kScreenOverlay, 3);
PS2_PROFILE_DEFINE_EVENT(DmaFlush,    " CacheFlsh",  kScreenOverlay, 4);
PS2_PROFILE_DEFINE_EVENT(View,        "View",        kScreenOverlay, 5);
PS2_PROFILE_DEFINE_EVENT(World,       "World",       kScreenOverlay, 6);
PS2_PROFILE_DEFINE_EVENT(Vis,         "Vis",         kScreenOverlay, 7);
PS2_PROFILE_DEFINE_EVENT(TexChains,   "TexChains",   kScreenOverlay, 8);
PS2_PROFILE_DEFINE_EVENT(LmChains,    "LmChains",    kScreenOverlay, 9);
PS2_PROFILE_DEFINE_EVENT(Entities,    "Entities",    kScreenOverlay, 10);
PS2_PROFILE_DEFINE_EVENT(EntCull,     " Cull",       kScreenOverlay, 11);
PS2_PROFILE_DEFINE_EVENT(EntShade,    " Shade",      kScreenOverlay, 12);
PS2_PROFILE_DEFINE_EVENT(EntGeom,     " Geom",       kScreenOverlay, 13);
PS2_PROFILE_DEFINE_EVENT(EntShadow,   " Shadow",     kScreenOverlay, 14);
PS2_PROFILE_DEFINE_EVENT(EntBrush,    " Brush",      kScreenOverlay, 15);
PS2_PROFILE_DEFINE_EVENT(Particles,   "Particles",   kScreenOverlay, 16);
PS2_PROFILE_DEFINE_EVENT(TurbSurfs,   "TurbSurfs",   kScreenOverlay, 17);
PS2_PROFILE_DEFINE_EVENT(Sky,         "Sky",         kScreenOverlay, 18);
PS2_PROFILE_DEFINE_EVENT(Ui,          "Ui",          kScreenOverlay, 19);
PS2_PROFILE_DEFINE_EVENT(Overlay,     "Overlay",     kScreenOverlay, 20);
PS2_PROFILE_DEFINE_EVENT(Sound,       "Sound",       kScreenOverlay, 21);
PS2_PROFILE_DEFINE_EVENT(Server,      "Server",      kScreenOverlay, 22);
PS2_PROFILE_DEFINE_EVENT(ClParse,     "ClParse",     kScreenOverlay, 23);
PS2_PROFILE_DEFINE_EVENT(ClScene,     "ClScene",     kScreenOverlay, 24);
PS2_PROFILE_DEFINE_EVENT(ClParticles, "ClPart",      kScreenOverlay, 25);
PS2_PROFILE_DEFINE_EVENT(SndMix,      "SndMix",      kScreenOverlay, 26);
PS2_PROFILE_DEFINE_EVENT(FsIo,        "FsIo",        kScreenOverlay, 27);
PS2_PROFILE_DEFINE_EVENT(Music,       "Music",       kScreenOverlay, 28);
PS2_PROFILE_DEFINE_EVENT(Input,       "Input",       kScreenOverlay, 29);

} // namespace ps2::prof_evt

// ------------------------------------------------------------------------------------------------
// Engine probes (debug/engine_profile.h)
// ------------------------------------------------------------------------------------------------

#if PS2_QUAKE_PROFILE
namespace {

static ps2::debug::ProfileEvent * const s_engineEvents[PS2_PROF_SITE_COUNT] = {
    &ps2::prof_evt::Server,      // PS2_PROF_SERVER
    &ps2::prof_evt::ClParse,     // PS2_PROF_CL_PARSE
    &ps2::prof_evt::ClScene,     // PS2_PROF_CL_SCENE
    &ps2::prof_evt::ClParticles, // PS2_PROF_CL_PARTICLES
    &ps2::prof_evt::SndMix,      // PS2_PROF_SND_MIX
};

static ps2::debug::CpuCycles s_engineStart[PS2_PROF_SITE_COUNT];

} // namespace

extern "C" void PS2_ProfileBegin(const int site)
{
    s_engineStart[site] = ps2::debug::ReadCycles();
}

extern "C" void PS2_ProfileEnd(const int site)
{
    ps2::debug::ProfileAccumulate(s_engineEvents[site],
                                  ps2::debug::ReadCycles() - s_engineStart[site]);
}
#endif // PS2_QUAKE_PROFILE

// ------------------------------------------------------------------------------------------------
// Frame log
// ------------------------------------------------------------------------------------------------

#if PS2_QUAKE_PROFILE
namespace ps2::debug {
namespace {

// Frames per dump. At 30-60fps this is a batch every one to two seconds, which
// keeps the hitch the dump causes rare while still bounding the buffer. Sized
// against the cost of the dump itself: one printf per row, each a round trip to
// the IOP, so a batch is tens of milliseconds no matter how it is arranged.
constexpr int kBatchFrames = 64;

// Columns taken from the profile registry, in header order.
constexpr int kNumEvents = 30;

// One frame's sample. Timings are held as raw cycles and converted at dump time,
// so capture stays a load and a store per field.
struct FrameSample
{
    u32 frameIndex;
    u32 cycles[kNumEvents];

    // rs::DrawStats
    int tris, batches, particles;

    // vram::Stats. Uploads are bursty around map transitions and each one that
    // followed an eviction also forced a GS drain, so these are the first thing
    // to check against a frame-time spike. vramUsedKB is the heap's committed VRAM
    // as the frame ended: its peak over a level is what the level asks of the
    // texture heap, which a larger framebuffer would take from. vramEvicts says
    // whether that peak is the level's whole demand (none) or the heap's cap (some).
    int vramUploads, vramOomSyncs, vramResident, vramUsedKB, vramEvicts;

    // chain counters. The chain is built front to back across a whole frame and
    // only moves to the other half when it runs out, so chainKB against
    // cmdbuf::kHalfBytes is what says whether the capacity is right - and
    // chainDrains is what says it was not: every one of those is a mid-frame kick
    // the frame did not ask for (it was a full pipeline stall until the overflow
    // stopped waiting for the GS). chainKicks is the number this refactor exists
    // to bring down.
    int chainKB, chainKicks, chainDrains;

    // The background loader (fs/async_io.h): bytes it landed during the frame, and how many
    // requests were queued or running at the capture.
    int aioBytes, aioQ;

    // The sound feeder's time inside audsrv's two RPCs during the frame (audio/snd.h): the
    // free-space query and the plays. Either waits on the IOP while a file read is out there, so
    // these say how long the feeder was held, and the ring with it, by the loader's reads.
    u32 sndQueryCycles, sndPlayCycles;
};

static FrameSample s_samples[kBatchFrames];
static int  s_count      = 0;
static u32  s_frameIndex = 0;
static bool s_skipNext   = false; // the frame a dump landed in is not representative
static bool s_headerDone = false;

static cvar_t s_frameLog = ps2::MakeCvar("ps2_frame_log", "0", CVAR_NONE); // <-- ENABLE FRAME LOG HERE

// Files opened since the last dump, written out with it. A file opened while a level runs is a
// synchronous read inside whatever frame asked for it, and without a name the log can only show
// the spike. A demo's map load opens about 40 (the perf run, 2026-10-09); past the buffer only
// the count is kept.
constexpr int kMaxOpenNotes = 64;

struct OpenNote
{
    u32          frameIndex;
    const char * kind; // "open" or "ready"
    char         name[MAX_QPATH];
};

static OpenNote s_openNotes[kMaxOpenNotes];
static int  s_openCount = 0; // including the ones past the buffer
static u32  s_openOverflowRow = 0; // the row of the first note that didn't fit
static bool s_noteOpens = false; // FrameLogNoteOpens: on for the perf run only

// Cycles to microseconds. Cold - only runs at dump time, so the 64-bit divide
// (a libgcc call on the R5900) is fine; it is what the capture path exists to avoid.
u32 ToMicrosec(u32 cycles)
{
    const u32 perMillisec = ps2::debug::ProfileCyclesPerMillisec();
    if (perMillisec == 0)
    {
        return 0;
    }
    return static_cast<u32>((static_cast<u64>(cycles) * 1000u) / perMillisec);
}

// Everything the frame log writes goes through Emit, and each dump ends with Commit. Emit prints
// straight to stdout - the PCSX2 emulog's path - one call per row, as ever. With
// PS2_QUAKE_FRAME_LOG_FILE (profile.h) a dump is built here instead and goes out at Commit: to
// stdout as one write, and appended to <gamedir>/frame_log.txt with one open, write and close.
// That is how a capture gets off a console, which has no stdout anyone reads; ps2_logfile can't
// carry it, since every console print it copies is a USB write inside the frames being measured.
void Emit(const char * format, ...) Q_PRINTF_FUNC(1, 2);

#if PS2_QUAKE_FRAME_LOG_FILE

// A dump: a 64-frame batch at a few hundred characters a row, its open notes and the header.
constexpr int kOutBytes = 64 * 1024;
static char s_out[kOutBytes];
static int  s_outUsed    = 0;
static bool s_outStarted = false; // the first dump of a run truncates the file
static bool s_outFailed  = false; // and one that fails to write turns it off for the run

void Emit(const char * format, ...)
{
    const int room = kOutBytes - s_outUsed;
    if (room <= 1)
    {
        return;
    }

    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(s_out + s_outUsed, static_cast<size_t>(room), format, args);
    va_end(args);

    if (n > 0)
    {
        s_outUsed += (n < room) ? n : (room - 1);
    }
}

void Commit()
{
    if (s_outUsed == 0)
    {
        return;
    }

    std::fputs(s_out, stdout);

    if (!s_outFailed)
    {
        char path[sizeof(com_gamedir) + sizeof("/frame_log.txt")];
        std::snprintf(path, sizeof(path), "%s/frame_log.txt", com_gamedir);

        const int flags = s_outStarted ? O_WRONLY : (O_WRONLY | O_CREAT | O_TRUNC);
        const int fd    = open(path, flags, 0666);
        bool written    = (fd >= 0);
        if (fd >= 0)
        {
            written = (lseek(fd, 0, SEEK_END) >= 0) && (write(fd, s_out, static_cast<size_t>(s_outUsed)) == s_outUsed);
            // A successful close on USB/HDD returns the iomanX slot, not 0 (see ps2-platform.md).
            written = (close(fd) >= 0) && written;
            written = (ps2::sys::SyncGameDataDevice() == 0) && written;
        }
        s_outStarted = true;

        if (!written)
        {
            s_outFailed = true;
            std::printf("FLOG#note,writing %s failed; stdout only from here\n", path);
        }
    }

    s_outUsed = 0;
    s_out[0]  = '\0';
}

#else // !PS2_QUAKE_FRAME_LOG_FILE

void Emit(const char * format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
}

void Commit()
{
}

#endif // PS2_QUAKE_FRAME_LOG_FILE

bool Enabled()
{
    return s_frameLog.value != 0.0f;
}

// Writes every sample the batch holds and empties it. Callers decide whether a
// partial batch is worth writing; see FrameLogFlush and FrameLogFinish.
void WriteBatch()
{
    if (!s_headerDone)
    {
        s_headerDone = true;
        Emit("FLOG#hdr,frame,"
             "Frame,VSync,GsWait,DmaSend,DmaFlush,View,World,Vis,"
             "TexChains,LmChains,Entities,EntCull,EntShade,EntGeom,EntShadow,EntBrush,"
             "Particles,TurbSurfs,Sky,Ui,Overlay,Sound,Server,ClParse,ClScene,ClParticles,SndMix,FsIo,Music,Input,"
             "tris,batches,particles,"
             "vramUploads,vramOomSyncs,vramResident,vramUsedKB,vramEvicts,"
             "chainKB,chainKicks,chainDrains,aioBytes,aioQ,sndQuery,sndPlay\n");
        Emit("FLOG#note,timings are microseconds\n");
        Emit("FLOG#note,aioBytes is what the background loader landed during the frame; aioQ its requests in flight at the capture\n");
        Emit("FLOG#note,sndQuery and sndPlay are the sound feeder's time inside audsrv's free-space and play RPCs during the frame, in microseconds\n");
    }

    // Built into one buffer and written with a single printf: every call is a
    // round trip to the IOP, so a per-column printf would turn the dump from
    // one stretched frame into several. The timings go through a loop rather
    // than a fixed argument list so adding an event needs no changes here.
    for (int i = 0; i < s_count; ++i)
    {
        const FrameSample & s = s_samples[i];

        char line[640];
        int at = std::snprintf(line, sizeof(line), "FLOG,%u", s.frameIndex);

        for (int e = 0; e < kNumEvents && at > 0 && at < static_cast<int>(sizeof(line)); ++e)
        {
            at += std::snprintf(line + at, sizeof(line) - static_cast<size_t>(at),
                                ",%u", ToMicrosec(s.cycles[e]));
        }

        if (at > 0 && at < static_cast<int>(sizeof(line)))
        {
            std::snprintf(line + at, sizeof(line) - static_cast<size_t>(at),
                          ",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u\n",
                          s.tris, s.batches, s.particles,
                          s.vramUploads, s.vramOomSyncs, s.vramResident, s.vramUsedKB, s.vramEvicts,
                          s.chainKB, s.chainKicks, s.chainDrains, s.aioBytes, s.aioQ,
                          ToMicrosec(s.sndQueryCycles), ToMicrosec(s.sndPlayCycles));
        }

        Emit("%s", line);
    }

    for (int i = 0; i < s_openCount && i < kMaxOpenNotes; ++i)
    {
        Emit("FLOG#%s,%u,%s\n", s_openNotes[i].kind, s_openNotes[i].frameIndex, s_openNotes[i].name);
    }
    if (s_openCount > kMaxOpenNotes)
    {
        Emit("FLOG#open,%u,+%d more\n", s_openOverflowRow, s_openCount - kMaxOpenNotes);
    }
    s_openCount = 0;

    s_count    = 0;
    s_skipNext = true; // this frame just absorbed the whole dump
}

} // namespace

void FrameLogInit()
{
    Cvar_RegisterVariable(&s_frameLog);
}

void FrameLogCapture()
{
    if (!Enabled())
    {
        return;
    }

    ++s_frameIndex;

    // The previous dump stretched this frame; logging it would read as a spike
    // in the renderer rather than in the logging.
    if (s_skipNext)
    {
        s_skipNext = false;
        return;
    }

    if (s_count >= kBatchFrames)
    {
        return; // Batch already full and waiting on FrameLogFlush.
    }

    FrameSample & s = s_samples[s_count++];
    s.frameIndex = s_frameIndex;

    static const ps2::debug::ProfileEvent * const s_events[kNumEvents] = {
        &prof_evt::Frame,     &prof_evt::VSync,       &prof_evt::GsWait,    &prof_evt::DmaSend,
        &prof_evt::DmaFlush,  &prof_evt::View,        &prof_evt::World,     &prof_evt::Vis,
        &prof_evt::TexChains, &prof_evt::LmChains,    &prof_evt::Entities,  &prof_evt::EntCull,
        &prof_evt::EntShade,  &prof_evt::EntGeom,     &prof_evt::EntShadow, &prof_evt::EntBrush,
        &prof_evt::Particles, &prof_evt::TurbSurfs,   &prof_evt::Sky,       &prof_evt::Ui,
        &prof_evt::Overlay,   &prof_evt::Sound,       &prof_evt::Server,    &prof_evt::ClParse,
        &prof_evt::ClScene,   &prof_evt::ClParticles, &prof_evt::SndMix,    &prof_evt::FsIo,
        &prof_evt::Music,     &prof_evt::Input,
    };
    static_assert(ArrayLength(s_events) == kNumEvents, "the frame log's columns and its events disagree");
    for (int i = 0; i < kNumEvents; ++i)
    {
        s.cycles[i] = s_events[i]->lastFrameCycles;
    }

    // These still hold the finished frame's values here: rs::BeginFrame, which clears the
    // submission counters, has not run yet for the new frame.
    const rs::DrawStats & r = rs::GetStats();
    s.tris      = r.trisDrawn;
    s.batches   = r.drawBatches;
    s.particles = r.particles;

    const vram::Stats v = vram::GetStats();
    s.vramUploads  = v.uploadsThisFrame;
    s.vramOomSyncs = v.oomSyncsThisFrame;
    s.vramResident = v.residentTextures;
    s.vramUsedKB   = (v.totalWords - v.freeWords) * 4 / 1024;
    s.vramEvicts   = v.evictionsThisFrame;

    // cmdbuf::EndFrame has not run for the new frame either, so these are still the finished
    // frame's. Rounded to KB because the interesting comparison is against a half, which is
    // cmdbuf::kHalfBytes.
    s.chainKB     = static_cast<int>(cmdbuf::BytesLastFrame() / 1024u);
    s.chainKicks  = cmdbuf::KicksLastFrame();
    s.chainDrains = cmdbuf::EmergencyDrainsLastFrame();

    const aio::Stats aioStats = aio::TakeFrameStats();
    s.aioBytes = static_cast<int>(aioStats.bytesLanded);
    s.aioQ     = static_cast<int>(aioStats.queueDepth);

    audio::TakeFeederRpcCycles(s.sndQueryCycles, s.sndPlayCycles);
}

void FrameLogFlush()
{
    if (s_count < kBatchFrames || !Enabled())
    {
        return;
    }
    WriteBatch();
    Commit();
}

void FrameLogFinish()
{
    if (!Enabled())
    {
        return;
    }

    // Whatever is left has nowhere else to go - this is the end of the run.
    if (s_count > 0)
    {
        WriteBatch();
    }

    // The vsync wait's worst wake latency over the run (gs.h): the DISPFB write has to land inside
    // the blanking interval, about 1500 us.
    const ps2::gs::PresentClock & clock = ps2::gs::GetPresentClock();
    Emit("FLOG#note,present: worst stamp-to-DISPFB %u us over %u presents\n",
         static_cast<unsigned>((static_cast<u64>(clock.worstWriteTicks) * 1000000u) / ps2::gs::kPresentTicksPerSec),
         static_cast<unsigned>(clock.frames));

    // Lets the reader tell a completed capture from one the emulator cut short.
    Emit("FLOG#end,%u\n", s_frameIndex);
    Commit();
    std::fflush(stdout);
}

void FrameLogNoteOpens(const bool enable)
{
    s_noteOpens = enable;
}

static void NoteFile(const char * const kind, const char * const fileName)
{
    if (!s_noteOpens || !Enabled())
    {
        return;
    }
    // The row the note is charged to: the frame being measured now, which FrameLogCapture will
    // number one past the last row it wrote.
    const u32 row = s_frameIndex + 1;
    if (s_openCount < kMaxOpenNotes)
    {
        OpenNote & note = s_openNotes[s_openCount];
        note.frameIndex = row;
        note.kind       = kind;
        std::snprintf(note.name, sizeof(note.name), "%s", (fileName != nullptr) ? fileName : "?");
    }
    else if (s_openCount == kMaxOpenNotes)
    {
        s_openOverflowRow = row;
    }
    ++s_openCount;
}

void FrameLogNoteOpen(const char * fileName)
{
    NoteFile("open", fileName);
}

void FrameLogNoteReady(const char * fileName)
{
    NoteFile("ready", fileName);
}

void FrameLogMarkMap(const char * mapName)
{
    if (!Enabled())
    {
        return;
    }
    Emit("FLOG#map,%u,%s\n", s_frameIndex, (mapName != nullptr) ? mapName : "?");
    Commit();
}

void FrameLogMarkView(const char * what)
{
    if (!Enabled())
    {
        return;
    }
    // Left for the next dump to carry: a file build's write here would land in a frame the pass
    // measures, and the scripts order the markers by their row, not by where they are in the log.
    Emit("FLOG#view,%u,%s\n", s_frameIndex, (what != nullptr) ? what : "?");
}

} // namespace ps2::debug
#endif // PS2_QUAKE_PROFILE
