/* ================================================================================================
 * File: sprite.cpp
 * Brief: Sprite models: QuakeSpasm's r_sprite.c on the VU1 path. See sprite.h.
 *
 *  A sprite frame is one image and where it sits about the entity's origin; drawing it is a quad
 *  with the image on it, oriented by the sprite's type - facing the view, facing the camera but
 *  standing upright, or held at the entity's own angles. Sprites draw unlit and opaque, their
 *  transparent texels cut out by the alpha test, as QuakeSpasm draws them.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/sprite.h"
#include "ps2/renderer/view.h"
#include "ps2/renderer/texmgr.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/render_system.h"
#include "ps2/renderer/vu1.h"
#include "ps2/math/vec_mat.h"

#include <cmath>

namespace ps2::sprite {
namespace {

// The GS modulate identity at full alpha: the texels as they are.
constexpr u32 kModulateIdentity = vu1::PackColorRGBA(128, 128, 128, 0x80);

// How far an oriented sprite - a decal on a wall - is drawn towards the camera. QuakeSpasm pulls
// them forward in depth with a polygon offset, which the GS has no equivalent of.
constexpr float kDecalPull = 0.25f;

// The frame to draw: R_GetSpriteFrame. A group either animates over its intervals or, for an
// angled sprite, holds eight views, of which the one facing the camera draws.
const mspriteframe_t * GetSpriteFrame(const entity_t & e, const msprite_t & sprite)
{
    int frame = e.frame;
    if (frame >= sprite.numframes || frame < 0)
    {
        Con_DPrintf("R_DrawSprite: no such frame %d for '%s'\n", frame, e.model->name);
        frame = 0;
    }

    const mspriteframedesc_t & desc = sprite.frames[frame];
    if (desc.type == SPR_SINGLE)
    {
        return desc.frameptr;
    }

    const mspritegroup_t & group = *static_cast<const mspritegroup_t *>(static_cast<const void *>(desc.frameptr));

    if (desc.type == SPR_ANGLED)
    {
        // erysdren's angled sprites, backported to QuakeSpasm from FTEQW.
        vec3_t axis[3];
        vec3_t angles;
        VectorCopy(e.angles, angles);
        AngleVectors(angles, axis[0], axis[1], axis[2]);

        const float f   = DotProduct(vpn, axis[0]);
        const float r   = DotProduct(vright, axis[0]);
        const int   dir = static_cast<int>((std::atan2(r, f) + (1.125f * math::kPI)) * (4.0f / math::kPI));
        return group.frames[dir & 7];
    }

    // Load guaranteed every interval positive, so there is no divide by zero here.
    const float * const intervals = group.intervals;
    const int numframes    = group.numframes;
    const float fullInterval = intervals[numframes - 1];
    const float time         = static_cast<float>(cl.time) + e.syncbase;
    const float targetTime   = time - (static_cast<float>(static_cast<int>(time / fullInterval)) * fullInterval);

    int i = 0;
    for (; i < numframes - 1; ++i)
    {
        if (intervals[i] > targetTime)
        {
            break;
        }
    }
    return group.frames[i];
}

} // namespace

void DrawSpriteModel(rs::TriangleStream & stream, const entity_t & e)
{
    const msprite_t & sprite = *static_cast<const msprite_t *>(e.model->cache.data);
    const mspriteframe_t & frame = *GetSpriteFrame(e, sprite);

    const tex::Texture * const texture = tex::TextureFor(frame.gltexture);
    if (texture == nullptr)
    {
        return;
    }

    // The quad's axes, by the sprite's type.
    vec3_t vUp, vRight;
    const float * sUp;
    const float * sRight;
    switch (sprite.type)
    {
    case SPR_VP_PARALLEL_UPRIGHT: // faces the view plane, up is towards the heavens
        vUp[0] = 0.0f;
        vUp[1] = 0.0f;
        vUp[2] = 1.0f;
        CrossProduct(vpn, vUp, vRight);
        VectorNormalize(vRight);
        sUp    = vUp;
        sRight = vRight;
        break;

    case SPR_FACING_UPRIGHT: // faces the camera's origin, up is towards the heavens
    {
        vec3_t vForward;
        VectorSubtract(e.origin, r_origin, vForward);
        vForward[2] = 0.0f;
        VectorNormalize(vForward);
        vRight[0] = vForward[1];
        vRight[1] = -vForward[0];
        vRight[2] = 0.0f;
        vUp[0] = 0.0f;
        vUp[1] = 0.0f;
        vUp[2] = 1.0f;
        sUp    = vUp;
        sRight = vRight;
        break;
    }

    case SPR_VP_PARALLEL: // faces the view plane, up is towards the top of the screen
        sUp    = vup;
        sRight = vright;
        break;

    case SPR_ORIENTED: // pitch, yaw and roll are the entity's, whatever the camera does
    {
        vec3_t vForward, angles;
        VectorCopy(e.angles, angles);
        AngleVectors(angles, vForward, vRight, vUp);
        sUp    = vUp;
        sRight = vRight;
        break;
    }

    case SPR_VP_PARALLEL_ORIENTED: // faces the view plane, but keeps the entity's roll
    {
        const float angle = math::DegToRad(e.angles[ROLL]);
        const float sr = math::Sinf(angle);
        const float cr = math::Cosf(angle);
        for (int i = 0; i < 3; ++i)
        {
            vRight[i] = (vright[i] * cr) + (vup[i] * sr);
            vUp[i]    = (vright[i] * -sr) + (vup[i] * cr);
        }
        sUp    = vUp;
        sRight = vRight;
        break;
    }

    default:
        return;
    }

    const float scale = ENTSCALE_DECODE(e.scale);

    float origin[3] = { e.origin[0], e.origin[1], e.origin[2] };
    if (sprite.type == SPR_ORIENTED)
    {
        for (int i = 0; i < 3; ++i)
        {
            origin[i] -= kDecalPull * vpn[i];
        }
    }

    // The corners QuakeSpasm fans, from the bottom left, clockwise as seen: the image's own
    // coordinates spread over its padded power-of-two extent (see tex::StScaleFor).
    float sMax, tMax;
    tex::StScaleFor(*texture, &sMax, &tMax);

    struct Corner { float up, right, s, t; };
    const Corner corners[4] = {
        { frame.down, frame.left,  0.0f, tMax },
        { frame.up,   frame.left,  0.0f, 0.0f },
        { frame.up,   frame.right, sMax, 0.0f },
        { frame.down, frame.right, sMax, tMax },
    };

    vu1::DrawVertex quad[4];
    for (int i = 0; i < 4; ++i)
    {
        const float up    = corners[i].up * scale;
        const float right = corners[i].right * scale;

        quad[i].position   = { origin[0] + (up * sUp[0]) + (right * sRight[0]),
                               origin[1] + (up * sUp[1]) + (right * sRight[1]),
                               origin[2] + (up * sUp[2]) + (right * sRight[2]) };
        quad[i].lightmap_s = 0.0f;
        quad[i].rgba       = kModulateIdentity;
        quad[i].s          = corners[i].s;
        quad[i].t          = corners[i].t;
        quad[i].lightmap_t = 0.0f;
    }

    stream.SetTransform(view::ViewProjection());
    stream.SetDrawFlags(rs::DrawFlags::None);
    stream.SetTexture(*texture);

    vu1::DrawVertex * const dst = stream.ReserveVerts(6);
    vu1::CopyDrawVertex(dst[0], quad[0]);
    vu1::CopyDrawVertex(dst[1], quad[1]);
    vu1::CopyDrawVertex(dst[2], quad[2]);
    vu1::CopyDrawVertex(dst[3], quad[0]);
    vu1::CopyDrawVertex(dst[4], quad[2]);
    vu1::CopyDrawVertex(dst[5], quad[3]);
    stream.CommitVerts(dst + 6);
}

} // namespace ps2::sprite
