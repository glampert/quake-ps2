#pragma once
/* ================================================================================================
 * File: particles.h
 * Brief: The particles r_part.c simulates, drawn: QuakeSpasm's R_DrawParticles on the VU1 path.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::particles {

// Draws the live particles (R_DrawParticles): each a disc in its palette colour facing the camera,
// or with r_particles 2 a square, growing with distance so it never shrinks under a pixel. VU1
// expands each into a GS sprite.
void Draw();

} // namespace ps2::particles
