#version 450 core
#include "gs_unit.glsl"
#include "gs_atest.glsl"
layout(location = 0) in vec3 vWorldN;
layout(location = 1) in vec3 vWorldPos;
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec2 vUV2;
layout(location = 4) in vec4 vColor;
layout(location = 5) in vec3 vLit; // clamped per-vertex in 3d.vert
layout(location = 6) in float vFogZ;
out vec4 frag;
uniform vec4 uColor;
uniform vec3 uAmbient;
uniform vec3 uDirColor[MAX_DIR_LIGHTS];
uniform vec3 uDirDir[MAX_DIR_LIGHTS];
uniform int uDirCount;
uniform vec3 uLocalColor[MAX_LOCAL_LIGHTS];
uniform vec3 uLocalPos[MAX_LOCAL_LIGHTS];
uniform vec3 uLocalAxis[MAX_LOCAL_LIGHTS];
uniform vec4 uLocalParams[MAX_LOCAL_LIGHTS]; // (radius, minusCosAngle, type 0=pt/1=spot/2=soft, -)
uniform int uLocalCount;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
uniform sampler2D uTex;
uniform sampler2D uDateTex;
uniform int uDateMode; // GS TEST.DATE/DATM: 0 off, 1 = DATM 0, 2 = DATM 1
uniform int uDebugChannel; // 0 = normal render; 1..10 isolate a shading component (number keys)
uniform float uSurfAmbient;
uniform float uSurfDiffuse;
#if ENVMAP
uniform float uEnvCoef; // RpMatFX envmap coef: how much of the reflection reaches the frame buffer
#endif

vec3 applyFog(vec3 c)
{
#if FOG
    return mix(uFogColor, c,
               clamp((uFogEnd - vFogZ) / max(uFogEnd - uFogStart, 1e-3), 0.0, 1.0));
#else
    return c;
#endif
}

float spotCone(vec4 p, vec3 L, vec3 axis)
{
    if (p.z < 0.5) return 1.0;                       // point light: no cone
    float edge = -p.y;                               // -minusCosAngle
    float c    = dot(-L, axis);
    return (p.z > 1.5) ? clamp((c - edge) / max(1.0 - edge, 1e-4), 0.0, 1.0)
                       : step(edge, c);
}

bool ps2DateFails()
{
    if (uDateMode == 0) return false;
    bool set = texelFetch(uDateTex, ivec2(gl_FragCoord.xy), 0).a >= 0.5;
    return set != (uDateMode == 2);
}

