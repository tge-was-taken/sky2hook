#version 450 core
// Game Im2D: the PS2 GS bits (PS2_UNIT, GsFrag uGs, ps2AtestDiscards) come from the includes below.
#include "gs_unit.glsl"
#include "gs_atest.glsl"
in vec2 vUV;
in vec4 vCol;
out vec4 frag;
uniform sampler2D uTex;
uniform float uTextured;
// GS CLAMP_1 region clamp as (min.xy, max.xy) in normalized UV. An empty rect means no clamp, so the
// all-zero value a never-uploaded uniform reads as is the inert case rather than a degenerate one.
uniform vec4 uUvClamp;
// GS TEX0.TCC=0: the texture function takes RGB from the texture and alpha from the vertex alone.
// Reads 0 when never uploaded, which is TCC=1, the behaviour every draw had before this existed.
uniform float uIgnoreTexAlpha;
// FeedbackBlurRemovalEnhancement::Rescale. A redirected tap lost its history, so rescale it to the
// steady state that history reached. 0 (never uploaded) is off, 1 gains by our own alpha, 2 by the
// destination's.
uniform int uFeedbackFixedPoint;
uniform sampler2D uDestTex; // this pass's own colour attachment
void main()
{
    if (uTextured > 0.5)
    {
        vec2 uv = vUV;
        if (uUvClamp.z > uUvClamp.x && uUvClamp.w > uUvClamp.y)
        {
            // Half a texel in from each edge: the GS clamps the texel index and then filters clamped
            // neighbours, so sampling on the boundary would blend in the texel outside the region.
            vec2 inset = 0.5 / vec2(textureSize(uTex, 0));
            uv = clamp(uv, uUvClamp.xy + inset, uUvClamp.zw - inset);
        }
        vec4 t = texture(uTex, uv);
        // TEXTURE alpha is PS2 (0x80 = opaque). Under TCC=0 it is not part of the texture function
        // at all, so the vertex alpha below stands alone.
        float ta = uIgnoreTexAlpha > 0.5 ? 1.0 : clamp(ps2AScale(t.a), 0.0, 1.0);
        frag = vec4(t.rgb * vCol.rgb, ta * vCol.a);
    }
    else
    {
        frag = vCol;
    }
    if (uFeedbackFixedPoint != 0)
    {
        // Each frame adds tap * coef * alpha to the last, so the effect settles per channel at
        // S/(1 - alpha*coef). The correction goes on RGB rather than alpha because the coefficient is
        // per channel and one blend alpha cannot express three factors.
        // Mode 2 reads the alpha GL itself will use as the blend factor, so the two agree whatever the
        // frame buffer's alpha scale turns out to be.
        float gain = uFeedbackFixedPoint == 2
                         ? texelFetch(uDestTex, ivec2(gl_FragCoord.xy), 0).a
                         : frag.a;
        frag.rgb /= max(vec3(1.0) - gain * vCol.rgb, vec3(1e-3));
    }
    frag.a = ps2AtestDiscards(frag.a);
}
