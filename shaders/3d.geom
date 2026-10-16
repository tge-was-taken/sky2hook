#version 450 core
// PS2All's VU1 rejects a whole triangle rather than clipping it: cullxformpersp gives each vertex a
// clip code from a near-plane, far-plane and guard-band x/y test, ORs the three codes of a triangle,
// and folds the result into the GS ADC bit so the triangle is never kicked. GL clips instead and
// keeps the on-screen remainder, which paints geometry the hardware never draws.
//
// Only the far-plane term is applied. RenderWare picks between that culling transform and a true
// clipper per draw, and sky2 cannot yet tell which a given draw got, so the near and guard-band
// terms are unsafe: geometry straddling the near plane or the screen edge is ordinary, and culling
// it on a clipping draw deletes visible surfaces. Geometry past the far plane is not ordinary, since
// scenes are authored inside their own camera's far plane, so that term only ever fires on data that
// is not real geometry, at the hardware's own threshold rather than an invented one.
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

layout(location = 0) in vec3 iWorldN[];
layout(location = 1) in vec3 iWorldPos[];
layout(location = 2) in vec2 iUV[];
layout(location = 3) in vec2 iUV2[];
layout(location = 4) in vec4 iColor[];
layout(location = 5) in vec3 iLit[];
layout(location = 6) in float iFogZ[];

layout(location = 0) out vec3 vWorldN;
layout(location = 1) out vec3 vWorldPos;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec2 vUV2;
layout(location = 4) out vec4 vColor;
layout(location = 5) out vec3 vLit;
layout(location = 6) out float vFogZ;

uniform float uFarPlane; // zero leaves the rejection off

void main()
{
    if (uFarPlane > 0.0
        && (gl_in[0].gl_Position.w > uFarPlane || gl_in[1].gl_Position.w > uFarPlane
            || gl_in[2].gl_Position.w > uFarPlane))
    {
        return;
    }
    for (int i = 0; i < 3; i++)
    {
        vWorldN = iWorldN[i];
        vWorldPos = iWorldPos[i];
        vUV = iUV[i];
        vUV2 = iUV2[i];
        vColor = iColor[i];
        vLit = iLit[i];
        vFogZ = iFogZ[i];
        gl_Position = gl_in[i].gl_Position;
        EmitVertex();
    }
    EndPrimitive();
}
