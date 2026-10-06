#pragma once
/* ================================================================================================
 * File: sys.h
 * Brief: The backend's side of the system layer: its clock, read off the EE system timer (T2),
 *        and the backend's own console commands. QuakeSpasm's Sys_* seam, implemented in
 *        sys.cpp alongside these, is declared by QuakeSpasm's sys.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::sys {

// Milliseconds since the first call. 32-bit math only: the R5900 has no 64-bit multiply or
// divide, so anything wider would be libgcc calls (see sys.cpp).
int Milliseconds();

// Seconds since the first call, at the timer's full 1.736 microsecond resolution. A double, as
// QuakeSpasm's Sys_DoubleTime wants: soft-float on the EE, so not for inner loops.
double Seconds();

// Registers the backend's console commands (ps2_dump_iop_mods, the profiler's). It has to run
// inside Host_Init: Sys_Init comes before the command system exists, and QuakeSpasm refuses
// new commands once Host_Init has finished. VID_Init, the first backend seam Host_Init calls
// after its command system is up, is the caller.
void RegisterCommands();

} // namespace ps2::sys