void main()
{
#if UNLIT
    // uColor is the material colour (e.g. an RpPTank particle material): raw RGB multiplies straight
    // in, like the lit path's mc, but alpha is PS2-doubled and must join texture alpha inside the one
    // ps2AScale call, not be applied on its own (see the lit path's testA for why that matters).
    vec3 c = uColor.rgb;
    float a = clamp(ps2AScale(uColor.a), 0.0, 1.0);
#if TEXTURED
    { vec4 t = texture(uTex, vUV); c *= t.rgb; a = clamp(ps2AScale(t.a * uColor.a), 0.0, 1.0); }
#endif
    frag = vec4(applyFog(c * vColor.rgb), ps2AlphaPremul(a * vColor.a));
    return;
#else
#if MODULATE
    vec3 mc = uColor.rgb;
    float matA = uColor.a; // raw: the GS unit scale is applied once, to the combined alpha below
#else
    vec3 mc = vec3(1.0);
    float matA = 1.0;
#endif
    vec3 dbgTex = vec3(1.0), dbgAmbient = vec3(0.0), dbgDiffuse = vec3(0.0), dbgPrelit = vec3(0.0);
#if TEXTURED
#if ENVMAP
    // The GS picks the reflection's mip from depth alone, LOD = log2(1/Q) * 2^L + K, not from UV
    // derivatives the way GL would. TEX1_2 reads MXL=3, MMIN=LINEAR_MIPMAP_LINEAR, L=0, K=-4.0 while
    // the base pass runs MXL=0, so the reflection is a soft sheen off a small mip where the base
    // texture is sharp. Clamp at 8x8: the PS2 chain stops there and ours runs on to 1x1.
    float maxLod = max(log2(float(max(textureSize(uTex, 0).x, textureSize(uTex, 0).y))) - 3.0, 0.0);
    vec4 tex = textureLod(uTex, vUV2, clamp(log2(max(vFogZ, 1.0)) - 4.0, 0.0, maxLod));
#elif DUAL
    vec4 tex = texture(uTex, vUV2);
#else
    vec4 tex = texture(uTex, vUV);
#endif
#if ENVMAP
    // The reflection is lit, not added flat: context 2's vertex colour is coef scaled by the lit
    // surface colour, measured as 0x0F..0x2B where a flat coef of 0.5 would be 0x40. So coef takes
    // the material colour's place here and the shared lighting below does the rest.
    vec3 base = tex.rgb * uEnvCoef;
#else
    vec3 base = tex.rgb * mc;
#endif
    // The texel's own 0x80 means 1.0, so this is the one factor the GS unit scale belongs to.
    float alpha = min(ps2AScale(tex.a), 1.0);
    dbgTex = tex.rgb;
#else
    vec3 base = mc;
    float alpha = 1.0; // no texture: nothing to rescale
#endif
    vec3 light;
#if LIT
    light = uAmbient * uSurfAmbient;
    dbgAmbient = light;
#if HAS_NORMALS
    // Sky lights in OBJECT space via inverse(atomic LTM), with an invScale/recipInvScale
    // compensation when the matrix is non-orthonormal. World space matches for rigid
    // transforms only; scaled atomics will diverge here.
    vec3 n = normalize(vWorldN);
    vec3 diffuse = vec3(0.0);
    for (int i = 0; i < uDirCount; i++)
        diffuse += uDirColor[i] * max(dot(n, uDirDir[i]), 0.0);
    for (int i = 0; i < uLocalCount; i++)
    {
        vec4 p = uLocalParams[i];
        vec3 ray = uLocalPos[i] - vWorldPos;
        float dist = length(ray);
        vec3 L = ray / max(dist, 1e-6);
        float atten = max(1.0 - dist / max(p.x, 1e-4), 0.0);
        if (atten <= 0.0) continue;
        diffuse += uLocalColor[i] * max(dot(n, L), 0.0)
                 * atten * spotCone(p, L, uLocalAxis[i]);
    }
    light += diffuse * uSurfDiffuse;
    dbgDiffuse = diffuse * uSurfDiffuse;
#endif
#if VCOLOR
    light += vColor.rgb;
    dbgPrelit = vColor.rgb;
#endif
#elif VCOLOR
    light = vColor.rgb;
    dbgPrelit = vColor.rgb;
#else
    light = vec3(0.0);
#endif
    vec3 litClamped = min(light, vec3(1.0));
#if PER_PIXEL_LIT
    vec3 rgb = applyFog(base * litClamped);
#else
    vec3 rgb = applyFog(base * vLit);
#endif
#if VCOLOR
    float vA = vColor.a; // raw, like matA
#else
    float vA = 1.0;
#endif
    // Prelit and material alpha arrive in blend units already: RW pre-scales the vertex colour on
    // VU1 by vuSDcolScale, whose alpha term is 128/255 whether or not the draw is textured
    // (skyinst.c colScaleTexCmd128 / colScaleNoTexCmd128), which cancels the 255/128 exactly. Scaling
    // them here too made every alpha twice what the GS blends with.
    float testA = min(alpha * matA * vA, 1.0);
#if DUAL
    float outA = testA;
#elif BLEND
#if !LIT && !VCOLOR
    float outA = alpha;
#else
    float outA = testA;
#endif
#else
    float outA = 1.0; // opaque draw: the GS still stores full alpha
#endif
    // The GS tests the texture-function alpha and stores something that can differ from it: an opaque
    // draw is alpha-tested all the same, and writes full alpha. Testing the stored value instead made
    // every opaque draw test a constant 1.0, which no ATST can fail.
    if (ps2AtestFails(testA)) discard;
    if (ps2DateFails()) discard;
    frag = vec4(rgb, ps2AlphaPremul(outA));

    // Channel isolation (number keys): visualise one shading component at a time. Each is shown at full
    // opacity so it isn't lost to blending. 0 leaves the normal render above.
    if      (uDebugChannel == 1) frag = vec4(dbgTex, 1.0);                        // base texture
    else if (uDebugChannel == 2) frag = vec4(mc, 1.0);                            // material colour
    else if (uDebugChannel == 3) frag = vec4(dbgAmbient, 1.0);                    // ambient term
    else if (uDebugChannel == 4) frag = vec4(dbgDiffuse, 1.0);                    // diffuse term
    else if (uDebugChannel == 5) frag = vec4(dbgPrelit, 1.0);                     // prelit (vertex colour)
    else if (uDebugChannel == 6) frag = vec4(light, 1.0);                         // combined light, unclamped
    else if (uDebugChannel == 7) frag = vec4(base, 1.0);                          // base = texture * material
    else if (uDebugChannel == 8) frag = vec4(fract(vUV), 0.0, 1.0);               // texcoords
    else if (uDebugChannel == 9) frag = vec4(normalize(vWorldN) * 0.5 + 0.5, 1.0);// world normal
    else if (uDebugChannel == 10)                                                 // saturation mask
        frag = vec4(any(greaterThan(light, vec3(1.0))) ? vec3(1.0, 0.0, 0.0)
                                                       : litClamped * 0.5, 1.0);
    else if (uDebugChannel == 11) frag = vec4(vLit, 1.0);                         // vLit
#endif
}
