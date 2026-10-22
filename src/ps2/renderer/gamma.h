#pragma once
/* ================================================================================================
 * File: gamma.h
 * Brief: The Options menu's Brightness and Contrast sliders, QuakeSpasm's gamma and contrast
 *        cvars, on the PS2. QuakeSpasm applies them to the finished frame, pow(colour * contrast,
 *        gamma) in a shader or as the display's gamma ramp; the GS has no such stage. Here they
 *        go into the palette instead, as software Quake's gamma did: everything the renderer
 *        draws samples Quake's palette through a CLUT, so the curve goes into the CLUTs and into
 *        the few colours the EE picks from the palette itself. The lightmaps' ramp takes the curve
 *        too, so a lit wall comes out as the shader would make it: pow(t * l, g) is
 *        pow(t, g) * pow(l, g), the texture's part in its CLUT and the light's in the ramp. The
 *        contrast is in the palette alone, so a bright texel saturates there whatever its light,
 *        where the shader saturates the lit result; the models' light takes the curve per model,
 *        their shading term stays linear, and the vertex dynamic lights, the glows and the view
 *        tint are left as they are.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <tamtypes.h>

namespace ps2::gamma {

// Builds the tables at gamma 1 and contrast 1 and hooks the two cvars. VID_Init calls it once the
// palette is loaded and the cvars are registered.
void Init();

// Re-uploads the CLUTs and rebuilds what was built from the palette if a cvar changed since the
// last call. Between frames, just before rs::BeginFrame: it finishes the frame left drawing first,
// since the CLUTs can't change under one.
void ApplyPendingChange();

// Extern globals so the accessors below can be inlined.
namespace detail {
extern u32   g_palette[];
extern u8    g_lightRamp[];
extern float g_gamma;
extern bool  g_identity;
} // namespace detail

// Quake's palette through the curve: the colour a palette index shows as, for colours that reach
// the GS from the EE rather than through a CLUT (the particles, the fills, the clear colour, the
// sky's front layer). d_8to24table stays the palette as the data has it.
inline const u32 * Palette()
{
    return detail::g_palette;
}

// The lightmap ramp through the curve: the alpha the light ramp CLUT gives each luxel value, the
// value itself at gamma 1.
inline const u8 * LightRamp()
{
    return detail::g_lightRamp;
}

// A light that reaches the GS as a vertex colour (an alias model's), through the curve.
inline float LightCurve(const float light)
{
    if (detail::g_identity)
    {
        return light;
    }
    return std::pow(std::max(light, 0.0f), detail::g_gamma);
}

} // namespace ps2::gamma
