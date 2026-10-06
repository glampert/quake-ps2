#pragma once
/* ================================================================================================
 * File: texture.h
 * Brief: Texture/image objects for the PS2 renderer, and the pool they live in. The
 *        engine's texture manager (texmgr.cpp, QuakeSpasm's TexMgr_* seam) creates them
 *        from pixels already in memory and destroys them when their owner goes; GS VRAM
 *        residency is managed underneath (vram.h), so a texture uploads on its first bind.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/vram.h"

#include <tamtypes.h>
#include <gs_psm.h>
#include <draw_buffers.h>  // texbuffer_t
#include <draw_sampling.h> // LOD_*

namespace ps2::tex {

// What a texture is used for by the game. Mirrors the image classes of the original
// renderers, and decides the default filtering (see Create).
enum class ImageType : u8
{
    Null,   // Free slot in the pool.
    Pic,    // 2D UI/HUD image.
    Skin,   // Model skin.
    Sprite, // Sprite frame.
    Wall,   // World texture.
    Sky     // Skybox face.
};

// Which images are pre-brightened by ps2_intensity before anything multiplies
// them back down again: a wall under its lightmap, a skin or a sprite under an
// entity's shade colour. Images drawn at face value - the HUD, the menus, the
// sky - are left alone, or the compensation would just wash them out.
constexpr bool TakesIntensity(ImageType type)
{
    return (type == ImageType::Wall) ||
           (type == ImageType::Skin) ||
           (type == ImageType::Sprite);
}

// Bit-flag texture properties, orthogonal to the ImageType.
enum class TexFlags : u8
{
    None      = 0,
    Builtin   = 1 << 0, // Embedded in the ELF; always available, never unloaded.
    Mipmapped = 1 << 1  // 'pixels' carries mip levels after level 0; see MipLevels.
};

constexpr TexFlags operator|(TexFlags lhs, TexFlags rhs)
{
    return TexFlags(static_cast<u8>(lhs) | static_cast<u8>(rhs));
}

constexpr bool HasFlag(TexFlags flags, TexFlags test)
{
    return (static_cast<u8>(flags) & static_cast<u8>(test)) != 0;
}

// Pixel storage formats we support, mapped to GS PSMs by GsPsm().
enum class PixelFormat : u8
{
    RGBA32,   // 4 bytes/texel, 8888.
    RGB16,    // 2 bytes/texel, 5551 (alpha bit present but unused as TexComponents::RGB).
    Palette8, // 1 byte/texel: PSMT8 indices into the shared global-palette CLUT
              // (gs::Init uploads it once; color and alpha come from the palette entry).
    Alpha8    // 1 byte/texel: PSMT8 indices into the shared alpha-ramp CLUT, where the
              // index *is* the alpha and the color is pinned at the modulate identity.
              // For images that carry only a coverage/intensity signal and take their
              // color from the primitive: the particle sprites and the lightmap atlases.
              // Needs TexComponents::RGBA, or the texture function drops the alpha.
};

// Whether the texture's own alpha participates in the texture function (GS TCC bit).
enum class TexComponents : u8 { RGB, RGBA };

// GS texture function applied when a primitive samples the texture.
enum class TexFunction : u8 { Modulate, Decal };

// Texel filtering.
enum class TexFilter : u8 { Nearest, Linear };

// A texture or 2D image. Plain data; lives in the texture pool (Create/Destroy).
struct Texture final
{
    char          name[MAX_QPATH]; // The engine's name for it, e.g. "gfx.wad:conchars"; for logs and VRAM dumps.
    const void *  pixels;          // Pixel data in EE RAM. Not owned: whoever created the texture keeps it alive.
    s16           width;           // Of 'pixels', in pixels, > 0.
    s16           height;          // Of 'pixels', in pixels, > 0.
    s16           srcWidth;        // Size the image had on disk. Tiling world textures are resampled to the next power of two on load, width/height
    s16           srcHeight;       // hold the scaled size, what actually sits in memory and VRAM. Same as width/height for images that didn't need resampling.
    mutable bool  dirtyPixels;     // CPU rewrote 'pixels'; the next bind re-uploads them.
    ImageType     type;
    TexFlags      flags;
    PixelFormat   format;
    TexComponents components;
    TexFunction   function;
    TexFilter     magFilter;
    TexFilter     minFilter;

    // Set when the image lives inside a shared scrap atlas (see scrap_atlas.h)
    // rather than owning VRAM: bind 'atlas' and shift the draw's texel
    // coordinates by atlasX/atlasY, which gs::ResolveBind2D works out. 'width' and
    // 'height' stay the image's own, so Draw_GetPicSize and every caller's
    // layout math are unaffected, and 'pixels' points into the atlas buffer -
    // it is not a standalone allocation and must not be freed (see Unload).
    const Texture * atlas;
    s16             atlasX;
    s16             atlasY;

    // Residency is a cache managed by GS/VRAM: binding a const Texture may
    // upload it (or evict others), so vramAddr mutates behind the const API.
    static constexpr auto kNotResident = vram::Address::Invalid;
    mutable vram::Address vramAddr; // GS VRAM word address; kNotResident when not uploaded.

    // True if the texture is uploaded to VRAM and ready to be used by the GS.
    bool IsVramResident() const { return vramAddr != kNotResident; }

    // For dynamic textures (cinematic frames/lightmaps/scrap atlas).
    // Called after rewriting 'pixels' so the next bind refreshes GS VRAM.
    void MarkPixelsDirty() const { dirtyPixels = true; }
};

// Mip levels, beyond level 0, a power-of-two image of 'width' x 'height' can carry: the
// three a WAL file holds, cut short where a level would drop below 8 texels on either
// side - the GS needs 8 for bilinear filtering (see the GS manual's MIPMAP section),
// so a 16x16 wall keeps one level and a 64x16 one keeps one too.
constexpr int kMaxMipLevels = 3;

constexpr int MipLevelsFor(const int width, const int height)
{
    const int minDim = (width < height) ? width : height;
    int levels = 0;
    while (levels < kMaxMipLevels && (minDim >> (levels + 1)) >= 8)
    {
        ++levels;
    }
    return levels;
}

// Mip levels 'pixels' carries after level 0. Only mipmapped walls have any, and those
// are powers of two, so the count follows from the size and needs no field of its own.
inline int MipLevels(const Texture & texture)
{
    return HasFlag(texture.flags, TexFlags::Mipmapped) ? MipLevelsFor(texture.width, texture.height) : 0;
}

// Bytes of an image and 'mipLevels' levels after it, packed one after another, each half
// the size of the one before in both dimensions. Every level of a mip chain is at least
// 8x8, so each starts 16-byte aligned, as the upload DMA needs.
constexpr int MipChainBytes(const int width, const int height, const int mipLevels, const int bytesPerTexel)
{
    int bytes = 0;
    for (int level = 0; level <= mipLevels; ++level)
    {
        bytes += (width >> level) * (height >> level) * bytesPerTexel;
    }
    return bytes;
}

// Mappings from the strongly typed enums above to the plain integer constants
// libdraw/GS registers expect. SDK constants stay out of the rest of the backend.
inline int GsComponents(TexComponents components)
{
    return (components == TexComponents::RGBA) ? TEXTURE_COMPONENTS_RGBA : TEXTURE_COMPONENTS_RGB;
}

inline int GsFunction(TexFunction function)
{
    return (function == TexFunction::Decal) ? TEXTURE_FUNCTION_DECAL : TEXTURE_FUNCTION_MODULATE;
}

inline int GsMagFilter(TexFilter filter)
{
    return (filter == TexFilter::Linear) ? LOD_MAG_LINEAR : LOD_MAG_NEAREST;
}

inline int GsMinFilter(TexFilter filter)
{
    return (filter == TexFilter::Linear) ? LOD_MIN_LINEAR : LOD_MIN_NEAREST;
}

inline int GsPsm(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::RGBA32   : return GS_PSM_32;
    case PixelFormat::RGB16    : return GS_PSM_16;
    case PixelFormat::Palette8 : return GS_PSM_8;
    case PixelFormat::Alpha8   : return GS_PSM_8;
    }
    return GS_PSM_32; // Unreachable; keeps GCC's -Wreturn-type happy.
}

// Bytes of EE RAM one texel occupies in each PixelFormat (Palette8 = 1).
inline int BytesPerTexel(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::RGBA32   : return 4;
    case PixelFormat::RGB16    : return 2;
    case PixelFormat::Palette8 : return 1;
    case PixelFormat::Alpha8   : return 1;
    }
    return 4; // Unreachable; keeps GCC's -Wreturn-type happy.
}

// Whether ps2_mip_filter decides the texture's filtering (see gs::MakeTex1): the walls and
// model skins, the images ref_gl's gl_texturemode covered that sample linear here. Sprites
// keep nearest, which keeps their cutouts free of fringes; the sky, the pics and the
// lightmap atlases - walls by type, but lighting sampled linear whatever the setting, as
// ref_gl's lightmaps were - keep what they were given.
inline bool FollowsFilterSetting(const Texture & texture)
{
    return (texture.type == ImageType::Wall || texture.type == ImageType::Skin) &&
           texture.format != PixelFormat::Alpha8;
}

// Bytes of EE RAM the texture's pixel buffer occupies: level 0 and any mip levels after it.
inline int PixelBytes(const Texture & texture)
{
    return MipChainBytes(texture.width, texture.height, MipLevels(texture), BytesPerTexel(texture.format));
}

// Pixel stride the texture occupies VRAM with (the TEX0 TBW and transfer DBW).
// 8-bit textures must use a multiple of 128 (TBW must be even for PSMT8/4);
// other formats use their width as-is.
inline int TextureStridePixels(const Texture & texture, int psm)
{
    if (psm == GS_PSM_8)
    {
        return (texture.width + 127) & ~127;
    }
    return texture.width;
}

// Inline replacement for draw_log2() from libdraw.
inline u8 Log2(u32 x)
{
    // plzcw counts the leading zeros of x minus one, so 30 - lzc is
    // the index of the highest set bit (floor of the base 2 log).
    u32 lzc;
    asm volatile ("plzcw %0, %1\n\t" : "=r" (lzc) : "r" (x));

    u32 res = 30 - lzc;
    res += (x > (1u << res)) ? 1u : 0u; // Round up for non-power-of-two x.

    return static_cast<u8>(res);
}

// Sets up the pool and the generated images (the debug checkerboards, the particle image).
// Call once.
void Init();

// A new texture over 'pixels', which stay the caller's and must outlive it. Streams into GS
// VRAM on its first bind. Pics and sprites filter nearest, the rest linear; the caller may
// change any field before the first bind. Running out of slots is a Sys_Error.
Texture & Create(const char * name, const void * pixels, int width, int height,
                 PixelFormat format, TexComponents components, ImageType type,
                 TexFlags flags = TexFlags::None);

// Gives the texture's VRAM back and frees its slot. Not its pixels: the caller owns those.
void Destroy(const Texture & texture);

// Capacity of the texture pool: world textures, model skins, HUD/menu pics.
// Running out is a Sys_Error telling you to bump this.
constexpr u32 kMaxTextures = 640;

// Number of built-in debug checkerboard variants (distinct colors).
constexpr int kNumDebugTextures = PS2_QUAKE_DEBUG ? 6 : 1;

// Checkerboard stand-ins. Variant 0 is the pink/black checker drawn wherever an image is
// missing; the others give test scenes several distinct textures to exercise VRAM streaming.
const Texture & DebugTexture(int variant = 0);

// The particle image, generated at Init. Carries its shape in alpha with every texel's
// colour at the modulate identity, so the particle's colour comes entirely from its vertices.
const Texture & ParticleTexture();

// Converts image-normalized texture coordinates - 0..1 spanning the image,
// which is what Quake's MD2 glcmds store - into the GS's normalized ST space.
// The two are not the same thing: normalized ST spans the TEX0 TW/TH extent,
// the image size rounded UP to a power of two, so for the non-power-of-two
// images Quake is full of (a 276x194 model skin samples as 512x256) ST = 1.0
// lands well past the last real texel. Multiply by these to hit the image's
// true right/bottom edge; both come back 1.0 for power-of-two images.
//
// Only valid for coordinates that stay within [0, 1]. A tiling texture still
// wraps at the power-of-two extent, so a coordinate scale cannot fix one; the
// world textures are resampled on load instead (see Texture::srcWidth), which
// is why they come back 1.0 here.
inline void StScaleFor(const Texture & texture, float * outScaleS, float * outScaleT)
{
    // tex::Log2 rounds up, and it is the same call gs.cpp fills TEX0's TW/TH
    // with - so this stays exact whatever the texture is, resident or not.
    const int potWidth  = 1 << Log2(static_cast<u32>(texture.width));
    const int potHeight = 1 << Log2(static_cast<u32>(texture.height));

    *outScaleS = static_cast<float>(texture.width)  / static_cast<float>(potWidth);
    *outScaleT = static_cast<float>(texture.height) / static_cast<float>(potHeight);
}

} // namespace ps2::tex
