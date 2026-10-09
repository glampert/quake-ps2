#pragma once
/* ================================================================================================
 * File: profile.h
 * Brief: Profile events shared by more than one source file, and the CSV frame log.
 *
 *        Mostly renderer events, but not exclusively: the frame log writes one column
 *        per event and so needs every one of them declared in a single place. Sound
 *        (the sound feeder thread's time) and the engine phases probed from C live here
 *        for that reason rather than because they belong to the renderer.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/debug/profile.h"

namespace ps2::prof_evt {

PS2_PROFILE_DECLARE_EVENT(Frame);
PS2_PROFILE_DECLARE_EVENT(VSync);
PS2_PROFILE_DECLARE_EVENT(GsWait);
PS2_PROFILE_DECLARE_EVENT(DmaSend);
PS2_PROFILE_DECLARE_EVENT(DmaFlush);
PS2_PROFILE_DECLARE_EVENT(View);
PS2_PROFILE_DECLARE_EVENT(World);
PS2_PROFILE_DECLARE_EVENT(Vis);
PS2_PROFILE_DECLARE_EVENT(TexChains);
PS2_PROFILE_DECLARE_EVENT(LmChains);
PS2_PROFILE_DECLARE_EVENT(Entities);
PS2_PROFILE_DECLARE_EVENT(EntCull);
PS2_PROFILE_DECLARE_EVENT(EntShade);
PS2_PROFILE_DECLARE_EVENT(EntGeom);
PS2_PROFILE_DECLARE_EVENT(EntShadow);
PS2_PROFILE_DECLARE_EVENT(EntBrush);
PS2_PROFILE_DECLARE_EVENT(Particles);
PS2_PROFILE_DECLARE_EVENT(TurbSurfs);
PS2_PROFILE_DECLARE_EVENT(Sky);
PS2_PROFILE_DECLARE_EVENT(Ui);
PS2_PROFILE_DECLARE_EVENT(Overlay);

// The sound feeder thread's wall time since the last S_Update (audio/snd.cpp). It runs every 5 ms
// whatever the main thread is doing, so this time sits inside the other columns, not beside them.
PS2_PROFILE_DECLARE_EVENT(Sound);

// Engine phases outside the refresh calls, probed from C (see debug/engine_profile.h).
PS2_PROFILE_DECLARE_EVENT(Server);
PS2_PROFILE_DECLARE_EVENT(ClParse);
PS2_PROFILE_DECLARE_EVENT(ClScene);
PS2_PROFILE_DECLARE_EVENT(ClParticles);
PS2_PROFILE_DECLARE_EVENT(SndMix);

// Sys_FileOpenRead/Seek/Read (system/sys.cpp): the engine's file reads, inside whichever phase
// loads.
PS2_PROFILE_DECLARE_EVENT(FsIo);

// BGM_Update: the music stream's decode and raw-sample top-up (ps2/audio/cd_audio.cpp).
PS2_PROFILE_DECLARE_EVENT(Music);

} // namespace ps2::prof_evt

// ------------------------------------------------------------------------------------------------
// Frame log
// ------------------------------------------------------------------------------------------------
//
// Buffers per-frame timings and draw statistics in RAM and dumps them to stdout
// in batches as CSV, for offline analysis of a whole run (the attract loop, a
// map cycle) rather than squinting at the on-screen overlay.
//
// The dump is the expensive part, so it never happens inside the measurement:
// Capture() only writes to a RAM buffer, and Flush() - which does the printf -
// is called from the main loop *outside* the Frame scope. The frame a dump
// lands in is still stretched by it, so that one sample is discarded rather
// than logged as a spurious spike.
//
// Rows are prefixed "FLOG" so they can be grepped out of a PCSX2 emulog that
// has the engine's own console output mixed in.
namespace ps2::debug {

// Registers the ps2_frame_log cvar that turns the log on. Call once, during Host_Init.
void FrameLogInit();

// Records the frame that just completed. Call from GL_BeginRendering right after
// ProfileNewFrame(), and before rs::BeginFrame() resets the per-frame counters
// this reads.
void FrameLogCapture();

// Writes a full batch to stdout, if one is ready. Cheap no-op otherwise. Call
// from the main loop with the Frame profile scope closed.
void FrameLogFlush();

// Emits a marker row so a run can be split by map. Call from R_NewMap.
void FrameLogMarkMap(const char * mapName);

// Emits a marker row for the map cycle's perf pass, "FLOG#view,<row>,<what>": the rows after it
// look around from the viewpoint <what> describes ("<n>,<kind>,<origin>"), until the next view or
// map marker. "0,end,-" ends a level's tour: the rows up to the next map are its way out.
void FrameLogMarkView(const char * what);

// Records a file being opened, written as an "FLOG#open,<row>,<name>" line with the next dump -
// the row being the one whose columns are charged with the read. COM_FindFile calls it through
// PS2Quake_FrameLogNoteOpen for every file the engine opens, and the music stream for its tracks,
// so every mid-level load names itself in the log. Note that the server frame and the client's
// message reads, where most of those happen, run before the frame's rollover: the read then
// stretches the Frame of the row after (see debug/engine_profile.h).
//
// Off until FrameLogNoteOpens(true): only the perf run turns the notes on. A map load opens
// hundreds of files, and every note is a line the next dump sends through the IOP.
void FrameLogNoteOpen(const char * fileName);
void FrameLogNoteOpens(bool enable);

// Ends the log: writes whatever the batch still holds, rather than waiting for
// it to fill, then an "FLOG#end" row. Call once when a run finishes - without it
// the last partial batch is lost, and a capture cut short by a crash reads the
// same as one that ran to completion.
void FrameLogFinish();

#if !PS2_QUAKE_PROFILE
// No-op stubs for when the profiler is disabled.
inline void FrameLogInit() {}
inline void FrameLogCapture() {}
inline void FrameLogFlush() {}
inline void FrameLogMarkMap(const char *) {}
inline void FrameLogMarkView(const char *) {}
inline void FrameLogNoteOpen(const char *) {}
inline void FrameLogNoteOpens(bool) {}
inline void FrameLogFinish() {}
#endif // PS2_QUAKE_PROFILE

} // namespace ps2::debug
