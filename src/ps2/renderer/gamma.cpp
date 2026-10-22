/* ================================================================================================
 * File: gamma.cpp
 * Brief: Brightness and Contrast as palette curves. See gamma.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/gamma.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/render_system.h"
#include "ps2/renderer/sky.h"

#include <algorithm>
#include <cmath>

extern "C" {
// refresh.cpp's: the clear colour is set into the render system by its callback, from the palette
// as it then showed, so the callback runs again when the palette changes.
extern cvar_t r_clearcolor;
} // extern "C"

namespace ps2::gamma {
namespace detail {

// QuakeSpasm's GLSL pass: pow(colour * contrast, gamma), the colour 0 to 1. Its Options sliders
// step gamma from 1 down to 0.5 (brighter) and contrast from 1 to 2. The console takes anything,
// so the curve clamps to what still shows a picture.
constexpr float kMaxGamma    = static_cast<float>(GAMMA_MAX); // vid.h's
constexpr float kMinGamma    = 1.0f / kMaxGamma;
constexpr float kMinContrast = 0.25f;
constexpr float kMaxContrast = 4.0f;

constexpr int kNumEntries = 256;

alignas(16) u32 g_palette[kNumEntries];
alignas(16) u8  g_lightRamp[kNumEntries];

float g_gamma    = 1.0f;
bool  g_identity = true;  // gamma 1, contrast 1: the tables are the data's

static float s_contrast = 1.0f;
static bool  s_pending  = false; // the tables changed since the last ApplyPendingChange

// A colour or light 'x' (0 to 1, or over for a light) through the curve.
Q_ALWAYS_INLINE static float Curve(const float x)
{
    return std::pow(x * s_contrast, g_gamma);
}

Q_ALWAYS_INLINE static u8 CurveByte(const u32 v)
{
    const float out = std::min(Curve(static_cast<float>(v) / 255.0f), 1.0f);
    return static_cast<u8>((out * 255.0f) + 0.5f);
}

static void Rebuild()
{
    g_gamma    = std::clamp(vid_gamma.value, kMinGamma, kMaxGamma);
    s_contrast = std::clamp(vid_contrast.value, kMinContrast, kMaxContrast);
    g_identity = (g_gamma == 1.0f && s_contrast == 1.0f);

    for (int i = 0; i < kNumEntries; ++i)
    {
        // The alpha stays the data's: 255 for every index but the transparent 255.
        const u32 rgba = d_8to24table[i];
        if (g_identity)
        {
            g_palette[i] = rgba;
        }
        else
        {
            const u32 r = CurveByte(rgba & 0xFFu);
            const u32 g = CurveByte((rgba >> 8) & 0xFFu);
            const u32 b = CurveByte((rgba >> 16) & 0xFFu);
            g_palette[i] = r | (g << 8) | (b << 16) | (rgba & 0xFF000000u);
        }
    }

    // The luxel's light is its value over 128 (clut.h), through the curve without the contrast,
    // which the texture it multiplies carries. Index 0 stays 0, the alpha the alpha test drops,
    // and the rest never fall to it: a luxel's light darkened to nothing would draw its texel
    // unlit instead.
    g_lightRamp[0] = 0;
    for (int i = 1; i < kNumEntries; ++i)
    {
        const float light = std::pow(static_cast<float>(i) / 128.0f, g_gamma);
        const int   alpha = static_cast<int>((light * 128.0f) + 0.5f);
        g_lightRamp[i] = static_cast<u8>(std::clamp(alpha, 1, 255));
    }
}

static void CvarChanged(cvar_t *)
{
    // The tables now, so anything built from the palette from here on takes the new curve (the sky
    // of a loading map); the GS and what was built already at the next frame.
    Rebuild();
    s_pending = true;
}

} // namespace detail

void Init()
{
    detail::Rebuild();
    Cvar_SetCallback(&vid_gamma, detail::CvarChanged);
    Cvar_SetCallback(&vid_contrast, detail::CvarChanged);
}

void ApplyPendingChange()
{
    if (!detail::s_pending)
    {
        return;
    }
    detail::s_pending = false;

    // The frame left drawing reads the CLUTs and the sky's layers until it is done: shown now,
    // on its vsync, so the next BeginFrame has nothing to wait for.
    rs::FinishFrameInFlight();

    gs::UpdateCluts(detail::g_palette, detail::g_lightRamp);
    sky::PaletteChanged();

    if (r_clearcolor.callback != nullptr)
    {
        r_clearcolor.callback(&r_clearcolor);
    }
}

} // namespace ps2::gamma
