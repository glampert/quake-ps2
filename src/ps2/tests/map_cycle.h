#pragma once
/* ================================================================================================
 * File: map_cycle.h
 * Brief: Memory smoke test that loads every map of the game data in sequence, in the order the
 *        game plays them, and logs what each one costs.
 *
 *        QuakeSpasm keeps a level in its hunk, which a map change empties, and loads models and
 *        sounds into the cache that shares it; the backend's own level data (the baked world,
 *        the lightmaps, the textures) sits in the tagged heap beside it. What each map takes of
 *        both is what sizes the hunk and shows how much RAM is left over.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#if PS2_QUAKE_DEBUG
namespace ps2::test {

// Registers the "ps2_testmaps" and "ps2_testmaps_dwell" cvars and the "ps2_testmaps_restart"
// command. VID_Init calls it, with the backend's other commands.
void InitMapCycle();

// Advances the map cycle test by one frame. Call every frame from the main loop, outside
// Host_Frame. Gated by "ps2_testmaps"; a no-op when it is 0 and once the last map has been
// visited. "ps2_testmaps_dwell" sets the seconds spent in each map after it finishes loading.
void RunMapCycle();

} // namespace ps2::test
#endif // PS2_QUAKE_DEBUG
