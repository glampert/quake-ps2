#pragma once
/* ================================================================================================
 * File: rumble.h
 * Brief: Force feedback through the gamepad's two vibration motors. What happens to the local
 *        player - weapon fire, damage taken, item pickups and powerups coming on - plays a
 *        short burst from rumble.cpp's effect tables, and bursts running at the same time
 *        overlap. Damage and pickups come in through the PS2_Rumble* hooks (engine_hooks.h)
 *        view.c calls as it handles them; shots and powerups are read off the client state.
 *        The input seam (input.cpp) sends the result to the pad once a frame. Gated by the
 *        in_rumble cvar (on by default); set in_rumbledebug to echo each effect as it starts.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::input {

class GamePad;

// Registers the rumble cvars and binds the pad the effects play on. Call from IN_Init.
void InitRumble(GamePad & pad);

// Picks up the frame's shots and powerups, then runs the pad's motors from the effects
// currently playing - or stops them while rumble isn't wanted: in_rumble off, no level
// running, the game paused, a menu or the console up, or a demo playing. Call once a frame,
// after the pad's Update().
void UpdateRumble();

} // namespace ps2::input
