/* ================================================================================================
 * File: input.cpp
 * Brief: QuakeSpasm's IN_* input seam (input.h), over the DualShock pad and an optional USB
 *        keyboard, in place of in_sdl.c.
 *
 *        The pad follows QuakeSpasm's game controller model. Its buttons send the controller
 *        keys the menus already take (Cross is K_ABUTTON, their Enter; Circle K_BBUTTON, their
 *        Escape), the D-pad sends the arrow keys, Start and Select send Escape and Tab, and the
 *        sticks move and look through in_sdl.c's joy_* cvars, dead zones and easing. Held
 *        buttons repeat, so menus scroll, and outside a game the left stick works as the arrows.
 *
 *        The keyboard sends Quake keys by position (the US layout) to Key_Event, and the
 *        characters they type to Char_Event while the engine takes text, as QuakeSpasm's SDL2
 *        backend does with SDL's text input. The last key pressed repeats while it is held.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/engine_hooks.h"
#include "ps2/input/keyboard.h"
#include "ps2/input/pad.h"
#include "ps2/input/rumble.h"
#include "ps2/system/sys.h"
#include "ps2/save/save_system.h"
#include "ps2/math/math.h"

#include <algorithm>
#include <cmath>
#include <utility>

// IN_Init reads in_keyboard out of config.cfg ahead of the configs, as VID_Init does the GS's
// cvars, so a keyboard switched off never has its driver loaded.
extern "C" {
    #include "quake/cfgfile.h"
}

extern "C" {
// Engine cvars that no header declares (in_sdl.c declared them for itself too).
extern cvar_t sv_maxspeed;
extern cvar_t cl_maxpitch;
extern cvar_t cl_minpitch;
} // extern "C"

namespace {

// ------------------------------------------------------------------------------------------------
// Cvars
// ------------------------------------------------------------------------------------------------

// QuakeSpasm's game controller cvars (in_sdl.c's), with its defaults. The dead zones are circular,
// as a fraction of the stick's travel, and the outer thresholds are how far short of the rim
// counts as full deflection. The exponents shape the response past the dead zone (1 is linear);
// the sensitivities are degrees per second at full deflection. joy_deadzone_trigger is left out:
// the DualShock's L2 and R2 are buttons.
static cvar_t s_joyDeadzoneLook       = ps2::MakeCvar("joy_deadzone_look",        "0.175", CVAR_ARCHIVE);
static cvar_t s_joyDeadzoneMove       = ps2::MakeCvar("joy_deadzone_move",        "0.175", CVAR_ARCHIVE);
static cvar_t s_joyOuterThresholdLook = ps2::MakeCvar("joy_outer_threshold_look", "0.02",  CVAR_ARCHIVE);
static cvar_t s_joyOuterThresholdMove = ps2::MakeCvar("joy_outer_threshold_move", "0.02",  CVAR_ARCHIVE);
static cvar_t s_joySensitivityYaw     = ps2::MakeCvar("joy_sensitivity_yaw",      "240",   CVAR_ARCHIVE);
static cvar_t s_joySensitivityPitch   = ps2::MakeCvar("joy_sensitivity_pitch",    "130",   CVAR_ARCHIVE);
static cvar_t s_joyInvert             = ps2::MakeCvar("joy_invert",               "0",     CVAR_ARCHIVE);
static cvar_t s_joyExponent           = ps2::MakeCvar("joy_exponent",             "2",     CVAR_ARCHIVE);
static cvar_t s_joyExponentMove       = ps2::MakeCvar("joy_exponent_move",        "2",     CVAR_ARCHIVE);
static cvar_t s_joySwapMoveLook       = ps2::MakeCvar("joy_swapmovelook",         "0",     CVAR_ARCHIVE);

// QuakeSpasm's switch for the whole controller. Here it switches the sticks only: the pad is all
// the input most PS2s have, and buttons switched off by an archived cvar would leave no way to
// switch them back on.
static cvar_t s_joyEnable = ps2::MakeCvar("joy_enable", "1", CVAR_ARCHIVE);

// The USB keyboard. Switching it on the first time brings its IOP driver up; switching it off
// lets go of whatever it holds down.
static cvar_t s_inKeyboard = ps2::MakeCvar("in_keyboard", "1", CVAR_ARCHIVE);

// QuakeSpasm's key event trace, here every pad button and keyboard usage as it arrives, mapped or
// not. Which USB usage a physical key sends depends on the keyboard's layout (and, under PCSX2,
// on how it translates host keys), so this is the trace that tells why a key does nothing.
static cvar_t s_inDebugKeys = ps2::MakeCvar("in_debugkeys", "0", CVAR_NONE);

// ------------------------------------------------------------------------------------------------
// The PS2's defaults
// ------------------------------------------------------------------------------------------------

// What cmd.c runs right after default.cfg (PS2_DefaultConfig). id's default.cfg binds nothing to
// the pad, so these are the binds QuakeSpasm's own default.cfg adds - the triggers attack and
// jump, the shoulders cycle the weapons - plus Cross to jump, as on most PlayStation games, and
// Circle to swim down. Circle is also the menus' Back: unbound, leaving a menu with it would
// print "BBUTTON is unbound" over the game. Always run is on, so that the stick's deflection
// sets the pace up to Quake's run rather than its walk.
constexpr char kDefaultConfig[] =
    "bind RTRIGGER +attack\n"
    "bind LTRIGGER +jump\n"
    "bind RSHOULDER \"impulse 10\"\n"
    "bind LSHOULDER \"impulse 12\"\n"
    "bind ABUTTON +jump\n"
    "bind BBUTTON +movedown\n"
    "cl_alwaysrun 1\n";

// ------------------------------------------------------------------------------------------------
// Devices
// ------------------------------------------------------------------------------------------------

static ps2::input::GamePad  s_gamepad;
static ps2::input::Keyboard s_keyboard;

// Mirrors in_keyboard, and whether the driver has been tried: a one-shot, since the IOP modules
// must not be loaded twice, and a failed attempt isn't worth repeating every frame.
static bool s_keyboardEnabled   = false;
static bool s_keyboardInitTried = false;

// IN_SendKeyEvents' cap on keyboard events per call. Each is a SIF round trip, so this bounds
// what a jammed key can cost a frame; whatever is left is taken next frame.
constexpr int kMaxKeyboardEventsPerPoll = 64;

// Held keys repeat after half a second: a pad's ten times a second (in_sdl.c's timing), a
// keyboard's twice as fast, as a PC's do. Key_Event drops repeats in a game, so they only reach
// the menus, the console and the chat line.
constexpr int kRepeatDelayMs            = 500;
constexpr int kPadRepeatIntervalMs      = 100;
constexpr int kKeyboardRepeatIntervalMs = 50;

// How far the left stick goes before it presses an arrow key outside a game: in_sdl.c's 0.9 of
// its travel.
constexpr float kStickKeyThreshold = 0.9f;

bool SticksActive()
{
    return (s_joyEnable.value != 0.0f) && s_gamepad.AnalogValid();
}

void ReadInitCvars()
{
    const char * vars[] = { s_inKeyboard.name };

    ps2::save::ReadConfigCvars(vars, ps2::ArrayLength(vars));
    CFG_ReadCvarOverrides(vars, ps2::ArrayLength(vars));
}

// ------------------------------------------------------------------------------------------------
// Pad keys
// ------------------------------------------------------------------------------------------------

// What each button sends: in_sdl.c's game controller mapping, with the DualShock's buttons in the
// places of the controller SDL2 models (Cross is A, Circle B, Square X, Triangle Y, Select Back).
struct ButtonKey
{
    u16          button; // PAD_* bit
    int          key;
    const char * name;   // For in_debugkeys.
};

constexpr ButtonKey kButtonKeys[] = {
    { PAD_CROSS,    K_ABUTTON,    "Cross"    },
    { PAD_CIRCLE,   K_BBUTTON,    "Circle"   },
    { PAD_SQUARE,   K_XBUTTON,    "Square"   },
    { PAD_TRIANGLE, K_YBUTTON,    "Triangle" },
    { PAD_L1,       K_LSHOULDER,  "L1"       },
    { PAD_R1,       K_RSHOULDER,  "R1"       },
    { PAD_L2,       K_LTRIGGER,   "L2"       },
    { PAD_R2,       K_RTRIGGER,   "R2"       },
    { PAD_L3,       K_LTHUMB,     "L3"       },
    { PAD_R3,       K_RTHUMB,     "R3"       },
    { PAD_UP,       K_UPARROW,    "Up"       },
    { PAD_DOWN,     K_DOWNARROW,  "Down"     },
    { PAD_LEFT,     K_LEFTARROW,  "Left"     },
    { PAD_RIGHT,    K_RIGHTARROW, "Right"    },
    { PAD_START,    K_ESCAPE,     "Start"    },
    { PAD_SELECT,   K_TAB,        "Select"   }
};

constexpr int kNumButtons = ps2::ArrayLength(kButtonKeys);

// A key the pad drives - a button, or a stick direction standing in for an arrow key - as of the
// last poll: whether it is down, and when it next repeats.
struct PadKey
{
    bool down;
    int  repeatMs;
};

static PadKey s_buttonKeys[kNumButtons] = {};
static PadKey s_stickKeys[4] = {}; // Left, right, up, down.

void TracePadKey(const char * name, const int key, const bool down)
{
    if (s_inDebugKeys.value != 0.0f)
    {
        Con_Printf("pad: %s %s -> %s\n", name, down ? "down" : "up", Key_KeynumToString(key));
    }
}

// Sends the key down when 'isDown' starts to hold, repeats while it does, and sends the key up
// when it stops: in_sdl.c's IN_JoyKeyEvent. The state is written before Key_Event is called,
// because a key handler can re-enter IN_Commands (SCR_ModalMessage polls for its Y/N answer from
// inside the menu's key handler), and the inner call must find this key already sent.
void UpdatePadKey(PadKey & padKey, const bool isDown, const int key, const char * name)
{
    const int now = ps2::sys::Milliseconds();

    if (padKey.down)
    {
        if (!isDown)
        {
            padKey.down = false;
            TracePadKey(name, key, false);
            Key_Event(key, false);
        }
        else if (now >= padKey.repeatMs)
        {
            padKey.repeatMs = now + kPadRepeatIntervalMs;
            Key_Event(key, true);
        }
    }
    else if (isDown)
    {
        padKey.down     = true;
        padKey.repeatMs = now + kRepeatDelayMs;
        TracePadKey(name, key, true);
        Key_Event(key, true);
    }
}

void SendPadKeys()
{
    for (int i = 0; i < kNumButtons; ++i)
    {
        // Read afresh for every button: a re-entered IN_Commands polls the pad again and sends
        // whatever changed there.
        const bool down = (s_gamepad.Buttons() & kButtonKeys[i].button) != 0;
        UpdatePadKey(s_buttonKeys[i], down, kButtonKeys[i].key, kButtonKeys[i].name);
    }

    // Outside a game the left stick works the menus, the console and the chat line as the arrow
    // keys. In one it moves the player instead (IN_Move), and any arrow it still holds is let go.
    const bool  arrows = (key_dest != key_game) && SticksActive();
    const float x      = arrows ? s_gamepad.LeftStickX() : 0.0f;
    const float y      = arrows ? s_gamepad.LeftStickY() : 0.0f;

    UpdatePadKey(s_stickKeys[0], x < -kStickKeyThreshold, K_LEFTARROW,  "left stick left");
    UpdatePadKey(s_stickKeys[1], x >  kStickKeyThreshold, K_RIGHTARROW, "left stick right");
    UpdatePadKey(s_stickKeys[2], y < -kStickKeyThreshold, K_UPARROW,    "left stick up");
    UpdatePadKey(s_stickKeys[3], y >  kStickKeyThreshold, K_DOWNARROW,  "left stick down");
}

// ------------------------------------------------------------------------------------------------
// Stick movement
// ------------------------------------------------------------------------------------------------

// A stick's position, each axis -1 to +1. Y grows downward, as the hardware (and SDL) reports it.
struct StickAxis
{
    float x;
    float y;
};

float Magnitude(const StickAxis & axis)
{
    return ps2::math::Sqrtf((axis.x * axis.x) + (axis.y * axis.y));
}

// in_sdl.c's IN_ApplyDeadzone: a circular inner dead zone and a circular outer threshold, with
// the magnitude rescaled so the dead zone's edge reads 0 and the threshold 1, and clamped there.
// A dead zone and threshold that leave no travel between them would divide by zero or less,
// which the EE's FPU saturates rather than turning into inf, so the range is kept positive.
StickAxis ApplyDeadzone(const StickAxis & axis, const float deadzone, const float outerThreshold)
{
    const float magnitude = Magnitude(axis);
    if (magnitude <= deadzone)
    {
        return {};
    }

    const float range        = std::max(1.0f - deadzone - outerThreshold, 0.01f);
    const float newMagnitude = std::min(1.0f, (magnitude - deadzone) / range);
    const float scale        = newMagnitude / magnitude;
    return { axis.x * scale, axis.y * scale };
}

// in_sdl.c's IN_ApplyEasing: raises the magnitude, at most 1, to 'exponent', keeping direction.
StickAxis ApplyEasing(const StickAxis & axis, const float exponent)
{
    const float magnitude = Magnitude(axis);
    if (magnitude == 0.0f)
    {
        return {};
    }

    const float scale = std::pow(magnitude, exponent) / magnitude;
    return { axis.x * scale, axis.y * scale };
}

// The speed full deflection moves at, in_sdl.c's way: a run when +speed and "always run"
// disagree, as the keyboard's movement keys do. id's own "always run" was cl_forwardspeed at
// 400, with +speed walking at that over cl_movespeedkey.
float MoveSpeed()
{
    const float maxSpeed     = sv_maxspeed.value;
    const float forwardSpeed = cl_forwardspeed.value;
    const bool  speedKey     = (in_speed.state & 1) != 0;
    const bool  alwaysRun    = (cl_alwaysrun.value != 0.0f) || (forwardSpeed >= maxSpeed);

    if (speedKey != alwaysRun)
    {
        return maxSpeed;
    }
    if (forwardSpeed >= maxSpeed)
    {
        return std::min(maxSpeed, forwardSpeed / cl_movespeedkey.value);
    }
    return forwardSpeed;
}

// ------------------------------------------------------------------------------------------------
// Keyboard keys
// ------------------------------------------------------------------------------------------------

// The keyboard key that repeats: the last one pressed, while it stays down, as on a PC. Modifiers
// never repeat, and pressing one doesn't stop another key's repeat.
struct KeyRepeat
{
    int key; // 0 = nothing repeats.
    int ch;
    int repeatMs;
};

static KeyRepeat s_keyRepeat = {};

// The Quake keys the keyboard holds down, so they can be let go when the keyboard is switched off
// mid-press - otherwise a held +attack would stick.
static bool s_keyboardHeld[MAX_KEYS] = {};

bool IsModifier(const int key)
{
    return (key == K_SHIFT) || (key == K_CTRL) || (key == K_ALT) || (key == K_COMMAND);
}

// Key_EventWithKeycode's keycode is what Y/N prompts read their answer from (SCR_ModalMessage
// takes 'y' or 'n' there). SDL2 passes its layout's key code, which for a printable key is the
// character it types unshifted: the Quake key itself.
int Keycode(const int key)
{
    return (key > ' ' && key <= '~') ? key : 0;
}

// Characters go to Char_Event only while the engine takes text, as SDL2 sends text input only
// then: the console, the chat line, and the menus' text fields and Y/N prompt.
void TypeChar(const int ch)
{
    if (ch != 0 && Key_TextEntry())
    {
        Char_Event(ch);
    }
}

void SendKeyboardKey(const int key, const bool down, const int ch)
{
    // The state first, for the same re-entry reason as the pad's (UpdatePadKey).
    s_keyboardHeld[key] = down;

    if (down && !IsModifier(key))
    {
        s_keyRepeat = { key, ch, ps2::sys::Milliseconds() + kRepeatDelayMs };
    }
    else if (!down && key == s_keyRepeat.key)
    {
        s_keyRepeat.key = 0;
    }

    Key_EventWithKeycode(key, down, Keycode(key));
    if (down)
    {
        TypeChar(ch);
    }
}

void RepeatKeyboardKey()
{
    if (s_keyRepeat.key == 0)
    {
        return;
    }

    const int now = ps2::sys::Milliseconds();
    if (now < s_keyRepeat.repeatMs)
    {
        return;
    }

    s_keyRepeat.repeatMs = now + kKeyboardRepeatIntervalMs;
    Key_EventWithKeycode(s_keyRepeat.key, true, Keycode(s_keyRepeat.key));
    TypeChar(s_keyRepeat.ch);
}

void ReleaseKeyboardKeys()
{
    s_keyRepeat.key = 0;

    for (int key = 0; key < MAX_KEYS; ++key)
    {
        if (s_keyboardHeld[key])
        {
            s_keyboardHeld[key] = false;
            Key_Event(key, false);
        }
    }
}

// Applies the current in_keyboard. Switching it on the first time brings the driver up - the IOP
// modules load on demand, so a player who never wants a keyboard never pays for one - and
// switching it off lets go of any held keys. What was typed while it was off is dropped.
void SyncKeyboardEnabled()
{
    const bool enabled = (s_inKeyboard.value != 0.0f);
    if (enabled == s_keyboardEnabled)
    {
        return;
    }
    s_keyboardEnabled = enabled;

    if (!enabled)
    {
        ReleaseKeyboardKeys();
        return;
    }

    if (!s_keyboardInitTried)
    {
        s_keyboardInitTried = true;
        if (s_keyboard.Init())
        {
            Con_Printf("USB keyboard input initialised.\n");
        }
    }
    else
    {
        s_keyboard.Flush();
    }
}

} // namespace

extern "C" {

// ------------------------------------------------------------------------------------------------
// IN_Init / IN_Shutdown
// ------------------------------------------------------------------------------------------------

void IN_Init()
{
    Cvar_RegisterVariable(&s_joyDeadzoneLook);
    Cvar_RegisterVariable(&s_joyDeadzoneMove);
    Cvar_RegisterVariable(&s_joyOuterThresholdLook);
    Cvar_RegisterVariable(&s_joyOuterThresholdMove);
    Cvar_RegisterVariable(&s_joySensitivityYaw);
    Cvar_RegisterVariable(&s_joySensitivityPitch);
    Cvar_RegisterVariable(&s_joyInvert);
    Cvar_RegisterVariable(&s_joyExponent);
    Cvar_RegisterVariable(&s_joyExponentMove);
    Cvar_RegisterVariable(&s_joySwapMoveLook);
    Cvar_RegisterVariable(&s_joyEnable);
    Cvar_RegisterVariable(&s_inKeyboard);
    Cvar_RegisterVariable(&s_inDebugKeys);
    ps2::input::Keyboard::RegisterCommands();

    ReadInitCvars();

    if (s_gamepad.Init())
    {
        Con_Printf("Gamepad input initialised.\n");
    }
    ps2::input::InitRumble(s_gamepad);

    // Quiet when no keyboard driver comes up: it is optional hardware.
    SyncKeyboardEnabled();
}

void IN_Shutdown()
{
    s_gamepad.Shutdown();
    s_keyboard.Shutdown();
}

// ------------------------------------------------------------------------------------------------
// Key events
// ------------------------------------------------------------------------------------------------

// Polls the pad and sends its keys. QuakeSpasm calls it twice a frame (Sys_SendKeyEvents, then
// Host_Frame itself), and from the loops that wait for a key (SCR_ModalMessage, Con_NotifyBox).
void IN_Commands()
{
    s_gamepad.Update();
    SendPadKeys();
    ps2::input::UpdateRumble();
}

// Drains the keyboard's queue into key events, and repeats the held key.
void IN_SendKeyEvents()
{
    SyncKeyboardEnabled();
    if (!s_keyboardEnabled)
    {
        return;
    }

    ps2::input::Keyboard::Event event;
    for (int i = 0; i < kMaxKeyboardEventsPerPoll && s_keyboard.NextEvent(event); ++i)
    {
        if (s_inDebugKeys.value != 0.0f)
        {
            if (event.key != 0)
            {
                Con_Printf("keyboard: usage 0x%02X %s -> %s\n", static_cast<unsigned>(event.usage),
                           event.down ? "down" : "up", Key_KeynumToString(event.key));
            }
            else
            {
                Con_Printf("keyboard: usage 0x%02X %s (unmapped, dropped)\n",
                           static_cast<unsigned>(event.usage), event.down ? "down" : "up");
            }
        }

        if (event.key != 0)
        {
            SendKeyboardKey(event.key, event.down, event.ch);
        }
    }

    RepeatKeyboardKey();
}

// ------------------------------------------------------------------------------------------------
// Movement
// ------------------------------------------------------------------------------------------------

// The sticks' share of the frame's move: in_sdl.c's IN_JoyMove. The left stick walks and strafes,
// the right one turns and looks (joy_swapmovelook swaps them), straight into cl.viewangles.
void IN_Move(usercmd_t * cmd)
{
    if (!SticksActive() || cl.paused || key_dest != key_game)
    {
        return;
    }

    StickAxis move = { s_gamepad.LeftStickX(),  s_gamepad.LeftStickY()  };
    StickAxis look = { s_gamepad.RightStickX(), s_gamepad.RightStickY() };
    if (s_joySwapMoveLook.value != 0.0f)
    {
        std::swap(move, look);
    }

    move = ApplyEasing(ApplyDeadzone(move, s_joyDeadzoneMove.value, s_joyOuterThresholdMove.value),
                       s_joyExponentMove.value);
    look = ApplyEasing(ApplyDeadzone(look, s_joyDeadzoneLook.value, s_joyOuterThresholdLook.value),
                       s_joyExponent.value);

    const float speed = MoveSpeed();
    cmd->sidemove    += speed * move.x;
    cmd->forwardmove -= speed * move.y;

    const float frameTime = static_cast<float>(host_frametime);
    const float invert    = (s_joyInvert.value != 0.0f) ? -1.0f : 1.0f;
    cl.viewangles[YAW]   -= look.x * s_joySensitivityYaw.value * frameTime;
    cl.viewangles[PITCH] += look.y * s_joySensitivityPitch.value * invert * frameTime;

    if (look.x != 0.0f || look.y != 0.0f)
    {
        V_StopPitchDrift();
    }

    // QuakeSpasm's variable pitch clamping, as the mouse look does it.
    if (cl.viewangles[PITCH] > cl_maxpitch.value)
    {
        cl.viewangles[PITCH] = cl_maxpitch.value;
    }
    if (cl.viewangles[PITCH] < cl_minpitch.value)
    {
        cl.viewangles[PITCH] = cl_minpitch.value;
    }
}

// ------------------------------------------------------------------------------------------------
// Focus and text input
// ------------------------------------------------------------------------------------------------

// No window to gain or lose focus, and no mouse to grab or let go of.
void IN_Activate() {}
void IN_Deactivate(qboolean free_cursor) { (void)free_cursor; }

// Nothing to switch either: whether the engine takes text is asked per keystroke (TypeChar), and
// there is no on-screen keyboard or driver text mode to bring up for it.
void IN_UpdateInputMode() {}

// Nothing calls it (keys.c's Key_ClearStates lets go of held keys), and in_sdl.c's is empty too.
void IN_ClearStates() {}

// ------------------------------------------------------------------------------------------------
// Configs
// ------------------------------------------------------------------------------------------------

const char * PS2_DefaultConfig()
{
    return kDefaultConfig;
}

} // extern "C"
