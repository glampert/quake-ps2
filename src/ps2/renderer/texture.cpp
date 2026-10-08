/* ================================================================================================
 * File: texture.cpp
 * Brief: Texture objects and the pool they live in. See texture.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/texture.h"
#include "ps2/renderer/gs.h" // gs::ReleaseTexture
#include "ps2/small_pool.h"

#include <cstdio>
#include <cstring>

namespace ps2::tex {
namespace {

// ------------------------------------------------------------------------------------------------
// Generated images
// ------------------------------------------------------------------------------------------------

// Checkerboards for the DebugTexture() variants, RGB16. Variant 0 (pink) is
// the classic missing-image stand-in; the others give test scenes several
// distinct textures to exercise VRAM streaming.
constexpr int kCheckerDim     = 32;
constexpr int kCheckerSquares = 4;

const u16 * MakeCheckerPattern(int variant)
{
    if (variant < 0 || variant >= kNumDebugTextures)
    {
        variant = 0;
    }

    constexpr auto Rgb16 = [](u32 r, u32 g, u32 b) -> u16
    {
        return static_cast<u16>((1u << 15) | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
    };

    // One bright color per variant, checkered against black.
    constexpr u16 variantColors[kNumDebugTextures] = {
        Rgb16(255, 100, 255), // pink
#if PS2_QUAKE_DEBUG
        Rgb16(255,  60,  60), // red
        Rgb16( 60, 255,  60), // green
        Rgb16( 80,  80, 255), // blue
        Rgb16(255, 255,  60), // yellow
        Rgb16( 60, 255, 255), // cyan
#endif // PS2_QUAKE_DEBUG
    };
    const u16 colors[2] = { variantColors[variant], Rgb16(0, 0, 0) };

    alignas(16) static u16 s_buffers[kNumDebugTextures][kCheckerDim * kCheckerDim];
    u16 * buffer = s_buffers[variant];

    constexpr int squareSize = kCheckerDim / kCheckerSquares;
    for (int y = 0; y < kCheckerDim; ++y)
    {
        for (int x = 0; x < kCheckerDim; ++x)
        {
            const int colorIndex = ((y / squareSize) + (x / squareSize)) % 2;
            buffer[x + (y * kCheckerDim)] = colors[colorIndex];
        }
    }

    return buffer;
}

// The particle images, generated rather than loaded.
//
// They are Alpha8: one coverage byte per texel, sampled through the shared alpha-ramp CLUT, which
// supplies the GS modulate identity (128) as the colour and the byte itself as the alpha. So a
// particle's colour rides entirely on its vertex colour and the image contributes only its shape.
// The ramp maps coverage 255 to alpha 128 (= 1.0 on the GS), so a fully opaque particle blends at
// exactly 1x rather than the ~2x an 0xFF alpha would give; coverage 0 maps to alpha 0, and those
// texels never reach the blender at all - the batch's alpha test drops them.
//
// Both are powers of two, so no ST rescale is needed (see StScaleFor).

// QuakeSpasm's particle disc (r_part.c's R_ParticleTextureLookup with a sharpness of 8): coverage
// 8 * (255 - r^2) about texel (16, 16), saturating, so a solid disc 16 texels in radius with a
// crisp edge. QuakeSpasm draws it in the corner of a 64-texel texture; this is that corner.
constexpr int kParticleDim = 32;

const u8 * MakeParticlePattern()
{
    alignas(16) static u8 s_buffer[kParticleDim * kParticleDim];
    for (int y = 0; y < kParticleDim; ++y)
    {
        for (int x = 0; x < kParticleDim; ++x)
        {
            const int dx = x - 16;
            const int dy = y - 16;
            const int r2 = (dx * dx) + (dy * dy);
            const int coverage = 8 * (255 - ((r2 > 255) ? 255 : r2));
            s_buffer[x + (y * kParticleDim)] = static_cast<u8>((coverage > 255) ? 255 : coverage);
        }
    }
    return s_buffer;
}

// r_particles 2's square: solid. The smallest image the upload takes, a quadword.
constexpr int kSquareParticleDim = 4;

const u8 * MakeSquareParticlePattern()
{
    alignas(16) static u8 s_buffer[kSquareParticleDim * kSquareParticleDim];
    std::memset(s_buffer, 255, sizeof(s_buffer));
    return s_buffer;
}

// ------------------------------------------------------------------------------------------------
// Texture pool
// ------------------------------------------------------------------------------------------------

using TexturePool = SmallPool<Texture, kMaxTextures>;
static TexturePool s_pool;

static const Texture * s_debugTextures[kNumDebugTextures] = {};
static const Texture * s_particleTexture       = nullptr;
static const Texture * s_squareParticleTexture = nullptr;

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

void Init()
{
    s_pool.Init(); // One-shot; asserts if called twice.

    for (int i = 0; i < kNumDebugTextures; ++i)
    {
        char name[16];
        std::snprintf(name, sizeof(name), "debug%d", i);
        s_debugTextures[i] = &Create(name, MakeCheckerPattern(i), kCheckerDim, kCheckerDim,
                                     PixelFormat::RGB16, TexComponents::RGB, ImageType::Pic,
                                     TexFlags::Builtin);
    }

    // Linear, as QuakeSpasm filters its disc: the edge would stair-step otherwise.
    Texture & particle = Create("particle", MakeParticlePattern(), kParticleDim, kParticleDim,
                                PixelFormat::Alpha8, TexComponents::RGBA, ImageType::Pic,
                                TexFlags::Builtin);
    particle.magFilter = TexFilter::Linear;
    particle.minFilter = TexFilter::Linear;
    s_particleTexture  = &particle;

    s_squareParticleTexture = &Create("particle_square", MakeSquareParticlePattern(), kSquareParticleDim,
                                      kSquareParticleDim, PixelFormat::Alpha8, TexComponents::RGBA,
                                      ImageType::Pic, TexFlags::Builtin);
}

Texture & Create(const char * name, const void * pixels, const int width, const int height,
                 const PixelFormat format, const TexComponents components, const ImageType type,
                 const TexFlags flags)
{
    PS2_Assert(width > 0 && height > 0 && pixels != nullptr);
    PS2_AssertMsg(width <= INT16_MAX && height <= INT16_MAX, "Texture width/height too big!");

    const u16 slot = s_pool.Alloc();
    if (slot == TexturePool::kInvalidIndex) [[unlikely]]
    {
        Sys_Error("Out of texture slots for '%s'! Bump tex::kMaxTextures (%u).", name, kMaxTextures);
    }

    // Pics and sprites keep crisp texels (and their transparency cutouts
    // fringe-free); skins, walls and sky get smoothed by bilinear sampling.
    // The GS filters the post-CLUT colors, so Palette8 works with Linear too.
    const TexFilter filter = (type == ImageType::Pic || type == ImageType::Sprite)
                           ? TexFilter::Nearest : TexFilter::Linear;

    Texture & texture = s_pool.Slot(slot);
    std::snprintf(texture.name, sizeof(texture.name), "%s", name);

    texture.pixels     = pixels;
    texture.width      = static_cast<s16>(width);
    texture.height     = static_cast<s16>(height);
    texture.srcWidth   = static_cast<s16>(width);
    texture.srcHeight  = static_cast<s16>(height);
    texture.type       = type;
    texture.flags      = flags;
    texture.format     = format;
    texture.components = components;
    texture.function   = TexFunction::Modulate;
    texture.magFilter  = filter;
    texture.minFilter  = filter;
    texture.atlas      = nullptr; // the caller packs it into a scrap afterwards, if it fits
    texture.atlasX     = 0;
    texture.atlasY     = 0;
    texture.vramAddr   = Texture::kNotResident;

    // The first upload writes the data cache back over the pixels before the DMA reads them:
    // whatever made them - a file read, a copy, the generators above - may have left them there.
    texture.dirtyPixels = true;

    return texture;
}

void Destroy(const Texture & texture)
{
    gs::ReleaseTexture(texture); // its GS VRAM back to the heap (no-op when not resident)
    s_pool.Free(s_pool.IndexOf(texture));
}

const Texture & DebugTexture(int variant)
{
    if (variant < 0 || variant >= kNumDebugTextures)
    {
        variant = 0;
    }
    return *s_debugTextures[variant];
}

const Texture & ParticleTexture()
{
    return *s_particleTexture;
}

const Texture & SquareParticleTexture()
{
    return *s_squareParticleTexture;
}

} // namespace ps2::tex
