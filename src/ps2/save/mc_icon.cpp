/* ================================================================================================
 * File: mc_icon.cpp
 * Brief: The save directory's icon.sys and 3D icon. See mc_icon.h.
 *
 *  icon.sys is libmc's mcIcon: the title (Shift-JIS, two lines split at nlOffset), the four
 *  background corner colours and the lighting the browser shows the icon with, and the icon
 *  files to use for listing, copying and deleting - all the same one here.
 *
 *  The icon is the quad damage pickup (progs/quaddama.mdl), as on the Quake II port: its first
 *  frame with its first skin, read straight from the MDL in the game data and turned into icon
 *  space. Should the game data not have it, the icon is a plain square instead, so a save
 *  directory always has one.
 *
 *  It is built from the game data whenever a save goes to a card, and rewritten if the card's
 *  copy differs - so a change of icon reaches cards that already have a save directory. The
 *  format isn't documented in the SDK; the layout below follows the community documentation
 *  of it (the one bmp2icon-style tools write):
 *
 *      header      magic 0x00010000, shape count, texture type, 1.0f, vertex count (x3)
 *      vertices    per vertex: a position per shape, a normal, UV, RGBA - s16 values in
 *                  4.12 fixed point (4096 = 1.0), colours with 0x80 = 1.0
 *      animation   header + frames + keys; a still icon has one frame of one key
 *      texture     128x128 texels, 16 bits each (GS PSMCT16: R5 G5 B5 A1), uncompressed
 *
 *  The browser's space has Y pointing down, with the icon standing on Y = 0. It turns the
 *  icon about the Y axis by itself, and draws its texture opaque, ignoring the alpha bit.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/mc_icon.h"
#include "ps2/math/vec_mat.h"

#include <cstdlib>
#include <cstring>
#include <libmc.h>
#include <sjis.h>

namespace ps2::save {
namespace {

using ps2::heap::MemTag;
using ps2::math::Vec3;

constexpr const char * kTitleLine1 = "Quake";
constexpr const char * kTitleLine2 = "Saved Games";
constexpr const char * kIconModel  = "progs/quaddama.mdl";

// Browser-space size of the longest side of the model's bounding box.
constexpr float kIconSize = 3.2f;

// Each model triangle goes in twice, once per winding, with the same normals. Whether the
// browser culls back faces, and which winding it takes for the front, isn't known: this way
// the outside of the model shows either way. Without culling, the two copies of a triangle
// draw identically, so there is nothing for them to fight over.
constexpr bool kDoubleSidedModel = true;

constexpr int kTextureSize = 128;

// Bounds a well-formed MDL stays within (QuakeSpasm's MAXALIASVERTS and MAXALIASTRIS, and the
// skin size its loader takes): anything past them is not a model to trust the offsets of.
constexpr int kMaxModelVerts = 2000;
constexpr int kMaxModelTris  = 4096;
constexpr int kMaxSkinSide   = 1024;

struct IconHeader
{
    u32 magic;
    u32 numShapes;
    u32 textureType;
    u32 reserved;
    u32 numVertices;
};

struct IconVertex // One animation shape.
{
    s16 x, y, z, w;
    s16 nx, ny, nz, nw;
    s16 u, v;
    u8  r, g, b, a;
};

struct IconAnimHeader
{
    u32   tag;
    u32   frameLength;
    float speed;
    u32   playOffset;
    u32   numFrames;
};

struct IconFrame
{
    u32 shapeId;
    u32 numKeys;
    u32 unknown1;
    u32 unknown2;
};

struct IconKey
{
    float time;
    float value;
};

static_assert(sizeof(IconHeader)     == 20);
static_assert(sizeof(IconVertex)     == 24);
static_assert(sizeof(IconAnimHeader) == 20);
static_assert(sizeof(IconFrame)      == 16);
static_assert(sizeof(IconKey)        == 8);

constexpr u32 kIconMagic    = 0x00010000u;
constexpr u32 kTextureRaw16 = 0x07u; // Textured, uncompressed.
constexpr u32 kFloatOne     = 0x3F800000u;
constexpr u32 kTextureBytes = kTextureSize * kTextureSize * 2u;

constexpr u32 kAnimationBytes = sizeof(IconAnimHeader) + sizeof(IconFrame) + sizeof(IconKey);

// An icon file being built: the whole file on the heap, and where its vertices and texture go.
struct IconFile
{
    u8 *  data;
    u32   sizeBytes;
    u8 *  vertices;
    u16 * texels;
};

inline u16 PackTexel(const int r, const int g, const int b)
{
    return static_cast<u16>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | 0x8000);
}

inline s16 Fixed(const float value)
{
    return static_cast<s16>(value * 4096.0f);
}

void SetVertex(IconVertex & vertex,
               const float x,  const float y,  const float z,
               const float nx, const float ny, const float nz,
               const float u,  const float v)
{
    vertex = IconVertex{};
    vertex.x  = Fixed(x);
    vertex.y  = Fixed(y);
    vertex.z  = Fixed(z);
    vertex.nx = Fixed(nx);
    vertex.ny = Fixed(ny);
    vertex.nz = Fixed(nz);
    vertex.u  = Fixed(u);
    vertex.v  = Fixed(v);
    vertex.r  = 0x80;
    vertex.g  = 0x80;
    vertex.b  = 0x80;
    vertex.a  = 0x80;
}

// Allocates an icon file for this many vertices and fills in everything but the vertices and
// the texture: the header, and a still animation. False if out of memory.
bool NewIconFile(const u32 numVertices, IconFile & outFile)
{
    const u32 verticesOffset = sizeof(IconHeader);
    const u32 textureOffset  = verticesOffset + numVertices * sizeof(IconVertex) + kAnimationBytes;
    const u32 sizeBytes      = textureOffset + kTextureBytes;

    u8 * const data = static_cast<u8 *>(ps2::heap::TryAlloc(sizeBytes, MemTag::SaveData));
    if (data == nullptr)
    {
        return false;
    }

    IconHeader header = {};
    header.magic       = kIconMagic;
    header.numShapes   = 1;
    header.textureType = kTextureRaw16;
    header.reserved    = kFloatOne;
    header.numVertices = numVertices;

    IconAnimHeader anim = {};
    anim.tag         = 1;
    anim.frameLength = 1;
    anim.speed       = 1.0f;
    anim.playOffset  = 0;
    anim.numFrames   = 1;

    IconFrame frame = {};
    frame.shapeId  = 0;
    frame.numKeys  = 1;
    frame.unknown1 = 1;
    frame.unknown2 = 0;

    const IconKey key = { 1.0f, 1.0f };

    u8 * cursor = data + textureOffset - kAnimationBytes;
    std::memcpy(data, &header, sizeof(header));
    std::memcpy(cursor, &anim, sizeof(anim));
    cursor += sizeof(anim);
    std::memcpy(cursor, &frame, sizeof(frame));
    cursor += sizeof(frame);
    std::memcpy(cursor, &key, sizeof(key));

    outFile.data      = data;
    outFile.sizeBytes = sizeBytes;
    outFile.vertices  = data + verticesOffset;

    // `data` comes from the heap and every block before the texture is a whole number of
    // u16s, so the texture is aligned for them.
    outFile.texels = static_cast<u16 *>(static_cast<void *>(data + textureOffset));
    return true;
}

inline void PutVertex(IconFile & file, u32 & index, const IconVertex & vertex)
{
    std::memcpy(file.vertices + index * sizeof(IconVertex), &vertex, sizeof(vertex));
    ++index;
}

void FillPlainTexture(u16 * outTexels)
{
    for (int i = 0; i < kTextureSize * kTextureSize; ++i)
    {
        outTexels[i] = PackTexel(72, 56, 40); // A dark bronze.
    }
}

// ------------------------------------------------------------------------------------------------
// The model: the parts of an MDL the icon needs, found with every offset checked
// ------------------------------------------------------------------------------------------------

// Reads a little-endian int at a byte offset, which an MDL doesn't promise is aligned.
inline int ReadInt(const u8 * const bytes, const size_t offset)
{
    int value;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

struct ModelView
{
    mdl_t               header;
    const u8 *          skin;      // header.skinwidth * header.skinheight palette indexes
    const stvert_t *    stverts;   // header.numverts
    const dtriangle_t * triangles; // header.numtris
    const trivertx_t *  verts;     // The first frame's, header.numverts
};

// Walks the MDL's blocks as Mod_LoadAliasModel does, taking the first skin and the first frame
// (the first of a group's, for a group). False if anything runs past the end of the file.
bool ParseModel(const u8 * const bytes, const size_t fileBytes, ModelView & out)
{
    if (fileBytes < sizeof(mdl_t))
    {
        return false;
    }
    std::memcpy(&out.header, bytes, sizeof(mdl_t));
    const mdl_t & header = out.header;

    if (header.ident != IDPOLYHEADER || header.version != ALIAS_VERSION || header.numskins < 1 ||
        header.skinwidth < 1 || header.skinwidth > kMaxSkinSide || header.skinheight < 1 ||
        header.skinheight > kMaxSkinSide || header.numverts < 1 || header.numverts > kMaxModelVerts ||
        header.numtris < 1 || header.numtris > kMaxModelTris || header.numframes < 1)
    {
        return false;
    }

    const size_t skinBytes = static_cast<size_t>(header.skinwidth) * static_cast<size_t>(header.skinheight);
    size_t offset = sizeof(mdl_t);
    const auto fits = [&](const size_t bytesNeeded) { return offset + bytesNeeded <= fileBytes; };

    out.skin = nullptr;
    for (int i = 0; i < header.numskins; ++i)
    {
        if (!fits(sizeof(int)))
        {
            return false;
        }
        const int type = ReadInt(bytes, offset);
        offset += sizeof(int);

        int numInGroup = 1;
        if (type == ALIAS_SKIN_GROUP)
        {
            if (!fits(sizeof(int)))
            {
                return false;
            }
            numInGroup = ReadInt(bytes, offset);
            offset += sizeof(int);
            if (numInGroup < 1 || !fits(static_cast<size_t>(numInGroup) * sizeof(float)))
            {
                return false;
            }
            offset += static_cast<size_t>(numInGroup) * sizeof(float); // the intervals
        }

        if (!fits(static_cast<size_t>(numInGroup) * skinBytes))
        {
            return false;
        }
        if (out.skin == nullptr)
        {
            out.skin = bytes + offset;
        }
        offset += static_cast<size_t>(numInGroup) * skinBytes;
    }

    const size_t stvertBytes   = static_cast<size_t>(header.numverts) * sizeof(stvert_t);
    const size_t triangleBytes = static_cast<size_t>(header.numtris) * sizeof(dtriangle_t);
    if (!fits(stvertBytes + triangleBytes + sizeof(int)))
    {
        return false;
    }
    out.stverts   = static_cast<const stvert_t *>(static_cast<const void *>(bytes + offset));
    out.triangles = static_cast<const dtriangle_t *>(static_cast<const void *>(bytes + offset + stvertBytes));
    offset += stvertBytes + triangleBytes;

    const int frameType = ReadInt(bytes, offset);
    offset += sizeof(int);
    if (frameType == ALIAS_GROUP)
    {
        if (!fits(sizeof(daliasgroup_t)))
        {
            return false;
        }
        const int numInGroup = ReadInt(bytes, offset);
        offset += sizeof(daliasgroup_t);
        if (numInGroup < 1 || !fits(static_cast<size_t>(numInGroup) * sizeof(float)))
        {
            return false;
        }
        offset += static_cast<size_t>(numInGroup) * sizeof(float); // the intervals
    }

    const size_t vertBytes = static_cast<size_t>(header.numverts) * sizeof(trivertx_t);
    if (!fits(sizeof(daliasframe_t) + vertBytes))
    {
        return false;
    }
    out.verts = static_cast<const trivertx_t *>(static_cast<const void *>(bytes + offset + sizeof(daliasframe_t)));

    // The blocks are read in place as structs of ints: they must sit on word boundaries, as they
    // do in every MDL of the game (a skin is a whole number of words, its width a multiple of 4).
    const uiptr mask = alignof(int) - 1u;
    return ((reinterpret_cast<uiptr>(out.stverts) | reinterpret_cast<uiptr>(out.triangles)) & mask) == 0;
}

// A frame vertex, decoded to model space - Quake's: X forward, Y left, Z up.
Vec3 FramePosition(const ModelView & model, const int index)
{
    const trivertx_t & vertex = model.verts[index];
    return Vec3{ static_cast<float>(vertex.v[0]) * model.header.scale[0] + model.header.scale_origin[0],
                 static_cast<float>(vertex.v[1]) * model.header.scale[1] + model.header.scale_origin[1],
                 static_cast<float>(vertex.v[2]) * model.header.scale[2] + model.header.scale_origin[2] };
}

// The skin's palette indexes, stretched over the texture through the game palette, bilinear.
void TextureFromSkin(const ModelView & model, u16 * outTexels)
{
    const int width  = model.header.skinwidth;
    const int height = model.header.skinheight;

    const auto channel = [&](const int x, const int y, const int shift) {
        const u32 rgba = d_8to24table[model.skin[y * width + x]];
        return static_cast<float>((rgba >> shift) & 0xFFu);
    };

    for (int ty = 0; ty < kTextureSize; ++ty)
    {
        const float sy = (static_cast<float>(ty) + 0.5f) * static_cast<float>(height) / kTextureSize - 0.5f;
        const int   y0 = (sy < 0.0f) ? 0 : static_cast<int>(sy);
        const int   y1 = (y0 + 1 < height) ? y0 + 1 : y0;
        const float fy = (sy < 0.0f) ? 0.0f : sy - static_cast<float>(y0);

        for (int tx = 0; tx < kTextureSize; ++tx)
        {
            const float sx = (static_cast<float>(tx) + 0.5f) * static_cast<float>(width) / kTextureSize - 0.5f;
            const int   x0 = (sx < 0.0f) ? 0 : static_cast<int>(sx);
            const int   x1 = (x0 + 1 < width) ? x0 + 1 : x0;
            const float fx = (sx < 0.0f) ? 0.0f : sx - static_cast<float>(x0);

            int rgb[3];
            for (int c = 0; c < 3; ++c)
            {
                const int shift = c * 8; // d_8to24table holds R in the low byte
                const float top    = channel(x0, y0, shift) + (channel(x1, y0, shift) - channel(x0, y0, shift)) * fx;
                const float bottom = channel(x0, y1, shift) + (channel(x1, y1, shift) - channel(x0, y1, shift)) * fx;
                rgb[c] = static_cast<int>(top + (bottom - top) * fy);
            }
            outTexels[ty * kTextureSize + tx] = PackTexel(rgb[0], rgb[1], rgb[2]);
        }
    }
}

// Turns the model's triangles into the icon's vertices: the bounding box's longest side
// kIconSize, centred, standing on the floor. Model space maps to icon space as X <- Y,
// Y <- -Z, Z <- -X: up is the icon's -Y, and the model's front faces -Z.
void AddModelTriangles(IconFile & file, const ModelView & model)
{
    const mdl_t & header = model.header;

    Vec3 mins = FramePosition(model, 0);
    Vec3 maxs = mins;
    for (int i = 1; i < header.numverts; ++i)
    {
        const Vec3 p = FramePosition(model, i);
        mins.x = (p.x < mins.x) ? p.x : mins.x;
        mins.y = (p.y < mins.y) ? p.y : mins.y;
        mins.z = (p.z < mins.z) ? p.z : mins.z;
        maxs.x = (p.x > maxs.x) ? p.x : maxs.x;
        maxs.y = (p.y > maxs.y) ? p.y : maxs.y;
        maxs.z = (p.z > maxs.z) ? p.z : maxs.z;
    }

    const float sizeX   = maxs.x - mins.x;
    const float sizeY   = maxs.y - mins.y;
    const float sizeZ   = maxs.z - mins.z;
    const float longest = (sizeX > sizeY) ? ((sizeX > sizeZ) ? sizeX : sizeZ) : ((sizeY > sizeZ) ? sizeY : sizeZ);
    const float unit    = (longest > 0.0f) ? (kIconSize / longest) : 1.0f;
    const float centreX = (mins.x + maxs.x) * 0.5f;
    const float centreY = (mins.y + maxs.y) * 0.5f;

    const float invSkinWidth  = 1.0f / static_cast<float>(header.skinwidth);
    const float invSkinHeight = 1.0f / static_cast<float>(header.skinheight);

    const auto makeVertex = [&](const int index, const bool facesFront) -> IconVertex {
        const Vec3 p = FramePosition(model, index);

        int normalIndex = model.verts[index].lightnormalindex;
        normalIndex = (normalIndex < NUMVERTEXNORMALS) ? normalIndex : 0;
        const float * const n = r_avertexnormals[normalIndex];

        // A vertex on the skin's seam is shared by the front and back halves of the skin: the
        // back-facing triangles take it from the back half, half the skin's width across (as
        // GL_MakeAliasModelDisplayLists does).
        const stvert_t & st = model.stverts[index];
        const int s = (!facesFront && st.onseam != 0) ? st.s + header.skinwidth / 2 : st.s;

        IconVertex vertex;
        SetVertex(vertex,
                  (p.y - centreY) * unit, -(p.z - mins.z) * unit, -(p.x - centreX) * unit,
                  n[1], -n[2], -n[0],
                  (static_cast<float>(s) + 0.5f) * invSkinWidth, (static_cast<float>(st.t) + 0.5f) * invSkinHeight);
        return vertex;
    };

    u32 vertexIndex = 0;
    for (int tri = 0; tri < header.numtris; ++tri)
    {
        const dtriangle_t & triangle = model.triangles[tri];
        const bool facesFront = (triangle.facesfront != 0);

        IconVertex corners[3];
        for (int c = 0; c < 3; ++c)
        {
            const int index = triangle.vertindex[c];
            corners[c] = makeVertex((index >= 0 && index < header.numverts) ? index : 0, facesFront);
        }

        PutVertex(file, vertexIndex, corners[0]);
        PutVertex(file, vertexIndex, corners[1]);
        PutVertex(file, vertexIndex, corners[2]);

        if (kDoubleSidedModel)
        {
            PutVertex(file, vertexIndex, corners[0]);
            PutVertex(file, vertexIndex, corners[2]);
            PutVertex(file, vertexIndex, corners[1]);
        }
    }
}

// The model icon. False if the model or memory isn't there, with nothing allocated.
bool BuildModelIcon(IconFile & outFile)
{
    u8 * const file = COM_LoadMallocFile(kIconModel, nullptr);
    if (file == nullptr)
    {
        Con_Printf("Save icon: no %s in the game data, the icon will be plain.\n", kIconModel);
        return false;
    }
    const size_t fileBytes = static_cast<size_t>(com_filesize);

    ModelView model = {};
    bool built = false;
    if (!ParseModel(file, fileBytes, model))
    {
        Con_Printf("Save icon: %s doesn't read as a model, the icon will be plain.\n", kIconModel);
    }
    else
    {
        const u32 verticesPerTri = kDoubleSidedModel ? 6u : 3u;
        if (NewIconFile(static_cast<u32>(model.header.numtris) * verticesPerTri, outFile))
        {
            AddModelTriangles(outFile, model);
            TextureFromSkin(model, outFile.texels);
            built = true;
        }
    }

    std::free(file);
    return built;
}

// What the icon is without the model: a plain square, kIconSize across, so the save directory
// still has an icon. Both windings, like the model. False if out of memory.
bool BuildPlainIcon(IconFile & outFile)
{
    constexpr int kNumCorners = 6; // Two triangles.
    if (!NewIconFile(kDoubleSidedModel ? kNumCorners * 2 : kNumCorners, outFile))
    {
        return false;
    }

    constexpr float kHalf = kIconSize * 0.5f;
    IconVertex corners[kNumCorners];
    SetVertex(corners[0], -kHalf, -kIconSize, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f);
    SetVertex(corners[1],  kHalf, -kIconSize, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f);
    SetVertex(corners[2], -kHalf,  0.0f,      0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f);
    SetVertex(corners[3],  kHalf, -kIconSize, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f);
    SetVertex(corners[4],  kHalf,  0.0f,      0.0f, 0.0f, 0.0f, -1.0f, 1.0f, 1.0f);
    SetVertex(corners[5], -kHalf,  0.0f,      0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f);

    u32 vertexIndex = 0;
    for (int tri = 0; tri < kNumCorners / 3; ++tri)
    {
        const IconVertex & a = corners[tri * 3 + 0];
        const IconVertex & b = corners[tri * 3 + 1];
        const IconVertex & c = corners[tri * 3 + 2];

        PutVertex(outFile, vertexIndex, a);
        PutVertex(outFile, vertexIndex, b);
        PutVertex(outFile, vertexIndex, c);

        if (kDoubleSidedModel)
        {
            PutVertex(outFile, vertexIndex, a);
            PutVertex(outFile, vertexIndex, c);
            PutVertex(outFile, vertexIndex, b);
        }
    }

    FillPlainTexture(outFile.texels);
    return true;
}

void BuildIconSys(mcIcon & sys)
{
    const iconIVECTOR kBackground[4] = {
        { 96, 36, 20, 0 }, // top left
        { 96, 36, 20, 0 }, // top right
        { 20,  8,  4, 0 }, // bottom left
        { 20,  8,  4, 0 }, // bottom right
    };
    const iconFVECTOR kLightDir[3] = {
        {  0.5f,  0.5f, 0.5f,  0.0f },
        {  0.0f, -0.4f, -0.1f, 0.0f },
        { -0.5f, -0.5f, 0.5f,  0.0f },
    };
    const iconFVECTOR kLightColour[3] = {
        { 0.3f, 0.3f, 0.3f, 0.0f },
        { 0.4f, 0.4f, 0.4f, 0.0f },
        { 0.5f, 0.5f, 0.5f, 0.0f },
    };
    const iconFVECTOR kAmbient = { 0.5f, 0.5f, 0.5f, 0.0f };

    std::memset(&sys, 0, sizeof(sys));
    std::memcpy(sys.head, "PS2D", 4);
    sys.type  = MCICON_TYPE_SAVED_DATA;
    sys.trans = 0x60;

    // Both lines as one string; nlOffset is where the second starts, in bytes of Shift-JIS
    // (every character converts to two). strcpy_sjis would turn a '\n' into junk.
    char title[40];
    std::snprintf(title, sizeof(title), "%s%s", kTitleLine1, kTitleLine2);
    strcpy_sjis(reinterpret_cast<short *>(sys.title), title);
    sys.nlOffset = static_cast<unsigned short>(std::strlen(kTitleLine1) * 2u);

    std::memcpy(sys.bgCol, kBackground, sizeof(kBackground));
    std::memcpy(sys.lightDir, kLightDir, sizeof(kLightDir));
    std::memcpy(sys.lightCol, kLightColour, sizeof(kLightColour));
    std::memcpy(sys.lightAmbient, kAmbient, sizeof(kAmbient));

    std::snprintf(reinterpret_cast<char *>(sys.view), sizeof(sys.view), "%s", kIconModelFile);
    std::snprintf(reinterpret_cast<char *>(sys.copy), sizeof(sys.copy), "%s", kIconModelFile);
    std::snprintf(reinterpret_cast<char *>(sys.del),  sizeof(sys.del),  "%s", kIconModelFile);
}

} // namespace

bool EnsureSaveIcons(Device & device)
{
    IconFile icon = {};
    if (!BuildModelIcon(icon) && !BuildPlainIcon(icon))
    {
        SetError("Not enough memory to create the save icon.");
        return false;
    }

    mcIcon sys;
    BuildIconSys(sys);

    const bool upToDate = FileMatches(device, kIconModelFile, icon.data, icon.sizeBytes) &&
                          FileMatches(device, kIconSysFile, &sys, sizeof(sys));

    const bool ok = upToDate ||
                    (WriteWholeFile(device, kIconModelFile, icon.data, icon.sizeBytes) &&
                     WriteWholeFile(device, kIconSysFile, &sys, sizeof(sys)));

    ps2::heap::Free(icon.data, icon.sizeBytes, MemTag::SaveData);

    if (!ok)
    {
        SetError("Could not write the save icon to the memory card.");
    }
    else if (!upToDate)
    {
        Con_Printf("Save icon: written to %s.\n", device.Describe(kIconModelFile));
    }
    return ok;
}

} // namespace ps2::save
