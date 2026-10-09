#pragma once
/* ================================================================================================
 * File: map_cycle.h
 * Brief: Unattended test that loads every level in the game data's paks in sequence: the campaign
 *        in the order the game plays it (the shareware pak0 has start and episode 1, the
 *        registered pak1 the rest), then any other levels (pak1's deathmatch arenas)
 *        alphabetically. It runs one of two passes:
 *
 *        - The memory pass ("ps2_testmaps 1") logs what each level costs. QuakeSpasm keeps a
 *          level in its hunk, which a map change empties, and loads models and sounds into the
 *          cache that shares it; the backend's own level data (the baked world, the lightmaps,
 *          the textures) sits in the tagged heap beside it. What each map takes of both is what
 *          sizes the hunk and shows how much RAM is left over.
 *        - The perf pass ("ps2_testmaps 2", profile builds) records the frame log while it looks
 *          around each level from a few spots spread across it. The perf run's demos see three
 *          levels; this sees all of them, sampled: enough to show whether a level sits over the
 *          60 fps budget and which spot in it, though not that none ever dips.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#if PS2_QUAKE_DEBUG
namespace ps2::test {

// Registers the "ps2_testmaps", "ps2_testmaps_dwell" and "ps2_testmaps_views" cvars and the
// "ps2_testmaps_restart" command. VID_Init calls it, with the backend's other commands.
void InitMapCycle();

// Advances the map cycle test by one frame. Call every frame from the main loop, outside
// Host_Frame. Gated by "ps2_testmaps"; a no-op when it is 0 and once the last map has been
// visited. Both passes print the memory lines as they leave each map.
//
// The memory pass (1) stays "ps2_testmaps_dwell" seconds in each map once it has loaded, and
// ends with "MapCycle: done." without quitting.
//
// The perf pass (2) sets up a capture as the perf run does (BeginPerfCapture in perf_run.h). In
// each map it turns god mode and notarget on, so nothing in the level ends or changes the tour,
// then visits up to "ps2_testmaps_views" viewpoints: where the player spawned, then each time
// the intermission camera, deathmatch start or teleporter exit farthest from those already
// picked, reached with "setpos" (which leaves the player in noclip). At each it writes an
// "FLOG#view" marker and turns a full circle, 2 degrees a frame. At the end it writes the frame
// log's tail and quits.
void RunMapCycle();

} // namespace ps2::test
#endif // PS2_QUAKE_DEBUG
