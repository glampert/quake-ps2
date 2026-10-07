/* ================================================================================================
 * File: keyboard.cpp
 * Brief: Keyboard implementation - IOP driver bring-up, polling, and the USB HID usage -> Quake
 *        key translation. See keyboard.h for the interface.
 *
 *  The driver is read in raw mode: instead of pre-cooked ASCII it hands back the USB HID usage
 *  plus an up/down state, which is what the engine needs - Quake binds physical keys and wants a
 *  release event for every press (+attack and friends). Raw mode has no key repeat and no notion
 *  of Shift, so both are done above it: the repeat in input.cpp, and the typed character here.
 *  That mirrors QuakeSpasm's SDL2 backend, which sends keys by position (the US layout) to
 *  Key_Event and the text SDL composes, Shift applied, to Char_Event.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/input/keyboard.h"
#include "ps2/system/iop_boot.h"
#include "ps2/common.h"

#include <cstdlib>

#include <libkbd.h>

extern "C" {
// Parses a key name the way the engine's own "bind" does, so in_keyboardmap below accepts
// everything a config already uses ("~", "a", "F1", "UPARROW"). keys.c exports it but declares
// it nowhere.
int Key_StringToKeynum(const char * str);

// IRX module images embedded by the Makefile's bin2c rule (IRX_FILES). usbd is shared with the
// USB mass-storage stack in iop_boot.cpp and only started here when that stack didn't already.
extern unsigned char usbd_irx[];
extern unsigned int  size_usbd_irx;
extern unsigned char ps2kbd_irx[];
extern unsigned int  size_ps2kbd_irx;
} // extern "C"

namespace ps2::input {
namespace {

// ------------------------------------------------------------------------------------------------
// USB HID usage -> Quake key
// ------------------------------------------------------------------------------------------------

// The Shift keys, whose state decides the case of what the other keys type.
constexpr u8 kUsageLeftShift  = 0xE1;
constexpr u8 kUsageRightShift = 0xE5;

// The irregular part of USB HID Usage Page 0x07; the contiguous runs (letters, digits, function
// keys) are filled in by BuildKeyTable below. Printable keys map to their unshifted US-layout
// character, as QuakeSpasm's SDL2 scancode table does. Usages left out have no Quake key (Caps
// Lock, Scroll Lock, Print Screen, the Menu key) and are dropped.
struct UsageMapping
{
    u8 usage;
    u8 key;
};

constexpr UsageMapping kUsageMap[] = {
    { 0x27, '0'             }, // digit row wraps around: '0' sits after '9'
    { 0x28, K_ENTER         },
    { 0x29, K_ESCAPE        },
    { 0x2A, K_BACKSPACE     },
    { 0x2B, K_TAB           },
    { 0x2C, K_SPACE         },
    { 0x2D, '-'             },
    { 0x2E, '='             },
    { 0x2F, '['             },
    { 0x30, ']'             },
    { 0x31, '\\'            },
    { 0x32, '#'             }, // non-US '#'/'~', the ISO key left of Enter
    { 0x33, ';'             },
    // HID says 0x34 is "' and \"", and that is what a keyboard plugged into a real console sends.
    // PCSX2's virtual keyboard sends it for the host's '`' key though (Qt names that key
    // QuoteLeft, which PCSX2 resolves to the apostrophe usage), and 0x35 then never arrives - so
    // under the emulator this is the only way to reach the console. Costs the apostrophe, which
    // Quake binds to nothing; put it back with "in_keyboardmap 0x34 '" on real hardware.
    { 0x34, '`'             },
    { 0x35, '`'             }, // the console toggle id's default.cfg binds
    { 0x36, ','             },
    { 0x37, '.'             },
    { 0x38, '/'             },
    { 0x48, K_PAUSE         },
    { 0x49, K_INS           },
    { 0x4A, K_HOME          },
    { 0x4B, K_PGUP          },
    { 0x4C, K_DEL           },
    { 0x4D, K_END           },
    { 0x4E, K_PGDN          },
    { 0x4F, K_RIGHTARROW    },
    { 0x50, K_LEFTARROW     },
    { 0x51, K_DOWNARROW     },
    { 0x52, K_UPARROW       },
    { 0x53, K_KP_NUMLOCK    },
    { 0x54, K_KP_SLASH      },
    { 0x55, K_KP_STAR       },
    { 0x56, K_KP_MINUS      },
    { 0x57, K_KP_PLUS       },
    { 0x58, K_KP_ENTER      },
    { 0x59, K_KP_END        },
    { 0x5A, K_KP_DOWNARROW  },
    { 0x5B, K_KP_PGDN       },
    { 0x5C, K_KP_LEFTARROW  },
    { 0x5D, K_KP_5          },
    { 0x5E, K_KP_RIGHTARROW },
    { 0x5F, K_KP_HOME       },
    { 0x60, K_KP_UPARROW    },
    { 0x61, K_KP_PGUP       },
    { 0x62, K_KP_INS        },
    { 0x63, K_KP_DEL        },
    { 0x64, '\\'            }, // non-US '\'/'|', the ISO key left of Z
    // Modifiers. Left and right collapse onto the same Quake key, so releasing one half of a pair
    // releases the key - the same simplification QuakeSpasm's SDL2 backend makes.
    { 0xE0, K_CTRL          },
    { 0xE1, K_SHIFT         },
    { 0xE2, K_ALT           },
    { 0xE3, K_COMMAND       },
    { 0xE4, K_CTRL          },
    { 0xE5, K_SHIFT         },
    { 0xE6, K_ALT           },
    { 0xE7, K_COMMAND       }
};

// Flat usage -> Quake key lookup, indexed by the raw usage byte (0 = unmapped).
struct KeyTable
{
    u8 keys[256];
};

constexpr KeyTable BuildKeyTable()
{
    KeyTable table = {};

    for (int i = 0; i < 26; ++i) { table.keys[0x04 + i] = static_cast<u8>('a' + i);  } // 'a'..'z'
    for (int i = 0; i < 9;  ++i) { table.keys[0x1E + i] = static_cast<u8>('1' + i);  } // '1'..'9'
    for (int i = 0; i < 12; ++i) { table.keys[0x3A + i] = static_cast<u8>(K_F1 + i); } // F1..F12

    for (const UsageMapping & mapping : kUsageMap)
    {
        table.keys[mapping.usage] = mapping.key;
    }
    return table;
}

// The table NextEvent actually reads: the defaults above, patchable at runtime by
// in_keyboardmap. Keyboards disagree on which usage a given physical key sends (an ISO board's
// extra key is 0x64, which ANSI boards don't have at all) and emulated ones can disagree with
// every layout, so the mapping has to be adjustable.
static constinit KeyTable s_keyTable = BuildKeyTable();

// ------------------------------------------------------------------------------------------------
// Quake key -> typed character
// ------------------------------------------------------------------------------------------------

// What the US layout's printable keys type with Shift held, letters aside.
struct ShiftedChar
{
    char plain;
    char shifted;
};

constexpr ShiftedChar kShiftedChars[] = {
    { '1', '!' }, { '2', '@' }, { '3', '#' }, { '4', '$' }, { '5', '%' },
    { '6', '^' }, { '7', '&' }, { '8', '*' }, { '9', '(' }, { '0', ')' },
    { '-', '_' }, { '=', '+' }, { '[', '{' }, { ']', '}' }, { '\\', '|' },
    { ';', ':' }, { '\'', '"' }, { '`', '~' }, { ',', '<' }, { '.', '>' },
    { '/', '?' }, { '#', '~' }
};

// What the keypad types, Num Lock taken to be on: the keypad sends its own K_KP_* keys for
// binds, which nothing would ever type in the console otherwise.
struct KeypadChar
{
    int  key;
    char ch;
};

constexpr KeypadChar kKeypadChars[] = {
    { K_KP_SLASH,     '/' }, { K_KP_STAR,      '*' }, { K_KP_MINUS,   '-' }, { K_KP_PLUS, '+' },
    { K_KP_HOME,      '7' }, { K_KP_UPARROW,   '8' }, { K_KP_PGUP,    '9' },
    { K_KP_LEFTARROW, '4' }, { K_KP_5,         '5' }, { K_KP_RIGHTARROW, '6' },
    { K_KP_END,       '1' }, { K_KP_DOWNARROW, '2' }, { K_KP_PGDN,    '3' },
    { K_KP_INS,       '0' }, { K_KP_DEL,       '.' }
};

// The character a Quake key types, or 0 for one that types nothing (Enter, the arrows, F1...).
int TypedChar(const int key, const bool shift)
{
    if (key >= 'a' && key <= 'z')
    {
        return shift ? (key - 'a' + 'A') : key;
    }

    if (key >= ' ' && key <= '~')
    {
        if (shift)
        {
            for (const ShiftedChar & entry : kShiftedChars)
            {
                if (entry.plain == key)
                {
                    return entry.shifted;
                }
            }
        }
        return key;
    }

    for (const KeypadChar & entry : kKeypadChars)
    {
        if (entry.key == key)
        {
            return entry.ch;
        }
    }
    return 0;
}

// ------------------------------------------------------------------------------------------------
// Commands
// ------------------------------------------------------------------------------------------------

// in_keyboardmap <usb usage> <key name> - points one usage at a different Quake key, e.g.
// "in_keyboardmap 0x34 '" to hand 0x34 back to the apostrophe.
void KeyboardMapCmd()
{
    if (Cmd_Argc() != 3)
    {
        Con_Printf("usage: in_keyboardmap <usb usage, e.g. 0x34> <key name, e.g. ~>\n");
        return;
    }

    const long usage = std::strtol(Cmd_Argv(1), nullptr, 0); // 0 = accept 0x hex or decimal
    if (usage < 0 || usage > 255)
    {
        Con_Printf("in_keyboardmap: '%s' is not a USB usage (0-255).\n", Cmd_Argv(1));
        return;
    }

    const int key = Key_StringToKeynum(Cmd_Argv(2)); // -1 when the name is unknown
    if (key < 0 || key > 255)
    {
        Con_Printf("in_keyboardmap: unknown key '%s'.\n", Cmd_Argv(2));
        return;
    }

    s_keyTable.keys[usage] = static_cast<u8>(key);
    Con_Printf("keyboard: USB usage 0x%02lX now sends '%s' (key %d).\n",
               static_cast<unsigned long>(usage), Cmd_Argv(2), key);
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Init / Shutdown
// ------------------------------------------------------------------------------------------------

void Keyboard::RegisterCommands()
{
    Cmd_AddCommand("in_keyboardmap", KeyboardMapCmd);
}

bool Keyboard::Init()
{
    // The keyboard driver lives on the IOP and speaks USB, so it needs usbd under it. The USB
    // boot path already started usbd; host: and HDD boots didn't, so start it here. Either way a
    // driver that won't come up only costs us the keyboard, so none of this is fatal.
    if (!ps2::sys::UsbStackStarted())
    {
        if (!ps2::sys::StartIopModuleFromBuffer("usbd", usbd_irx, size_usbd_irx))
        {
            Con_Printf("WARNING: keyboard disabled!\n");
            return false;
        }
    }

    if (!ps2::sys::StartIopModuleFromBuffer("ps2kbd", ps2kbd_irx, size_ps2kbd_irx))
    {
        Con_Printf("WARNING: keyboard disabled!\n");
        return false;
    }

    if (PS2KbdInit() == 0)
    {
        Con_Printf("WARNING: PS2KbdInit failed - keyboard disabled!\n");
        return false;
    }

    // Raw mode: usages with an up/down state instead of cooked ASCII. The driver already
    // defaults to non-blocking reads (a blocking one would stall the whole frame), and the
    // flush drops anything queued before the switch.
    PS2KbdSetReadmode(PS2KBD_READMODE_RAW);
    PS2KbdSetBlockingMode(PS2KBD_NONBLOCKING);
    PS2KbdFlushBuffer();

    m_available = true;
    return true;
}

void Keyboard::Shutdown()
{
    if (m_available)
    {
        PS2KbdClose();
        m_available = false;
    }
    m_shiftKeys = 0;
}

// ------------------------------------------------------------------------------------------------
// Polling
// ------------------------------------------------------------------------------------------------

bool Keyboard::NextEvent(Event & event)
{
    if (!m_available)
    {
        return false;
    }

    PS2KbdRawKey raw = {};
    if (PS2KbdReadRaw(&raw) <= 0)
    {
        return false; // Queue drained - the usual case, nothing was typed.
    }

    const bool down = (raw.state == PS2KBD_RAWKEY_DOWN);

    if (raw.key == kUsageLeftShift || raw.key == kUsageRightShift)
    {
        const u8 bit = (raw.key == kUsageLeftShift) ? 1u : 2u;
        m_shiftKeys  = static_cast<u8>(down ? (m_shiftKeys | bit) : (m_shiftKeys & ~bit));
    }

    event.usage = raw.key;
    event.down  = down;
    event.key   = s_keyTable.keys[raw.key];
    event.ch    = TypedChar(event.key, m_shiftKeys != 0);
    return true;
}

void Keyboard::Flush()
{
    if (m_available)
    {
        PS2KbdFlushBuffer();
    }
    m_shiftKeys = 0;
}

} // namespace ps2::input
