/* ================================================================================================
 * File: particles.cpp
 * Brief: The particles, drawn: QuakeSpasm's R_DrawParticles on the VU1 path. See particles.h.
 *
 *  QuakeSpasm draws a particle as one triangle from its origin out along the view's up and right,
 *  1.5 units each times a scale that grows with the particle's depth (1 + depth * 0.004, so it
 *  never shrinks under a pixel), with a disc in the corner of the texture where the triangle's
 *  right angle is. Here the disc is a texture of its own and the quad around it a GS sprite, which
 *  particles.vcl expands with the same scale: the disc comes out as wide and in the same place.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/engine_hooks.h"
#include "ps2/renderer/particles.h"
#include "ps2/renderer/view.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/profile.h"
#include "ps2/renderer/render_system.h"
#include "ps2/renderer/vu1.h"

extern "C" {
// r_part.c's: 0 draws no particles, 1 discs, 2 squares.
extern cvar_t r_particles;
} // extern "C"

namespace ps2::particles {
namespace {

// How far a particle's quad reaches along the view's up and right, at a scale of 1. QuakeSpasm's
// disc spans half of each 1.5-unit leg of its triangle, times the 1.27 it makes up for the disc's
// size with (texturescalefactor); its square, the same half without it.
constexpr float kDiscSpan   = 1.5f * 0.5f * 1.27f;
constexpr float kSquareSpan = 1.5f * 0.5f;

} // namespace

void Draw()
{
    if (r_particles.value == 0.0f || active_particles == nullptr)
    {
        return;
    }

    PS2_PROFILE_SCOPED_EVENT(prof_evt::Particles);

    int count = 0;
    for (const particle_t * p = active_particles; p != nullptr; p = p->next)
    {
        ++count;
    }

    // Each in its palette colour, opaque; the texture's coverage blends the edge.
    vu1::ParticleVertex * __restrict particles = rs::Begin<vu1::ParticleVertex *>(count);
    vu1::ParticleVertex * __restrict out = particles;
    for (const particle_t * p = active_particles; p != nullptr; p = p->next, ++out)
    {
        const u32 rgb = d_8to24table[static_cast<int>(p->color) & 0xFF] & 0x00FFFFFFu;
        out->rgba = rgb | (0x80u << 24);
        out->x    = p->org[0];
        out->y    = p->org[1];
        out->z    = p->org[2];
    }

    const bool  squares = (static_cast<int>(r_particles.value) == 2);
    const float span    = squares ? kSquareSpan : kDiscSpan;
    const math::Vec3 quadOffset = { (vup[0] + vright[0]) * span,
                                    (vup[1] + vright[1]) * span,
                                    (vup[2] + vright[2]) * span };

    rs::Submit(particles, view::ViewProjection(),
               squares ? tex::SquareParticleTexture() : tex::ParticleTexture(),
               quadOffset, rs::DrawFlags::Blended);
}

} // namespace ps2::particles
