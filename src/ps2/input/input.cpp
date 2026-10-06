/* ================================================================================================
 * File: input.cpp
 * Brief: QuakeSpasm's IN_* input seam (input.h).
 *
 *        No devices yet: the DualShock pad and the USB keyboard are brought up here when input
 *        is ported, after 2D rendering, so the menus they drive can be seen.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

extern "C" {

void IN_Init() {}
void IN_Shutdown() {}

// Called by Sys_SendKeyEvents, once per frame: where devices turn into Key_Event calls.
void IN_Commands() {}
void IN_SendKeyEvents() {}

// Adds analog movement to the frame's usercmd.
void IN_Move(usercmd_t * cmd) { (void)cmd; }

// There is no window to gain or lose focus, and no text input mode to switch the devices into.
void IN_Activate() {}
void IN_Deactivate(qboolean free_cursor) { (void)free_cursor; }
void IN_UpdateInputMode() {}

} // extern "C"
