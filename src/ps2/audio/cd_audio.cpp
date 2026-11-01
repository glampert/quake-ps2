/* ================================================================================================
 * File: cd_audio.cpp
 * Brief: The soundtrack: QuakeSpasm's BGM_* background music (bgmusic.h) and its CDAudio_* seam
 *        (cdaudio.h). QuakeSpasm plays a map's track off the disc when it can, and otherwise from
 *        a loose music/trackNN file through its codec libraries (bgmusic.c). The PS2 port reads
 *        no audio CD and has no codec layer: CDAudio_Play always declines, and BGM_* streams
 *        music/trackNN.adp - SPU2 ADPCM, what `make music` encodes - or, when that hasn't been
 *        made, the trackNN.wav itself, through a MusicStream (music_stream.h).
 *
 *        What it decodes goes into the mixer's raw-sample channel, s_rawsamples in snd_dma.c,
 *        where bgmusic.c put its music too. The mixer lays it under the sound effects, so it
 *        reaches the SPU2 inside the audsrv stream that carries the mix anyway. A track at
 *        another sample rate than the mixer's is converted on the way in: a 2:1 half-band
 *        filter for exactly double (a 44.1kHz CD rip), linear interpolation otherwise -
 *        better than the nearest sample S_RawSamples takes.
 *
 *        Behaviour follows bgmusic.c: bgm_extmusic (the options menu's "Background Music",
 *        QuakeSpasm's "External Music") gates it, bgmvolume (its "Music Volume") sets the
 *        level, music_loop decides whether a track starts over when it ends, and "music",
 *        "music_stop", "music_pause" and "music_resume" drive it by hand. One addition:
 *        bgm_extmusic takes effect at once - off stops the music, on starts the map's track
 *        again - rather than at the next map.
 *
 *        Everything here runs on the main thread, in BGM_Update just before S_Update mixes,
 *        so the mixer and this top-up never race over s_rawsamples. Only MusicStream's file
 *        reads happen elsewhere.
 *
 *        The backend can also hold the music through a stall, fading it out first and back in
 *        after (cd_audio.h): the save notice does, around a save or a load.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/audio/cd_audio.h"
#include "ps2/audio/half_band.h"
#include "ps2/audio/music_stream.h"
#include "ps2/common.h"
#include "ps2/renderer/profile.h"
#include "ps2/system/sys.h"

extern "C" {
    #include "quake/bgmusic.h"
    #include "quake/snd_codec.h"
}

#include <cstring>
#include <iterator>

namespace {

using ps2::audio::HalfBandDecimator;
using ps2::audio::MusicStream;

// Highest track number a map may ask for, as on an audio CD.
constexpr int kMaxTrack = 99;

// The files a track's music can come from, tried in this order in each game directory: the .adp
// `make music` encodes, then the plain .wav it encodes from, for whoever hasn't run it. A
// ripper's capitalised Track02.wav is tried too, as `make music` accepts it: FAT and macOS hosts
// ignore case anyway, but PCSX2 on a case-sensitive host does not.
constexpr const char * kTrackFiles[] = {
    "music/track%02d.adp",
    "music/track%02d.wav",
    "music/Track%02d.wav",
};

// The extensions the "music" command tries for a name without one, in order.
constexpr const char * kMusicExtensions[] = { "adp", "wav" };

// How far ahead of the mixer's paint position the raw-sample ring is kept filled. This is the
// hitch tolerance: a frame that takes longer than this to come round again (348 ms at 22050Hz)
// leaves a gap in the music, not a desync. The margin keeps the fill clear of the ring's wrap
// onto samples the mixer hasn't consumed yet.
constexpr int kRawLeadFrames = MAX_RAW_SAMPLES - 512;

// Frames decoded per MusicStream::Decode call, into a stack buffer.
constexpr int kPumpFrames = 256;

// bgmvolume maps to a 0..kUnityGain multiplier. At unity a sample scaled by it lands in the ring
// exactly as S_RawSamples would store it (<< 8), the scale the mixer expects.
constexpr int kUnityGain = 256;

// How long HoldMusic's fade-out takes - longer when more than that is queued already, since a
// fade-out drops none of it - and ReleaseMusic's fade back in.
constexpr int kFadeOutMsec = 500;
constexpr int kFadeInMsec  = 1000;

// A fade's gain, 1.0 = kFadeUnity. A ring sample, 16 bits scaled by up to kUnityGain, is shifted
// down 8 bits before it is multiplied by one, which keeps the product inside 32 bits.
constexpr int kFadeBits  = 14;
constexpr int kFadeUnity = 1 << kFadeBits;

// How far behind the mixer's cursor (soundtime: what the sound feeder has handed to audsrv) the
// speaker is: audsrv's queue, which the feeder keeps 43 ms deep (snd.cpp).
constexpr int kOutputLatencyMsec = 50;

// HoldMusic paints the mix and looks at the cursor this often, and gives up waiting for the fade
// to play out this long after it should have: when the mixer has stopped painting
// (snd_noextraupdate 1 turns S_ExtraUpdate off).
constexpr unsigned long kHoldPollMsec = 5;
constexpr int kHoldSlackMsec = 1000;

// A fade: a gain going in a straight line over the ring's frames [s_fadeFrom, s_fadeTo), counted
// as paintedtime and s_rawend are. A fade-out goes from full volume down to silence at s_fadeTo,
// and no music is stored past it; a fade-in comes up from silence, and the music after it plays
// at full volume.
enum class Fade : u8
{
    None,
    Out,
    In,
};

static MusicStream s_stream;

static bool s_initialized = false;
static bool s_noExtMusic  = false; // -noextmusic on the command line, as bgmusic.c.
static bool s_extMusicOn  = true;  // bgm_extmusic, as BGM_Update last saw it.
static bool s_paused      = false; // The game is paused, or music_pause.
static bool s_ringPrimed  = false; // s_rawsamples holds music the mixer hasn't painted yet.
static bool s_held        = false; // HoldMusic: nothing is decoded until ReleaseMusic.

static Fade s_fade        = Fade::None;
static int  s_fadeFrom    = 0;
static int  s_fadeTo      = 0;
static bool s_fadeInArmed = false; // ReleaseMusic: the next music stored starts a fade-in.

static int  s_requestedTrack = 0;  // The map's track, started again when bgm_extmusic comes on.
static int  s_playingTrack   = 0;  // The track open in s_stream; 0 for none, or a "music" file.
static char s_playingPath[MAX_OSPATH] = {};

static bool s_warnedMissing[kMaxTrack + 1] = {};

// For a track at exactly twice the mixer's rate: a 44.1kHz WAV at 22050. See half_band.h.
static HalfBandDecimator s_halfBand;

// Linear resampler, for any other rate mismatch: a 22050Hz track at -mixspeed 44100, or a WAV
// at an odd rate. 15-bit phase keeps the products inside 32 bits; the R5900 has no 64-bit
// multiply.
constexpr int kPhaseBits = 15;
constexpr int kPhaseOne  = 1 << kPhaseBits;

static int s_resamplePhase   = kPhaseOne; // Position between s_resamplePrev and s_resampleNext.
static int s_resamplePrev[2] = {};
static int s_resampleNext[2] = {};
static s16 s_sourceFrames[kPumpFrames * 2];
static int s_sourceCount     = 0;
static int s_sourcePos       = 0;

bool SoundRunning()
{
    return (shm != nullptr) && (shm->buffer != nullptr);
}

bool ExtMusicEnabled()
{
    return !s_noExtMusic && (bgm_extmusic.value != 0.0f);
}

void ResetRateConversion()
{
    s_halfBand.Reset();
    s_resamplePhase = kPhaseOne;
    s_resamplePrev[0] = s_resamplePrev[1] = 0;
    s_resampleNext[0] = s_resampleNext[1] = 0;
    s_sourceCount = s_sourcePos = 0;
}

// Throws away the music already queued in the ring but not painted yet, so a stop or a track
// change is heard at once rather than a ring's worth (~350 ms) later. bgmusic.c's BGM_Stop
// zeroed s_rawend to the same end.
void DropQueuedMusic()
{
    if (s_ringPrimed && SoundRunning() && s_rawend > paintedtime)
    {
        s_rawend = paintedtime;
    }
    s_ringPrimed = false;
}

void CloseTrack(const bool dropQueued)
{
    s_stream.Close();
    s_playingTrack   = 0;
    s_playingPath[0] = '\0';

    // A fade belongs to the music it was laid on: the next track starts at full volume.
    s_fade        = Fade::None;
    s_fadeInArmed = false;

    if (dropQueued)
    {
        DropQueuedMusic();
    }
}

// Opens the first of `relatives` (paths under a game directory) found as a loose file, trying
// each game directory in turn, the mod's before id1's, as any other game file - but loose
// only: the stream reads through its own file descriptor, not the pak code. Every name is
// tried in one directory before the next, so a mod's track wins whatever its format, as in
// bgmusic.c. Returns false, quietly, when none is there.
bool OpenMusicFile(const char * const * relatives, const int count)
{
    char path[MAX_OSPATH];

    for (const searchpath_t * search = com_searchpaths; search != nullptr; search = search->next)
    {
        if (search->pack != nullptr)
        {
            continue;
        }

        for (int i = 0; i < count; ++i)
        {
            q_snprintf(path, sizeof(path), "%s/%s", search->filename, relatives[i]);

            switch (s_stream.Open(path, bgmloop ? MusicStream::kLoopForever : 0))
            {
            case MusicStream::OpenResult::Opened:
                q_strlcpy(s_playingPath, path, sizeof(s_playingPath));
                ps2::debug::FrameLogNoteOpen(relatives[i]); // as COM_FindFile names the engine's opens
                return true;

            case MusicStream::OpenResult::Unusable:
                // Already reported. The fallback is for a file that isn't there, not one
                // that is broken or didn't fit in memory - the next one most likely wouldn't
                // fit either.
                return false;

            case MusicStream::OpenResult::Missing:
                break;
            } // switch (s_stream.Open(...))
        }
    }
    return false;
}

void StreamStarted(const int track)
{
    s_playingTrack = track;
    s_paused       = false;
    ResetRateConversion();

    Con_DPrintf("Music: %s (%s, %d Hz %s)%s.\n", s_playingPath, s_stream.FormatName(), s_stream.SampleRate(),
                (s_stream.Channels() > 1) ? "stereo" : "mono", bgmloop ? ", looping" : "");
}

// bgmusic.c's BGM_PlayCDtrack once CDAudio_Play has declined.
void StartTrack(const int track)
{
    CloseTrack(true);

    if (track < 1 || track > kMaxTrack || !ExtMusicEnabled())
    {
        return; // Track 0 is how a map asks for silence.
    }

    char names[std::size(kTrackFiles)][MAX_QPATH];
    const char * relatives[std::size(kTrackFiles)];
    for (size_t i = 0; i < std::size(kTrackFiles); ++i)
    {
        q_snprintf(names[i], sizeof(names[i]), kTrackFiles[i], track);
        relatives[i] = names[i];
    }

    if (!OpenMusicFile(relatives, static_cast<int>(std::size(relatives))))
    {
        if (!s_warnedMissing[track])
        {
            s_warnedMissing[track] = true;
            Con_Printf("Couldn't find a cdrip for track %d (music/track%02d.adp or .wav)\n", track, track);
        }
        return;
    }
    StreamStarted(track);
}

// Stores `count` decoded frames at the ring's fill position. The fill position is kept in a
// local for the loop: with strict aliasing off, every ring store could alias the global
// s_rawend, and it would be reloaded and stored back once a frame.
void StoreFrames(const s16 * const frames, const int count, const int gain)
{
    const s16 * __restrict             in   = frames;
    portable_samplepair_t * __restrict ring = s_rawsamples;
    int fill = s_rawend;

    for (int i = 0; i < count; ++i)
    {
        portable_samplepair_t & out = ring[fill & (MAX_RAW_SAMPLES - 1)];
        out.left  = in[i * 2] * gain;
        out.right = in[(i * 2) + 1] * gain;
        ++fill;
    }
    s_rawend = fill;
}

// Fills up to `wanted` ring frames from a track at twice the mixer's rate, through the
// half-band decimator. Returns how many it managed before the stream ran dry.
int StoreDecimated(const int wanted, const int gain)
{
    s16 frames[HalfBandDecimator::kMaxOutputs * 2];
    int done = 0;

    while (done < wanted)
    {
        const int batch = ((wanted - done) < HalfBandDecimator::kMaxOutputs) ? (wanted - done) : HalfBandDecimator::kMaxOutputs;

        int needed = s_halfBand.InputNeeded(batch);
        if (needed > s_halfBand.Room())
        {
            needed = s_halfBand.Room();
        }
        if (needed > 0)
        {
            s_halfBand.Commit(s_stream.Decode(s_halfBand.Tail(), needed));
        }

        const int count = s_halfBand.Produce(frames, batch);
        if (count <= 0)
        {
            break;
        }
        StoreFrames(frames, count, gain);
        done += count;
    }
    return done;
}

bool PullSourceFrame(int frame[2])
{
    if (s_sourcePos == s_sourceCount)
    {
        s_sourceCount = s_stream.Decode(s_sourceFrames, kPumpFrames);
        s_sourcePos   = 0;
        if (s_sourceCount <= 0)
        {
            s_sourceCount = 0;
            return false;
        }
    }
    frame[0] = s_sourceFrames[s_sourcePos * 2];
    frame[1] = s_sourceFrames[(s_sourcePos * 2) + 1];
    ++s_sourcePos;
    return true;
}

// Fills `wanted` ring frames by linear interpolation. Returns how many it managed before the
// stream ran dry.
int StoreResampled(const int wanted, const int gain)
{
    const int step = static_cast<int>((static_cast<u32>(s_stream.SampleRate()) << kPhaseBits) / static_cast<u32>(shm->speed));

    for (int done = 0; done < wanted; ++done)
    {
        while (s_resamplePhase >= kPhaseOne)
        {
            int frame[2];
            if (!PullSourceFrame(frame))
            {
                return done;
            }
            s_resamplePrev[0] = s_resampleNext[0];
            s_resamplePrev[1] = s_resampleNext[1];
            s_resampleNext[0] = frame[0];
            s_resampleNext[1] = frame[1];
            s_resamplePhase  -= kPhaseOne;
        }

        const int left  = s_resamplePrev[0] + (((s_resampleNext[0] - s_resamplePrev[0]) * s_resamplePhase) >> kPhaseBits);
        const int right = s_resamplePrev[1] + (((s_resampleNext[1] - s_resamplePrev[1]) * s_resamplePhase) >> kPhaseBits);

        portable_samplepair_t & out = s_rawsamples[s_rawend & (MAX_RAW_SAMPLES - 1)];
        out.left  = left * gain;
        out.right = right * gain;
        ++s_rawend;

        s_resamplePhase += step;
    }
    return wanted;
}

// The mixer's frames in `msec` milliseconds.
int FramesForMsec(const int msec)
{
    return (shm->speed * msec) / 1000;
}

// Scales the ring's frames [from, to), which lie inside the fade, by the fade's gain at each.
void ApplyFade(const int from, const int to)
{
    // The gain at each frame as a 16.16 fraction of kFadeUnity, stepped along the line.
    const int length = s_fadeTo - s_fadeFrom;
    const int slope  = (kFadeUnity << 16) / length;
    const int step   = (s_fade == Fade::In) ? slope : -slope;
    int gain = ((s_fade == Fade::In) ? (from - s_fadeFrom) : (s_fadeTo - from)) * slope;

    portable_samplepair_t * __restrict ring = s_rawsamples;
    for (int i = from; i < to; ++i)
    {
        portable_samplepair_t & pair = ring[i & (MAX_RAW_SAMPLES - 1)];
        const int g = gain >> 16;
        pair.left  = ((pair.left >> 8) * g) >> (kFadeBits - 8);
        pair.right = ((pair.right >> 8) * g) >> (kFadeBits - 8);
        gain += step;
    }
}

// Lays the fade over frames just stored, [from, to): those inside it take its gain. A fade-in is
// over once the ring is past it.
void FadeStoredFrames(const int from, const int to)
{
    const int start = (from > s_fadeFrom) ? from : s_fadeFrom;
    const int end   = (to < s_fadeTo) ? to : s_fadeTo;
    if (start < end)
    {
        ApplyFade(start, end);
    }

    if (s_fade == Fade::In && to >= s_fadeTo)
    {
        s_fade = Fade::None;
    }
}

// Tops the raw-sample ring up to kRawLeadFrames ahead of the mixer.
void PumpMusic()
{
    if (!SoundRunning())
    {
        s_ringPrimed = false;
        return;
    }

    if (s_rawend < paintedtime)
    {
        // The mixer painted past everything we had queued: a frame took longer than the
        // lead, or the reads fell behind. As in S_RawSamples, carry on from here.
        if (s_ringPrimed) [[unlikely]]
        {
            Con_DPrintf("Music: underrun (%d frames).\n", paintedtime - s_rawend);
        }
        s_rawend = paintedtime;
    }

    // Held music comes back with a fade-in, from where the fade-out stopped storing it.
    if (s_fadeInArmed)
    {
        s_fadeInArmed = false;
        s_fade        = Fade::In;
        s_fadeFrom    = s_rawend;
        s_fadeTo      = s_rawend + FramesForMsec(kFadeInMsec);
        Con_DPrintf("Music: fading back in over %d ms.\n", kFadeInMsec);
    }

    int wanted = (paintedtime + kRawLeadFrames) - s_rawend;
    if (s_fade == Fade::Out && wanted > s_fadeTo - s_rawend)
    {
        wanted = s_fadeTo - s_rawend; // The music stops where a fade-out ends, until ReleaseMusic.
    }
    if (wanted <= 0)
    {
        return;
    }

    const int storedFrom = s_rawend;
    const int gain = static_cast<int>((bgmvolume.value * static_cast<float>(kUnityGain)) + 0.5f);
    const int rate = s_stream.SampleRate();

    if (rate == shm->speed)
    {
        s16 frames[kPumpFrames * 2];
        while (wanted > 0)
        {
            const int count = s_stream.Decode(frames, (wanted < kPumpFrames) ? wanted : kPumpFrames);
            if (count <= 0)
            {
                break;
            }
            StoreFrames(frames, count, gain);
            wanted -= count;
            s_ringPrimed = true;
        }
    }
    else if (((rate == 2 * shm->speed) ? StoreDecimated(wanted, gain) : StoreResampled(wanted, gain)) > 0)
    {
        s_ringPrimed = true;
    }

    if (s_fade != Fade::None)
    {
        FadeStoredFrames(storedFrom, s_rawend);
    }
}

// Called with the stream finished: all of its music is in the ring, still playing out.
void TrackFinished()
{
    char path[MAX_OSPATH];
    q_strlcpy(path, s_playingPath, sizeof(path));
    const int  track  = s_playingTrack;
    const bool failed = s_stream.Failed();

    CloseTrack(false); // Keep the tail that is still queued.

    // music_loop came on while the music was playing once: it starts over, as bgmusic.c's
    // stream would have looped. (Music opened with looping on never finishes.)
    if (!failed && bgmloop && s_stream.Open(path, MusicStream::kLoopForever) == MusicStream::OpenResult::Opened)
    {
        q_strlcpy(s_playingPath, path, sizeof(s_playingPath));
        StreamStarted(track);
    }
}

// --- Console commands, bgmusic.c's -------------------------------------------------------------

void MusicCommand()
{
    if (Cmd_Argc() != 2)
    {
        Con_Printf("music <musicfile>\n");
        return;
    }
    BGM_Play(Cmd_Argv(1));
}

void MusicPauseCommand()
{
    BGM_Pause();
}

void MusicResumeCommand()
{
    BGM_Resume();
}

void MusicLoopCommand()
{
    if (Cmd_Argc() == 2)
    {
        const char * const value = Cmd_Argv(1);
        if (q_strcasecmp(value, "0") == 0 || q_strcasecmp(value, "off") == 0)
        {
            bgmloop = false;
        }
        else if (q_strcasecmp(value, "1") == 0 || q_strcasecmp(value, "on") == 0)
        {
            bgmloop = true;
        }
        else if (q_strcasecmp(value, "toggle") == 0)
        {
            bgmloop = !bgmloop;
        }

        // Off ends the track with the pass playing now. On is taken up when it ends.
        if (!bgmloop && s_stream.IsOpen())
        {
            s_stream.StopLooping();
        }
    }

    Con_Printf(bgmloop ? "Music will be looped\n" : "Music will not be looped\n");
}

void MusicStopCommand()
{
    BGM_Stop();
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Holding the music through a stall (cd_audio.h)
// ------------------------------------------------------------------------------------------------

void ps2::audio::HoldMusic()
{
    if (s_held)
    {
        return;
    }
    s_held        = true;
    s_fadeInArmed = false;

    const int queued = SoundRunning() ? (s_rawend - paintedtime) : 0;
    if (!s_ringPrimed || queued <= 0)
    {
        s_fade = Fade::None;
        return; // Nothing is playing: it stays that way.
    }

    // The fade-out runs past what is queued only while there is music to decode into it: not
    // while it holds still (paused, volume 0), when the track has run out or closed with its tail
    // still queued, or over a fade-in still under way, whose ramp is on the queued frames but not
    // yet on what would follow them.
    const bool decodeMore = (s_fade != Fade::In) && !s_paused && bgmvolume.value > 0.0f && s_stream.IsOpen() &&
                            !s_stream.Finished();
    const int  fadeOut    = FramesForMsec(kFadeOutMsec);
    const int  length     = (decodeMore && queued < fadeOut) ? fadeOut : queued;

    s_fade     = Fade::Out;
    s_fadeFrom = paintedtime;
    s_fadeTo   = paintedtime + length;
    ApplyFade(s_fadeFrom, s_rawend);

    // Then mix on, as frames would, until the fade is heard to its end: where the music stops,
    // which is the fade's end unless the stream runs dry first.
    const int startMsec = ps2::sys::Milliseconds();
    const int latency   = FramesForMsec(kOutputLatencyMsec);
    const int deadline  = startMsec + ((length * 1000) / shm->speed) + kHoldSlackMsec;
    for (;;)
    {
        if (decodeMore && s_rawend < s_fadeTo)
        {
            PumpMusic();
        }
        S_ExtraUpdate();

        const bool more = decodeMore && s_rawend < s_fadeTo && !s_stream.Finished();
        const int  end  = (more || s_rawend > s_fadeTo) ? s_fadeTo : s_rawend;
        if (soundtime - end >= latency)
        {
            s_ringPrimed = false; // All played: the empty ring after the hold is no underrun.
            break;
        }
        if (ps2::sys::Milliseconds() - deadline >= 0) [[unlikely]]
        {
            Con_DPrintf("Music: the mixer stopped short of the fade's end.\n");
            break;
        }
        Sys_Sleep(kHoldPollMsec);
    }
    s_fade = Fade::None;

    // The wait is the fade, the output latency and whatever the mixer had painted ahead when it
    // began: a wait of no more than the first two means that ran out, and the music stopped short
    // of its fade.
    Con_DPrintf("Music: held, faded out over %d ms (%d frames, %d of them queued) in %d ms.\n",
                (length * 1000) / shm->speed, length, queued, ps2::sys::Milliseconds() - startMsec);
}

void ps2::audio::ReleaseMusic()
{
    if (!s_held)
    {
        return;
    }
    s_held        = false;
    s_fade        = Fade::None;
    s_fadeInArmed = true;
}

// ------------------------------------------------------------------------------------------------
// BGM_*: the soundtrack
// ------------------------------------------------------------------------------------------------

extern "C" {

// The options menu's "Background Music" (QuakeSpasm's "External Music"), and the music_loop
// setting, both bgmusic.c's.
cvar_t bgm_extmusic = ps2::MakeCvar("bgm_extmusic", "1", CVAR_ARCHIVE);
qboolean bgmloop = true;

qboolean BGM_Init()
{
    Cvar_RegisterVariable(&bgm_extmusic);
    Cmd_AddCommand("music", MusicCommand);
    Cmd_AddCommand("music_pause", MusicPauseCommand);
    Cmd_AddCommand("music_resume", MusicResumeCommand);
    Cmd_AddCommand("music_loop", MusicLoopCommand);
    Cmd_AddCommand("music_stop", MusicStopCommand);

    s_noExtMusic  = (COM_CheckParm("-noextmusic") != 0);
    s_extMusicOn  = ExtMusicEnabled();
    bgmloop       = true;
    s_initialized = true;
    return true;
}

void BGM_Shutdown()
{
    BGM_Stop();
}

// The "music" command: music/<name>, with an .adp or .wav extension or none, which tries both.
void BGM_Play(const char * filename)
{
    BGM_Stop();

    if (filename == nullptr || filename[0] == '\0')
    {
        Con_DPrintf("null music file name\n");
        return;
    }

    char name[MAX_QPATH];
    const char * const extension = COM_FileGetExtension(filename);
    bool opened = false;

    const char * const relatives[] = { name };
    if (extension[0] == '\0')
    {
        // Each extension through every game directory, then the next, as bgmusic.c tried its
        // codecs.
        for (const char * const ext : kMusicExtensions)
        {
            q_snprintf(name, sizeof(name), "music/%s.%s", filename, ext);
            if (OpenMusicFile(relatives, 1))
            {
                opened = true;
                break;
            }
        }
    }
    else if (q_strcasecmp(extension, "adp") == 0 || q_strcasecmp(extension, "wav") == 0)
    {
        q_snprintf(name, sizeof(name), "music/%s", filename);
        opened = OpenMusicFile(relatives, 1);
    }
    else
    {
        Con_Printf("Unhandled extension for %s\n", filename);
        return;
    }

    if (!opened)
    {
        Con_Printf("Couldn't handle music file %s\n", filename);
        return;
    }
    StreamStarted(0);
}

void BGM_PlayCDtrack(byte track, qboolean looping)
{
    BGM_Stop();
    s_requestedTrack = track;

    if (CDAudio_Play(track, looping) == 0)
    {
        return; // Never, on the PS2.
    }
    StartTrack(track);
}

void BGM_Stop()
{
    if (s_initialized)
    {
        CloseTrack(true);
    }
}

void BGM_Pause()
{
    if (s_stream.IsOpen())
    {
        s_paused = true;
    }
}

void BGM_Resume()
{
    s_paused = false;
}

void BGM_Update()
{
    if (!s_initialized)
    {
        return;
    }

    PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::Music);

    if (bgmvolume.value < 0.0f || bgmvolume.value > 1.0f)
    {
        Cvar_SetValueQuick(&bgmvolume, (bgmvolume.value < 0.0f) ? 0.0f : 1.0f);
    }

    const bool extMusicOn = ExtMusicEnabled();
    if (extMusicOn != s_extMusicOn)
    {
        s_extMusicOn = extMusicOn;
        if (!extMusicOn)
        {
            CloseTrack(true);
        }
        else if (s_requestedTrack > 0)
        {
            StartTrack(s_requestedTrack);
        }
    }

    // As bgmusic.c, the music holds still at volume 0 rather than playing on unheard - and while
    // HoldMusic holds it.
    if (!s_stream.IsOpen() || s_paused || s_held || bgmvolume.value <= 0.0f)
    {
        return;
    }

    PumpMusic();

    const int wraps = s_stream.TakeWraps();
    if (wraps > 0)
    {
        Con_DPrintf("Music: %s looped.\n", s_playingPath);
    }

    if (s_stream.Finished())
    {
        TrackFinished();
    }
}

// ------------------------------------------------------------------------------------------------
// CDAudio_*: no audio CD on the PS2 port
// ------------------------------------------------------------------------------------------------

int CDAudio_Init()
{
    return -1;
}

int CDAudio_Play(byte track, qboolean looping)
{
    (void)track;
    (void)looping;
    return -1; // no disc: BGM_PlayCDtrack streams the track instead
}

void CDAudio_Stop() {}
void CDAudio_Pause() {}
void CDAudio_Resume() {}
void CDAudio_Shutdown() {}
void CDAudio_Update() {}

// QuakeSpasm's sound init and shutdown start and stop its codec layer, which the PS2 build leaves
// out with bgmusic.c.
void S_CodecInit() {}
void S_CodecShutdown() {}

} // extern "C"
