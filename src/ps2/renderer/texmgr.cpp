/* ================================================================================================
 * File: texmgr.cpp
 * Brief: QuakeSpasm's texture manager seam (gl_texmgr.h). The engine loads every 3D texture through
 *        TexMgr_LoadImage - the world's and the brush models' from gl_model.c, model skins and
 *        sprite frames later - and keeps the gltexture_t it gets back. Behind each one is the PS2
 *        texture (ps2::tex) the renderer binds, which streams into GS VRAM on its first use.
 *
 *        BSP textures stay where gl_model.c put them, on the hunk: all four of id's mip levels,
 *        16-byte aligned (see texture_t's padding), which the GS upload DMAs in place. Only a
 *        texture that has to be resampled to a power of two is copied. The palette variant
 *        QuakeSpasm picks by the texture's flags becomes the CLUT the texture samples through
 *        (see tex::PixelFormat). The 2D pics don't come through here: draw.cpp makes those.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/texmgr.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/render_system.h"

#include <cstddef>
#include <cstring>
#include <tamtypes.h>

extern "C" {
// QuakeSpasm's switch for the fullbright passes, which decides the palette a texture with
// fullbright texels loads with. The refresh registers it; no header declares it.
extern cvar_t gl_fullbrights;
} // extern "C"

namespace {

using ps2::tex::Texture;

// ------------------------------------------------------------------------------------------------
// Records
// ------------------------------------------------------------------------------------------------

// A gltexture_t as the engine sees it, and the PS2 texture behind it. The engine only ever holds
// pointers to the gltexture_t, which comes first, so one converts to the other.
struct TextureRecord
{
    gltexture_t gl;

    // Null when there is nothing to draw: warp images (the PS2 warps on VU1), and the dummy
    // RGBA textures QuakeSpasm makes with no pixels at all.
    Texture * texture;

    // Pixels this record allocated - a resampled copy - and frees with the texture. Null when the
    // texture uses the caller's in place.
    void * ownedPixels;
    u32    ownedBytes;
};
static_assert(offsetof(TextureRecord, gl) == 0, "The engine's gltexture_t pointer is the record's");

Q_ALWAYS_INLINE TextureRecord & RecordOf(gltexture_t * gl)
{
    return *static_cast<TextureRecord *>(static_cast<void *>(gl));
}

Q_ALWAYS_INLINE const TextureRecord & RecordOf(const gltexture_t * gl)
{
    return *static_cast<const TextureRecord *>(static_cast<const void *>(gl));
}

// Every live texture, newest first, through gltexture_t's own 'next' link.
static gltexture_t * s_textures = nullptr;

gltexture_t * NewTexture()
{
    // TODO: Allocate these from a fixed-size pool instead.
    void * const memory = ps2::heap::Alloc(sizeof(TextureRecord), ps2::heap::MemTag::TexImage);
    std::memset(memory, 0, sizeof(TextureRecord));

    gltexture_t * const texture = static_cast<gltexture_t *>(memory);
    texture->next = s_textures;
    s_textures = texture;

    return texture;
}

// Destroys the PS2 texture behind a record and frees what it owns, leaving the record itself.
void ReleaseTexture(TextureRecord & record)
{
    if (record.texture != nullptr)
    {
        ps2::tex::Destroy(*record.texture);
        record.texture = nullptr;
    }
    if (record.ownedPixels != nullptr)
    {
        ps2::heap::Free(record.ownedPixels, record.ownedBytes, ps2::heap::MemTag::TexImage);
        record.ownedPixels = nullptr;
        record.ownedBytes  = 0;
    }
}

// Unlinks and frees every texture 'shouldFree' accepts.
//
// Waits out the frame the GS may still be drawing first: its chain can be uploading one of these
// textures, out of pixels that are about to go - the hunk a map change frees right after this.
template<typename Predicate>
void FreeTexturesWhere(Predicate shouldFree)
{
    bool waited = false;

    gltexture_t ** link = &s_textures;
    while (*link != nullptr)
    {
        gltexture_t * const texture = *link;
        if (shouldFree(*texture))
        {
            if (!waited)
            {
                ps2::rs::FinishFrameInFlight();
                waited = true;
            }

            *link = texture->next;
            ReleaseTexture(RecordOf(texture));
            ps2::heap::Free(texture, sizeof(TextureRecord), ps2::heap::MemTag::TexImage);
        }
        else
        {
            link = &texture->next;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Loading
// ------------------------------------------------------------------------------------------------

// Whether 'name' ends in 'suffix', ignoring case.
bool EndsWith(const char * name, const char * suffix)
{
    const size_t nameLen   = std::strlen(name);
    const size_t suffixLen = std::strlen(suffix);
    return (nameLen >= suffixLen) && (q_strcasecmp(name + nameLen - suffixLen, suffix) == 0);
}

// What a texture is for, from what owns it: QuakeSpasm keys its textures to a model. By the model's
// file name rather than its type, which the loaders only set once they are done - while a model's
// skins load, it still reads as a brush model.
ps2::tex::ImageType ImageTypeFor(const qmodel_t * owner)
{
    if (owner == nullptr)
    {
        return ps2::tex::ImageType::Pic;
    }
    if (owner->name[0] == '*' || EndsWith(owner->name, ".bsp"))
    {
        return ps2::tex::ImageType::Wall;
    }
    if (EndsWith(owner->name, ".mdl"))
    {
        return ps2::tex::ImageType::Skin;
    }
    if (EndsWith(owner->name, ".spr"))
    {
        return ps2::tex::ImageType::Sprite;
    }
    return ps2::tex::ImageType::Pic;
}

// The CLUT an 8-bit texture samples through: the palette variant QuakeSpasm's TexMgr_LoadImage8
// picks by the same flags. gl_fullbrights is read once, at load, so switching it takes effect on
// the next map; QuakeSpasm reloads every no-bright texture on the spot instead.
ps2::tex::PixelFormat PaletteFormatFor(const unsigned flags)
{
    if ((flags & TEXPREF_FULLBRIGHT) != 0)
    {
        return ps2::tex::PixelFormat::Palette8Fullbright;
    }

    if ((flags & TEXPREF_NOBRIGHT) != 0 && gl_fullbrights.value != 0.0f)
    {
        return ps2::tex::PixelFormat::Palette8NoBright;
    }
    return ps2::tex::PixelFormat::Palette8;
}

// QuakeSpasm's TexMgr_LoadImage8 hack, from tomazquake: this texture in b_shell1.bsp has some of
// its first 32 pixels painted white, invisible in the software renderer but ugly once filtered.
// So the first row takes the last one's pixels. In place, as QuakeSpasm does it.
void FixShot1Sid(const char * name, byte * data, const int width, const int height)
{
    if (std::strstr(name, "shot1sid") != nullptr && width == 32 && height == 32 &&
        CRC_Block(data, 1024) == 65393)
    {
        std::memcpy(data, data + (32 * 31), 32);
    }
}

// Whether an image has any index-255 texel, which is what TEXPREF_ALPHA cuts out: QuakeSpasm drops
// the flag from an image without one ("detect false alpha cases").
bool HasTransparentTexels(const byte * data, const int count)
{
    for (int i = 0; i < count; ++i)
    {
        if (data[i] == 255)
        {
            return true;
        }
    }
    return false;
}

// Stretches an image up to the next power of two in both dimensions, into a fresh allocation, for
// the textures that tile: the GS spreads normalized ST over the TEX0 extent, the image size rounded
// UP to a power of two, so a 48-texel-wide wall would wrap every 64 texels, over 16 columns of
// whatever else is in VRAM. Filling the extent puts the wrap back on the image's own edge, and the
// world's texture coordinates keep dividing by the size the image had on disk (Texture::srcWidth),
// so a tile still spans the world units it used to.
//
// Point sampling: an 8-bit image is palette indices, and the mean of two indices is an unrelated
// colour. 'mipLevels' levels follow level 0 in 'pixels' (see tex::MipChainBytes), each stretched to
// its own half of the one before, so the chain comes out as the power-of-two image's.
byte * ResampleToPowerOfTwo(const byte * pixels, const int srcW, const int srcH, const int dstW,
                            const int dstH, const int mipLevels, u32 * outBytes)
{
    const u32 bytes = static_cast<u32>(ps2::tex::MipChainBytes(dstW, dstH, mipLevels, 1));
    byte * const scaled = static_cast<byte *>(
        ps2::heap::AllocAligned(ps2::heap::MemAlign(16), bytes, ps2::heap::MemTag::TexImage));

    const byte * srcLevel = pixels;
    byte *       dstLevel = scaled;
    for (int level = 0; level <= mipLevels; ++level)
    {
        const int levelSrcW = srcW >> level;
        const int levelSrcH = srcH >> level;
        const int levelDstW = dstW >> level;
        const int levelDstH = dstH >> level;

        // 16.16 fixed-point steps through the source. The destination never shrinks, so both
        // steps are at most 1.0 and the accumulators stay in bounds.
        const u32 stepS = (static_cast<u32>(levelSrcW) << 16) / static_cast<u32>(levelDstW);
        const u32 stepT = (static_cast<u32>(levelSrcH) << 16) / static_cast<u32>(levelDstH);

        u32 accT = 0;
        for (int y = 0; y < levelDstH; ++y, accT += stepT)
        {
            const byte * const srcRow = srcLevel + (static_cast<int>(accT >> 16) * levelSrcW);
            byte * const       dstRow = dstLevel + (y * levelDstW);

            u32 accS = 0;
            for (int x = 0; x < levelDstW; ++x, accS += stepS)
            {
                dstRow[x] = srcRow[accS >> 16];
            }
        }

        srcLevel += levelSrcW * levelSrcH;
        dstLevel += levelDstW * levelDstH;
    }

    *outBytes = bytes;
    return scaled;
}

// Makes the PS2 texture for an 8-bit image QuakeSpasm asked for.
void CreateIndexedTexture(TextureRecord & record, const qmodel_t * owner, byte * data,
                          const int width, const int height, const unsigned flags)
{
    using namespace ps2::tex;

    const ImageType   type   = ImageTypeFor(owner);
    const PixelFormat format = PaletteFormatFor(flags);

    // Only BSP textures for now: gl_model.c keeps all four of their mip levels on the hunk, 16-byte
    // aligned, for as long as the map is loaded, which is what lets the texture use them in place.
    // Model skins and sprite frames arrive in file buffers that are gone once the model loads.
    if (type != ImageType::Wall)
    {
        return;
    }
    PS2_AssertMsg((reinterpret_cast<uintptr_t>(data) & 15u) == 0, "BSP texture pixels not 16-byte aligned!");

    FixShot1Sid(record.gl.name, data, width, height);

    // The fullbright pass cuts out everything but its own texels by their alpha, and a fence
    // texture its index 255; every other wall is opaque, and as RGB the GS ignores the palette's
    // transparent 255 for it, as QuakeSpasm's GL path does without an alpha test.
    const bool cutout = (format == PixelFormat::Palette8Fullbright) ||
                        ((flags & TEXPREF_ALPHA) != 0 && HasTransparentTexels(data, width * height));

    // Tiling, so a power of two; mip levels down to 8 texels, as many as the GS can filter.
    const int potWidth  = 1 << Log2(static_cast<u32>(width));
    const int potHeight = 1 << Log2(static_cast<u32>(height));
    const int mipLevels = MipLevelsFor(potWidth, potHeight);

    const void * pixels = data;
    if (potWidth != width || potHeight != height)
    {
        pixels = record.ownedPixels = ResampleToPowerOfTwo(data, width, height, potWidth, potHeight,
                                                           mipLevels, &record.ownedBytes);
    }

    Texture & texture = Create(record.gl.name, pixels, potWidth, potHeight, format,
                               cutout ? TexComponents::RGBA : TexComponents::RGB, type,
                               (mipLevels > 0) ? TexFlags::Mipmapped : TexFlags::None);
    texture.srcWidth  = static_cast<s16>(width);
    texture.srcHeight = static_cast<s16>(height);
    record.texture    = &texture;
}

} // namespace

namespace ps2::tex {

// gfx/palette.lmp as QuakeSpasm keeps it: one RGBA word per index, little-endian, with index 255
// transparent. gl_model.c reads it when it flood-fills a model skin's background.
void LoadPalette()
{
    const void * const file = COM_LoadTempFile("gfx/palette.lmp", nullptr);
    if (file == nullptr)
    {
        Sys_Error("Couldn't load gfx/palette.lmp");
    }

    const byte * const rgb = static_cast<const byte *>(file);
    for (int i = 0; i < 256; ++i)
    {
        const u32 r = rgb[(i * 3) + 0];
        const u32 g = rgb[(i * 3) + 1];
        const u32 b = rgb[(i * 3) + 2];
        const u32 a = (i == 255) ? 0u : 255u;
        d_8to24table[i] = r | (g << 8) | (b << 16) | (a << 24);
    }
}

const Texture * TextureFor(const gltexture_t * gl)
{
    return (gl != nullptr) ? RecordOf(gl).texture : nullptr;
}

} // namespace ps2::tex

extern "C" {

// ------------------------------------------------------------------------------------------------
// Engine-visible texture state
// ------------------------------------------------------------------------------------------------

gltexture_t * notexture   = nullptr;
gltexture_t * nulltexture = nullptr;

unsigned int d_8to24table[256];

// QuakeSpasm only generates mipmaps for its warp images when the GL driver can; the model loader
// asks through this pointer, and on the PS2 there is no driver to ask.
QS_PFNGENERATEMIPMAP GL_GenerateMipmap = nullptr;

// ------------------------------------------------------------------------------------------------
// TexMgr_*
// ------------------------------------------------------------------------------------------------

// The palette is already in: VID_Init loaded it for the GS.
void TexMgr_Init()
{
    notexture   = TexMgr_LoadImage(nullptr, "notexture",   2, 2, SRC_RGBA, nullptr, "", 0, TEXPREF_PERSIST);
    nulltexture = TexMgr_LoadImage(nullptr, "nulltexture", 2, 2, SRC_RGBA, nullptr, "", 0, TEXPREF_PERSIST);
}

gltexture_t * TexMgr_LoadImage(qmodel_t * owner, const char * name, int width, int height,
                               enum srcformat format, byte * data, const char * source_file,
                               src_offset_t source_offset, unsigned flags)
{
    // TEXPREF_OVERWRITE reloads a texture in place (a player skin taking new colours), so the
    // engine's pointer to it stays good. Anything else is a new texture.
    gltexture_t * texture = nullptr;
    if ((flags & TEXPREF_OVERWRITE) != 0)
    {
        for (gltexture_t * t = s_textures; t != nullptr; t = t->next)
        {
            if (t->owner == owner && std::strcmp(t->name, name) == 0)
            {
                texture = t;
                ps2::rs::FinishFrameInFlight();
                ReleaseTexture(RecordOf(texture));
                break;
            }
        }
    }
    if (texture == nullptr)
    {
        texture = NewTexture();
    }

    texture->owner = owner;
    q_strlcpy(texture->name, name, sizeof(texture->name));
    texture->width         = static_cast<unsigned int>(width);
    texture->height        = static_cast<unsigned int>(height);
    texture->flags         = flags;
    q_strlcpy(texture->source_file, source_file, sizeof(texture->source_file));
    texture->source_offset = source_offset;
    texture->source_format = format;
    texture->source_width  = static_cast<unsigned int>(width);
    texture->source_height = static_cast<unsigned int>(height);
    texture->shirt         = -1;
    texture->pants         = -1;

    // Warp images are QuakeSpasm's render targets for its water; the PS2 warps on VU1 instead.
    // Anything else without pixels (the dummy RGBA textures) is a name and nothing to draw.
    if (format == SRC_INDEXED && data != nullptr && width > 0 && height > 0 &&
        (flags & TEXPREF_WARPIMAGE) == 0)
    {
        CreateIndexedTexture(RecordOf(texture), owner, data, width, height, flags);
    }
    return texture;
}

void TexMgr_FreeTexturesForOwner(qmodel_t * owner)
{
    FreeTexturesWhere([owner](const gltexture_t & t) { return t.owner == owner; });
}

// A new game (the 'game' command) drops every texture not marked to persist. QuakeSpasm reloads
// the palette here too, for a mod that ships its own; the GS CLUTs are fixed at init, so this
// keeps the one it has. ('game' needs the registered version, so on shareware it never runs.)
void TexMgr_NewGame()
{
    FreeTexturesWhere([](const gltexture_t & t) { return (t.flags & TEXPREF_PERSIST) == 0; });
}

// QuakeSpasm pads a non-power-of-two texture for drivers that need it, which none here does.
int TexMgr_PadConditional(int s)
{
    return s;
}

} // extern "C"
