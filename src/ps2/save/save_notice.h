#pragma once
/* ================================================================================================
 * File: save_notice.h
 * Brief: What the screen says while a save goes to its device or comes back off it, the way PS2
 *        games say it:
 *
 *            Data is being saved to the
 *            Memory Card in slot 1.
 *
 *            Do not remove the Memory Card
 *            or reset/switch off the console.
 *
 *        in Quake's own text box, naming the device ps2_savedevice picks: the memory card, the
 *        USB drive, the hard disk or the emulator's host filesystem. On a console a card save
 *        stalls the game for a second or more, with nothing drawn or mixed meanwhile. So before the
 *        device is touched the notice goes up, in a frame shown before BeginSaveNotice returns,
 *        and the music fades out under it rather than stopping mid-note (audio::HoldMusic).
 *        Afterwards the notice stays until it has been up for a second, so a quick access can
 *        still be read, and the music fades back in. A drive is quicker than the card, and gets
 *        the same notice: every save device looks and behaves the same.
 *
 *        Over the game the notice sits on the faded view, as the engine's own dialogs do; with the
 *        loading plaque up, on the loading screen, under the plaque; before the renderer exists,
 *        over the boot splash, which is where the boot's read of the card's config.cfg finds it.
 *        Saves put it up before the engine writes the game out (PS2_SaveOpenWrite to
 *        PS2_SaveCloseWrite), loads before the device is read (PS2_SaveLoadText, or the load menu
 *        ahead of its plaque, PS2_SaveLoadStarting), and config.cfg as the save device's copy is
 *        read and written (save_api.cpp).
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::save {

class Device;

enum class SaveAccess
{
    Saving,
    Loading,
};

// Puts the notice up and the music out ahead of an access to `device`: true once both are done,
// and its saved data may be read and written until EndSaveNotice (memcard.cpp and drive.cpp
// assert it). A device that can't be used (no card in the slot, unformatted, ...) gets no notice:
// false, with the reason given by SetError. False too where nothing can be drawn (inside a
// frame, or between gs::Init and SCR_Init): a device is never touched with nobody told. True at
// once if the notice is up already: the load menu puts it up ahead of its loading plaque, and
// the load finds it there.
bool BeginSaveNotice(Device & device, SaveAccess access);

// Takes the notice down, once it has been up long enough to read, and lets the music fade back
// in. Does nothing if it isn't up.
void EndSaveNotice();

// Whether the notice is up. Every frame drawn meanwhile shows it, and is shown before the call
// that drew it returns: GL_EndRendering and the loading screen ask.
bool SaveNoticeShown();

// Draws the notice into the frame being drawn: over the game, which it fades, or on the loading
// screen.
void DrawSaveNotice(bool onLoadingScreen);

} // namespace ps2::save
