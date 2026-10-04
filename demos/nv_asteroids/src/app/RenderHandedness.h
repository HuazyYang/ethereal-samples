#pragma once

// Rendering handedness of the demo on Donut.
//
// deviation: Donut's camera builds the left-handed (D3D) basis, right = cross(up, dir), so that
// cross(right, up) = dir with +Z forward. The 2018 demo ran on an older donut whose camera built
// right = cross(dir, up) -- the same basis with the right vector negated -- and its content was
// authored and verified through that mirrored camera. The render pipeline here is left-handed
// throughout: every engine::IView carries Donut's camera matrix unchanged, so view space is the
// left-handed one, IView::IsMirrored() is false for the main view, and every pass that works in
// view space works in Donut's. The content's handedness is reconciled once, in the projection:
// MirrorProjectionX negates view-space X before the D3D projection. World-to-clip, and with it the
// triangle winding, is therefore the 2018 transform, so the 2018 rasterizer states and the
// reconstructed shaders are unchanged.
//
// The consequence for code reading the projection: P[0][0] is negative. Anything that uses the
// projection's focal terms as a size or an aspect ratio must take ProjectionScale(), not the raw
// diagonal (lens flare, star field, particles, the meshlet LOD metric). Code that only multiplies
// matrices (world-to-clip, clip-to-view, inverses) needs nothing. Third-party code that assumes a
// projection with positive focal terms gets the same world-to-clip transform with the reflection
// moved onto its world-to-view matrix instead; HBAO+ is the one such consumer (HbaoPlusPass).

#include <donut/core/math/math.h>

#include <cmath>

namespace demo
{
    // Donut's D3D projection with view-space X mirrored: the one place the 2018 content's handedness
    // enters the left-handed pipeline. Row-vector convention, so row 0 multiplies view-space X.
    inline donut::math::float4x4 MirrorProjectionX(donut::math::float4x4 projection)
    {
        projection[0] = -projection[0];
        return projection;
    }

    // Focal scale of a projection (clip units per view-space unit at unit depth), independent of its
    // orientation: what screen-size and aspect-ratio computations need.
    inline donut::math::float2 ProjectionScale(const donut::math::float4x4& projection)
    {
        return donut::math::float2(std::fabs(projection[0][0]), std::fabs(projection[1][1]));
    }
}
