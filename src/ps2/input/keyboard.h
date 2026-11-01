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

    // Whether the driver's queue is worth reading this time round: always while the keyboard is
    // in use, every kIdlePollMsec otherwise. Ask once per round of NextEvent calls.
    bool PollDue() const;

    // Takes the next key transition off the driver's queue into 'event'. Returns false once the
    // queue is empty, and always while there is no driver. Each call is a read() on the driver's
    // device file, an IOP round trip, so the caller bounds how many it makes per frame; what is
    // left stays queued for the next frame. Also false, leaving the keys queued, while the music
    // loader has a read out (see aio::Pending). Taking the events one at a time keeps
    // re-entry safe: a key handler that polls for input itself (SCR_ModalMessage's Y/N prompt)
    // takes the next ones off the same queue.
    bool NextEvent(Event & event);

    // Drops whatever the driver has queued and forgets which keys were down: for a keyboard
    // switched back on, so keys typed while it was off don't arrive late.
    void Flush();

private:
    // How often the driver is polled. Every poll is a file read - an IOP round trip, which also
    // waits behind any other file I/O in flight - and the driver can't say whether a keyboard is
    // plugged in at all: its read comes back empty either way. So a keyboard nobody is using is
    // polled every kIdlePollMsec, which puts the first key after a quiet spell up to that late,
    // and one in use every frame: until kActiveMsec after the last key, and for as long as any
    // key is held, so letting go after a long hold (running forward) is never the late one.
    static constexpr int kActiveMsec   = 10 * 1000;
    static constexpr int kIdlePollMsec = 250;

    // Forgets every key held, for Shutdown and Flush.
    void ForgetHeldKeys();

    bool m_available = false;

    // Which Shift keys are down: bit 0 the left one, bit 1 the right one. The driver reports raw
    // usages, so the typed characters' case is the input layer's to work out, as an OS's is.
    u8 m_shiftKeys = 0;

    bool m_held[256]    = {};    // by raw USB usage, as the driver reports them
    int  m_numHeld      = 0;     // how many of m_held are set
    bool m_keySeen      = false; // whether m_lastKeyMsec holds anything yet
    int  m_lastKeyMsec  = 0;     // when the last raw event came, mapped or not
    int  m_lastPollMsec = 0;     // when the driver was last read
};

} // namespace ps2::input
