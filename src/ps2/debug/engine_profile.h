/* ================================================================================================
 * File: engine_profile.h
 * Brief: Profile probes for the engine's own C code, and the frame log's file-open notes.
 *        NOTE: Shared header between C and C++.
 *
 *        The renderer's probes see only what happens inside the refresh calls, which leaves
 *        the rest of _Host_Frame - the server frame, reading and parsing what it sent, building
 *        the client's scene, moving the particles, mixing sound - as one unmeasured remainder.
 *        These split it. PS2_PROFILE_SCOPED_EVENT is a C++ destructor and the engine is C,
 *        hence a begin/end pair per site instead. The file reads (FsIo) are timed in the
 *        backend's Sys_File* functions, so they need no site here.
 *
 *        The events themselves are declared with the rest in renderer/profile.h, which is what
 *        puts them in the frame log.
 *
 *        Reading the log: the frame log rolls a row over in GL_BeginRendering, from inside
 *        SCR_UpdateScreen. Host_ServerFrame and CL_ReadFromServer run before it, so Server,
 *        ClParse and ClScene are charged to the row *before* the one whose Frame holds their
 *        time; frame_budget.py shifts them back. ClParticles, SndMix and Music run after the
 *        rollover and sit in their own row. A parse spike in the frame after a log dump is lost
 *        with the dropped row.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#ifndef PS2_DEBUG_ENGINE_PROFILE_H
#define PS2_DEBUG_ENGINE_PROFILE_H

/* One per probe site. A site never nests inside itself, so each keeps a single start time. */
enum
{
    PS2_PROF_SERVER,       /* Host_ServerFrame - physics and QuakeC; nothing runs here during a demo */
    PS2_PROF_CL_PARSE,     /* CL_ReadFromServer's message loop - reading (a demo's packets too) and
                              parsing, which spawns the temporary entities' effects */
    PS2_PROF_CL_SCENE,     /* CL_RelinkEntities and CL_UpdateTEnts - entity lerps, trails, dynamic
                              lights, lightning beams */
    PS2_PROF_CL_PARTICLES, /* CL_RunParticles - moves and expires the particles, after the frame drew */
    PS2_PROF_SND_MIX,      /* S_Update - spatialize and mix */

    PS2_PROF_SITE_COUNT
};

#if PS2_QUAKE_PROFILE

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

void PS2Quake_ProfileBegin(int site);
void PS2Quake_ProfileEnd(int site);

/* Names a file being opened in the frame log (ps2::debug::FrameLogNoteOpen). A no-op unless the
 * perf run has turned the notes on. */
void PS2Quake_FrameLogNoteOpen(const char * fileName);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#else /* PS2_QUAKE_PROFILE */

#define PS2Quake_ProfileBegin(site) ((void)0)
#define PS2Quake_ProfileEnd(site)   ((void)0)
#define PS2Quake_FrameLogNoteOpen(fileName) ((void)0)

#endif /* PS2_QUAKE_PROFILE */

#endif /* PS2_DEBUG_ENGINE_PROFILE_H */
