#pragma once
/* ================================================================================================
 * File: cd_audio.h
 * Brief: What the rest of the backend asks of the soundtrack (cd_audio.cpp) beyond the engine's
 *        BGM_* and CDAudio_* seams: holding the music through a stall that would otherwise cut it
 *        off mid-note.
 *
 *        The main thread decodes the music into the mixer's raw-sample ring about 350 ms ahead of
 *        what is playing, and the mixer only paints from it while frames run. Through a stall -
 *        a memory card save, a few seconds on a console - the sound feeder plays out what was
 *        painted and then pads with silence (snd.cpp), so the music stopped dead and came back
 *        just as abruptly. HoldMusic fades it out first, and ReleaseMusic fades it back in.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::audio {

// Fades the music out and holds it. What is already queued in the ring fades out where it is,
// none of it dropped, so the music picks up where the fade ended; beyond it the music is decoded
// on, ramped down, to the fade's full length. Returns once the fade has been heard to its end,
// painting the mix meanwhile as frames would (S_ExtraUpdate): about half a second and the
// mixer's lead, or at once when no music is playing. Nothing more is decoded until ReleaseMusic.
// Nothing happens if the music is held already.
void HoldMusic();

// Lets the held music go on, fading back in from where it stopped. A track opened in the
// meantime starts at full volume, as tracks always do.
void ReleaseMusic();

} // namespace ps2::audio
