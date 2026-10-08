/* ================================================================================================
 * File: rumble.cpp
 * Brief: Force feedback - the effect each gameplay event plays, the PS2_Rumble* hooks view.c
 *        calls, and the mixer that overlaps effects onto the two motors. See rumble.h for the
 *        overview.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/engine_hooks.h"
#include "ps2/input/pad.h"
#include "ps2/input/rumble.h"
#include "ps2/system/sys.h"

#include <algorithm>

namespace {

// ------------------------------------------------------------------------------------------------
// Effects
// ------------------------------------------------------------------------------------------------

// One burst of vibration. The pad has two motors: a small one that is either on or
// off - a light, high-pitched buzz - and a large one with a variable speed for the
// heavy rumble. An effect can use either or both; a zero duration leaves one out.
struct RumbleEffect
{
    const char * name; // what in_rumbledebug prints
    u8  largeSpeed;
    u16 largeMs;
    u16 smallMs;
};

// The local player's shots, by the weapon in hand (STAT_ACTIVEWEAPON) when the server flags a
// muzzle flash on the player's entity. The nailguns and the lightning gun flash every 0.1 s:
// their pulses run longer than that and blend into a steady rumble. The axe flashes nothing.
struct WeaponRumble
{
    int weapon;
    RumbleEffect effect;
};

constexpr WeaponRumble kWeaponRumbles[] = {
    //                                         large  large  small
    //                                         speed     ms     ms
    { IT_SHOTGUN,          { "shotgun",          0xD0,   180,   120 } },
    { IT_SUPER_SHOTGUN,    { "double shotgun",   0xFF,   280,   180 } },
    { IT_NAILGUN,          { "nailgun",          0x90,   120,   120 } },
    { IT_SUPER_NAILGUN,    { "super nailgun",    0xB0,   120,   120 } },
    { IT_GRENADE_LAUNCHER, { "grenade launcher", 0xA0,   150,    80 } },
    { IT_ROCKET_LAUNCHER,  { "rocket launcher",  0xD0,   200,   120 } },
    { IT_LIGHTNING,        { "thunderbolt",      0x80,   120,   120 } },
};

// Every pickup - ammo, health, armor, a weapon, a key - sends the client a bonus flash ("bf"),
// which is all it tells the client: one effect for them all.
constexpr RumbleEffect kPickupRumble = { "pickup", 0x80, 120, 100 };

// A powerup coming on: the quad damage, the pentagram, the ring or the biosuit. See CheckPowerups.
constexpr RumbleEffect kPowerupRumble = { "powerup on", 0xFF, 500, 400 };
constexpr int kPowerupItems = IT_QUAD | IT_INVULNERABILITY | IT_INVISIBILITY | IT_SUIT;

// Damage taken scales from the weakest rumble at 0 up to the strongest at kHeavyDamage and
// beyond: a rocket's direct hit, a shambler's lightning.
constexpr int kHeavyDamage = 50;
constexpr int kDamageMinSpeed = 0x70;

RumbleEffect DamageEffect(const int damage)
{
    const int severity = std::clamp(damage, 0, kHeavyDamage);
    RumbleEffect effect = { "damage", 0, 0, 120 };
    effect.largeSpeed = static_cast<u8>(kDamageMinSpeed + (0xFF - kDamageMinSpeed) * severity / kHeavyDamage);
    effect.largeMs = static_cast<u16>(150 + 250 * severity / kHeavyDamage);
    return effect;
}

// ------------------------------------------------------------------------------------------------
// RumbleMixer
// ------------------------------------------------------------------------------------------------

// Overlaps the effects playing onto the two motors: the small one runs while any
// effect still wants it, the large one at the highest speed any running effect asks
// for. Effects are flat pulses, so the motor values only change as a pulse starts or
// ends - which keeps the IOP calls behind GamePad::SetMotors down to a few.
class RumbleMixer final
{
public:
    void Play(const RumbleEffect & effect, u32 nowMs);
    void Stop() { *this = RumbleMixer{}; }

    bool SmallOn(u32 nowMs) const { return Running(m_small, nowMs); }
    u8 LargeSpeed(u32 nowMs) const;

private:
    struct Pulse
    {
        u32 endMs = 0;
        u8  speed = 0; // 0 = slot unused. The small motor's pulse only uses 1.
    };

    // Plenty: effects last under a second, and a frame starts a few at most.
    static constexpr int kMaxLargePulses = 8;

    // Wrap-safe ordering of two times on the millisecond clock.
    static bool Before(u32 a, u32 b) { return static_cast<s32>(a - b) < 0; }
    static bool Running(const Pulse & pulse, u32 nowMs) { return pulse.speed != 0 && Before(nowMs, pulse.endMs); }

    Pulse m_small;
    Pulse m_large[kMaxLargePulses];
};

void RumbleMixer::Play(const RumbleEffect & effect, const u32 nowMs)
{
    if (effect.smallMs != 0)
    {
        const u32 endMs = nowMs + effect.smallMs;
        if (!Running(m_small, nowMs) || Before(m_small.endMs, endMs))
        {
            m_small = { endMs, 1 };
        }
    }

    if (effect.largeSpeed != 0 && effect.largeMs != 0)
    {
        // A free slot, or else the pulse closest to its end.
        Pulse * slot = &m_large[0];
        for (Pulse & pulse : m_large)
        {
            if (!Running(pulse, nowMs))
            {
                slot = &pulse;
                break;
            }
            if (Before(pulse.endMs, slot->endMs))
            {
                slot = &pulse;
            }
        }
        *slot = { nowMs + effect.largeMs, effect.largeSpeed };
    }
}

u8 RumbleMixer::LargeSpeed(const u32 nowMs) const
{
    u8 speed = 0;
    for (const Pulse & pulse : m_large)
    {
        if (Running(pulse, nowMs) && pulse.speed > speed)
        {
            speed = pulse.speed;
        }
    }
    return speed;
}

// ------------------------------------------------------------------------------------------------
// State + helpers
// ------------------------------------------------------------------------------------------------

static cvar_t s_inRumble      = ps2::MakeCvar("in_rumble", "1", CVAR_ARCHIVE);
static cvar_t s_inRumbleDebug = ps2::MakeCvar("in_rumbledebug", "0", CVAR_NONE);

static ps2::input::GamePad * s_pad = nullptr;
static RumbleMixer s_mixer;

// What the last frame's checks saw. Cleared whenever rumble isn't wanted, so a level start or
// a loaded game - which arrive with weapons flashing or powerups held - plays nothing.
static bool   s_primed         = false;
static int    s_lastItems      = 0;
static double s_lastFlashTime  = 0.0; // The player entity's msgtime at the last shot played.

Q_ALWAYS_INLINE u32 NowMs()
{
    return static_cast<u32>(ps2::sys::Milliseconds());
}

// Whether gameplay events should rumble right now; see UpdateRumble.
bool RumbleWanted()
{
    return s_pad != nullptr &&
           s_inRumble.value != 0.0f &&
           cls.state == ca_connected &&
           cls.signon == SIGNONS &&
           !cls.demoplayback &&
           key_dest == key_game &&
           !cl.paused &&
           !cl.intermission;
}

void Play(const RumbleEffect & effect)
{
    if (s_inRumbleDebug.value != 0.0f)
    {
        Con_Printf("Rumble: %s - large motor %d for %d ms, small motor %d ms\n",
                   effect.name, effect.largeSpeed, effect.largeMs, effect.smallMs);
    }
    s_mixer.Play(effect, NowMs());
}

// A shot: the server flags a muzzle flash on the player's entity for the one update the
// weapon fired in. The entity keeps the flag until the next update, so a shot is played once
// per update that carries one.
void CheckShots()
{
    const entity_t & player = cl_entities[cl.viewentity];
    if ((player.effects & EF_MUZZLEFLASH) == 0 || player.msgtime == s_lastFlashTime)
    {
        return;
    }
    s_lastFlashTime = player.msgtime;

    const int weapon = cl.stats[STAT_ACTIVEWEAPON];
    for (const WeaponRumble & entry : kWeaponRumbles)
    {
        if (entry.weapon == weapon)
        {
            Play(entry.effect);
            return;
        }
    }
}

// A powerup coming on: its bit appearing in the player's items. One picked up while the same
// kind still runs plays only as a pickup.
void CheckPowerups()
{
    const int gained = cl.items & ~s_lastItems & kPowerupItems;
    s_lastItems = cl.items;
    if (gained != 0)
    {
        Play(kPowerupRumble);
    }
}

} // namespace

namespace ps2::input {

// ------------------------------------------------------------------------------------------------
// InitRumble / UpdateRumble
// ------------------------------------------------------------------------------------------------

void InitRumble(GamePad & pad)
{
    Cvar_RegisterVariable(&s_inRumble);
    Cvar_RegisterVariable(&s_inRumbleDebug);
    s_pad = &pad;
}

void UpdateRumble()
{
    if (!RumbleWanted())
    {
        s_primed = false;
        s_mixer.Stop(); // Drops the effects too, so none resumes later.
        if (s_pad != nullptr)
        {
            s_pad->SetMotors(false, 0);
        }
        return;
    }

    if (!s_primed)
    {
        // Events are changes from the frame before, so there has to be one.
        s_primed        = true;
        s_lastItems     = cl.items;
        s_lastFlashTime = cl_entities[cl.viewentity].msgtime;
    }
    else
    {
        CheckShots();
        CheckPowerups();
    }

    const u32 nowMs = NowMs();
    s_pad->SetMotors(s_mixer.SmallOn(nowMs), s_mixer.LargeSpeed(nowMs));
}

} // namespace ps2::input

extern "C" {

// ------------------------------------------------------------------------------------------------
// PS2_Rumble* - view.c's hooks (engine_hooks.h)
// ------------------------------------------------------------------------------------------------

void PS2_RumbleDamage(const int armor, const int blood)
{
    if (RumbleWanted())
    {
        Play(DamageEffect(armor + blood));
    }
}

void PS2_RumblePickup(void)
{
    if (RumbleWanted())
    {
        Play(kPickupRumble);
    }
}

} // extern "C"
