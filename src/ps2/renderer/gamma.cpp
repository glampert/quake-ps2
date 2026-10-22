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
namespace {

// QuakeSpasm's GLSL pass: pow(colour * contrast, gamma), the colour 0 to 1. Its Options sliders
// step gamma from 1 down to 0.5 (brighter) and contrast from 1 to 2. The console takes anything,
// so the curve clamps to what still shows a picture.
constexpr float kMaxGamma    = static_cast<float>(GAMMA_MAX); // vid.h's
constexpr float kMinGamma    = 1.0f / kMaxGamma;
constexpr float kMinContrast = 0.25f;
constexpr float kMaxContrast = 4.0f;

constexpr int kNumEntries = 256;

alignas(16) static u32 s_palette[kNumEntries];
alignas(16) static u8  s_lightRamp[kNumEntries];

static float s_gamma    = 1.0f;
static float s_contrast = 1.0f;
static bool  s_identity = true;  // gamma 1, contrast 1: the tables are the data's
static bool  s_pending  = false; // the tables changed since the last ApplyPendingChange

// A colour or light 'x' (0 to 1, or over for a light) through the curve.
float Curve(const float x)
{
    return std::pow(x * s_contrast, s_gamma);
}

u8 CurveByte(const u32 v)
{
    const float out = std::min(Curve(static_cast<float>(v) / 255.0f), 1.0f);
    return static_cast<u8>((out * 255.0f) + 0.5f);
}

void Rebuild()
{
    s_gamma    = std::clamp(vid_gamma.value, kMinGamma, kMaxGamma);
    s_contrast = std::clamp(vid_contrast.value, kMinContrast, kMaxContrast);
    s_identity = (s_gamma == 1.0f && s_contrast == 1.0f);

    for (int i = 0; i < kNumEntries; ++i)
    {
        // The alpha stays the data's: 255 for every index but the transparent 255.
        const u32 rgba = d_8to24table[i];
        if (s_identity)
        {
            s_palette[i] = rgba;
        }
        else
        {
            const u32 r = CurveByte(rgba & 0xFFu);
            const u32 g = CurveByte((rgba >> 8) & 0xFFu);
            const u32 b = CurveByte((rgba >> 16) & 0xFFu);
            s_palette[i] = r | (g << 8) | (b << 16) | (rgba & 0xFF000000u);
        }
    }

    // The luxel's light is its value over 128 (clut.h), through the curve without the contrast,
    // which the texture it multiplies carries. Index 0 stays 0, the alpha the alpha test drops,
    // and the rest never fall to it: a luxel's light darkened to nothing would draw its texel
    // unlit instead.
    s_lightRamp[0] = 0;
    for (int i = 1; i < kNumEntries; ++i)
    {
        const float light = std::pow(static_cast<float>(i) / 128.0f, s_gamma);
        const int   alpha = static_cast<int>((light * 128.0f) + 0.5f);
        s_lightRamp[i] = static_cast<u8>(std::clamp(alpha, 1, 255));
    }
}

void CvarChanged(cvar_t *)
{
    // The tables now, so anything built from the palette from here on takes the new curve (the sky
    // of a loading map); the GS and what was built already at the next frame.
    Rebuild();
    s_pending = true;
}

} // namespace

void Init()
{
    Rebuild();
    Cvar_SetCallback(&vid_gamma, CvarChanged);
    Cvar_SetCallback(&vid_contrast, CvarChanged);
}

const u32 * Palette()
{
    return s_palette;
}

const u8 * LightRamp()
{
    return s_lightRamp;
}

float LightCurve(const float light)
{
    if (s_identity)
    {
        return light;
    }
    return std::pow(std::max(light, 0.0f), s_gamma);
}

void ApplyPendingChange()
{
    if (!s_pending)
    {
        return;
    }
    s_pending = false;

    // The frame left drawing reads the CLUTs and the sky's layers until it is done: shown now,
    // on its vsync, so the next BeginFrame has nothing to wait for.
    rs::FinishFrameInFlight();

    gs::UpdateCluts(s_palette, s_lightRamp);
    sky::PaletteChanged();
    if (r_clearcolor.callback != nullptr)
    {
        r_clearcolor.callback(&r_clearcolor);
    }
}

} // namespace ps2::gamma
