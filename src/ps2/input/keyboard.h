#pragma once
/* ================================================================================================
 * File: keyboard.h
 * Brief: USB keyboard abstraction over the ps2kbd IOP driver. The Keyboard class owns the driver
 *        bring-up (the usbd + ps2kbd IRX modules) and hands over the driver's key transitions one
 *        at a time, translated from raw USB HID usages into Quake keys and the characters they
 *        type. The input seam (input.cpp) drives a single static instance, gated by the
 *        in_keyboard cvar: a keyboard is optional, and the gamepad is unaffected by it.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <tamtypes.h>

namespace ps2::input {

class Keyboard final
{
public:
    // One key transition, as the driver reported it.
    struct Event
    {
        u8   usage; // The USB HID usage (keyboard page) the driver sent, for in_debugkeys.
        bool down;
        int  key;   // Quake key for Key_Event: a K_* constant or a lowercase ASCII character. 0 = none.
        int  ch;    // The character the key types for Char_Event, Shift applied (US layout). 0 = none.
    };

    // Registers in_keyboardmap. Call once, during Host_Init, whether or not a driver ever comes up:
    // QuakeSpasm refuses new commands after Host_Init, and the keyboard may be switched on later.
    static void RegisterCommands();

    // Starts the IOP-side keyboard driver and opens it in raw (scan code) mode. Returns false
    // when the driver can't be brought up, after which the keyboard stays silent for good:
    // NextEvent() then reports nothing. Success only means the driver is running; no keyboard
    // needs to be plugged in.
    bool Init();
    void Shutdown();

    // Takes the next key transition off the driver's queue into 'event'. Returns false once the
    // queue is empty, and always while there is no driver. Each call is a SIF RPC, so the caller
    // bounds how many it makes per frame; what is left stays queued for the next frame. Taking
    // the events one at a time keeps re-entry safe: a key handler that polls for input itself
    // (SCR_ModalMessage's Y/N prompt) takes the next ones off the same queue.
    bool NextEvent(Event & event);

    // Drops whatever the driver has queued and forgets which Shift keys were down: for a
    // keyboard switched back on, so keys typed while it was off don't arrive late.
    void Flush();

private:
    bool m_available = false;

    // Which Shift keys are down: bit 0 the left one, bit 1 the right one. The driver reports raw
    // usages, so the typed characters' case is the input layer's to work out, as an OS's is.
    u8 m_shiftKeys = 0;
};

} // namespace ps2::input
