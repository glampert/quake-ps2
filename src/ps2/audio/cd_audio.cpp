/* ================================================================================================
 * File: cd_audio.cpp
 * Brief: The soundtrack: QuakeSpasm's CDAudio_* seam (cdaudio.h), and its BGM_* background music
 *        entry points (bgmusic.h). QuakeSpasm's own bgmusic.c streams music files through codec
 *        libraries the PS2 build doesn't have; on the PS2 the CD audio module is the soundtrack,
 *        so BGM_PlayCDtrack hands every track to it, as bgmusic.c did first anyway.
 *
 *        No music yet: the tracks are streamed from id1/music/ here when music is ported.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

extern "C" {
    #include "quake/bgmusic.h"
    #include "quake/snd_codec.h"
}

extern "C" {

// The options menu's "External Music" toggle, which bgmusic.c owned.
cvar_t bgm_extmusic = ps2::MakeCvar("bgm_extmusic", "1", CVAR_ARCHIVE);

// ------------------------------------------------------------------------------------------------
// CDAudio_*
// ------------------------------------------------------------------------------------------------

int CDAudio_Init()
{
    return 0;
}

int CDAudio_Play(byte track, qboolean looping)
{
    (void)track;
    (void)looping;
    return -1; // no track played
}

void CDAudio_Stop() {}
void CDAudio_Pause() {}
void CDAudio_Resume() {}
void CDAudio_Shutdown() {}
void CDAudio_Update() {}

// ------------------------------------------------------------------------------------------------
// BGM_*: the soundtrack is the CD audio module's
// ------------------------------------------------------------------------------------------------

qboolean BGM_Init()
{
    Cvar_RegisterVariable(&bgm_extmusic);
    return true;
}

void BGM_PlayCDtrack(byte track, qboolean looping)
{
    CDAudio_Play(track, looping);
}

void BGM_Shutdown() {}
void BGM_Stop() {}
void BGM_Update() {}
void BGM_Pause() {}
void BGM_Resume() {}

// QuakeSpasm's sound init and shutdown start and stop its codec layer, which the PS2 build leaves
// out with bgmusic.c.
void S_CodecInit() {}
void S_CodecShutdown() {}

} // extern "C"
