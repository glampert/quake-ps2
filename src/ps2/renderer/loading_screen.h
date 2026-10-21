#pragma once
/* ================================================================================================
 * File: loading_screen.h
 * Brief: What the screen shows while a level loads: the "loading" plaque, with a status bar
 *        along the bottom naming the file being read, how many files and megabytes have been
 *        read so far, and for how long.
 *
 *        SCR_BeginLoadingPlaque draws the plaque once, over the last view, and then stops the
 *        screen updating until the level is up. With ps2_gs_latency on, though, a frame only
 *        reaches the screen when the next one begins, so that plaque sat unseen for the whole
 *        load, behind the last frame of the level before. Each loading screen frame is shown
 *        before the call that draws it returns, and the file reads keep redrawing it, so a slow
 *        load from USB shows where it is. The engine's side, PS2_LoadingScreenBegin and End and
 *        PS2_FileOpening, is in engine_hooks.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::loading {

// A file COM_FindFile opened for a read (PS2_FileOpening): named on the status bar.
void NoteFile(const char * fileName);

// A read of 'bytes' is about to start (Sys_FileRead): counted, and the screen redrawn if it is
// due - at most every few hundred milliseconds, or at once for a large file, which takes a
// while off a USB drive. Nothing happens while the plaque is down.
void NoteRead(int bytes);

// The file NoteFile named last, for the load trace's lines; "?" before any.
const char * CurrentFile();

// Draws the loading screen again at once, if the plaque is up: the save notice going up or
// coming down on it (save/save_notice.h). False, drawing nothing, while the plaque is down.
bool Redraw();

// In vid.cpp, with the frame bracket: draws one loading screen frame with 'status' on the bar
// and shows it before returning. False, drawing nothing, while a frame is open: a file read
// from inside one (an evicted model reloaded as it is drawn) leaves the screen as it is.
bool DrawLoadingScreen(const char * status);

} // namespace ps2::loading
