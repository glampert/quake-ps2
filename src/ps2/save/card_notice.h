#pragma once
/* ================================================================================================
 * File: card_notice.h
 * Brief: What the screen says while a save goes to the memory card or comes back off it, the way
 *        PS2 games say it:
 *
 *            Data is being saved to the
 *            Memory Card in slot 1.
 *
 *            Do not remove the Memory Card
 *            or reset/switch off the console.
 *
 *        in Quake's own text box. On a console a card save stalls the game for a second or more,
 *        with nothing drawn or mixed meanwhile. So before the card is touched the notice goes up,
 *        in a frame shown before BeginCardNotice returns, and the music fades out under it rather
 *        than stopping mid-note (audio::HoldMusic). Afterwards the notice stays until it has been
 *        up for a second, so a quick read can still be read, and the music fades back in.
 *
 *        Over the game the notice sits on the faded view, as the engine's own dialogs do; with the
 *        loading plaque up, on the loading screen, under the plaque; before the renderer exists,
 *        over the boot splash, which is where the boot's read of config.cfg finds it. Card saves
 *        put it up before the engine writes the game out (PS2_SaveOpenWrite to
 *        PS2_SaveCloseWrite), loads before the card is read (PS2_SaveLoadText, or the load menu
 *        ahead of its plaque, PS2_SaveLoadStarting), and the card's config.cfg as it is read and
 *        written (save_api.cpp); saves kept as host files under the emulator
 *        (ps2_savedevice "host") don't.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::save {

enum class CardAccess
{
    Saving,
    Loading,
};

// Puts the notice up and the music out ahead of a card access: true once both are done. A card
// that can't be used (none in the slot, unformatted, ...) gets no notice: false, with the reason
// given by SetError. True at once if the notice is up already: the load menu puts it up ahead of
// its loading plaque, and the load finds it there.
bool BeginCardNotice(CardAccess access);

// Takes the notice down, once it has been up long enough to read, and lets the music fade back
// in. Does nothing if it isn't up.
void EndCardNotice();

// Whether the notice is up. Every frame drawn meanwhile shows it, and is shown before the call
// that drew it returns: GL_EndRendering and the loading screen ask.
bool CardNoticeShown();

// Draws the notice into the frame being drawn: over the game, which it fades, or on the loading
// screen.
void DrawCardNotice(bool onLoadingScreen);

} // namespace ps2::save
