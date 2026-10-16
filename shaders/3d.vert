#version 450 core
// 3D vertex shader. Variant #defines injected after #version (prog3d.cpp); PS2_UNIT below.
#include "gs_unit.glsl"
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
layout(location = 4) in vec4 aBoneIdx;
layout(location = 5) in vec4 aBoneWt;
layout(location = 6) in vec2 aUV2;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform vec3 uMatfxUVRow0;
uniform vec3 uMatfxUVRow1;
uniform vec3 uMatfxDualUVRow0;
uniform vec3 uMatfxDualUVRow1;
#if ENVMAP
uniform mat3 uEnvView; // world -> camera rotation; the reflection is anchored to the camera
#endif
#if SKINNED
uniform int uBoneOffset; // this draw's slice of the frame-wide arena
layout(std430, binding = 0) readonly buffer Bones { mat4 uBones[]; };
#endif
uniform vec3 uAmbient;
uniform vec3 uDirColor[MAX_DIR_LIGHTS];
uniform vec3 uDirDir[MAX_DIR_LIGHTS];
uniform int uDirCount;
uniform vec3 uLocalColor[MAX_LOCAL_LIGHTS];
uniform vec3 uLocalPos[MAX_LOCAL_LIGHTS];
uniform vec3 uLocalAxis[MAX_LOCAL_LIGHTS];
uniform vec4 uLocalParams[MAX_LOCAL_LIGHTS]; // (radius, minusCosAngle, type 0=pt/1=spot/2=soft, -)
uniform int uLocalCount;
uniform float uSurfAmbient;
uniform float uSurfDiffuse;
layout(location = 0) out vec3 vWorldN;
layout(location = 1) out vec3 vWorldPos;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec2 vUV2;
layout(location = 4) out vec4 vColor;
layout(location = 5) out vec3 vLit;
layout(location = 6) out float vFogZ;

float spotCone(vec4 p, vec3 L, vec3 axis)
{
    if (p.z < 0.5) return 1.0;                       // point light: no cone
    float edge = -p.y;                               // -minusCosAngle
    float c    = dot(-L, axis);
    return (p.z > 1.5) ? clamp((c - edge) / max(1.0 - edge, 1e-4), 0.0, 1.0)
                       : step(edge, c);
}

void main()
{
    vec4 pos = vec4(aPos, 1.0);
    vec3 nrm = aNormal;
#if SKINNED
    mat4 sm = uBones[uBoneOffset + int(aBoneIdx.x)] * aBoneWt.x
            + uBones[uBoneOffset + int(aBoneIdx.y)] * aBoneWt.y
            + uBones[uBoneOffset + int(aBoneIdx.z)] * aBoneWt.z
            + uBones[uBoneOffset + int(aBoneIdx.w)] * aBoneWt.w;
    pos = sm * pos;
    nrm = mat3(sm) * nrm;
#endif
    vWorldN = mat3(uModel) * nrm;
    vWorldPos = vec3(uModel * pos);
    vec3 uvh = vec3(aUV, 1.0);
    vUV = vec2(dot(uvh, uMatfxUVRow0), dot(uvh, uMatfxUVRow1));
#if ENVMAP
    // RpMatFX env map: the reflection texcoords are the camera-space normal mapped onto the unit
    // square. Both axes are negated: the projection flips X for RW's left-handedness, and V runs
    // down. Measured against the reference; the other three sign combinations all place it worse.
    vec3 envN = normalize(uEnvView * vWorldN);
    vUV2 = -envN.xy * 0.5 + 0.5;
#else
    vec3 uv2h = vec3(aUV2, 1.0);
    vUV2 = vec2(dot(uv2h, uMatfxDualUVRow0), dot(uv2h, uMatfxDualUVRow1));
#endif
    vColor = aColor;

#if !PER_PIXEL_LIT
#if LIT
    vec3 lit = uAmbient * uSurfAmbient;
#if HAS_NORMALS
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
    lit += diffuse * uSurfDiffuse;
#endif
#if VCOLOR
    lit += aColor.rgb;
#endif
#elif VCOLOR
    vec3 lit = aColor.rgb;
#else
    vec3 lit = vec3(0.0);
#endif
    vLit = min(lit, vec3(1.0));
#endif

    gl_Position = uMVP * pos;
    vFogZ = gl_Position.w; // view-space z, for fog
}
