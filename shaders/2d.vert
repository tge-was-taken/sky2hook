#version 450 core
// Shared 2D vertex shader (font + Im2D): screen-space pos, passthrough UV + colour.
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aCol;
layout(location = 3) in float aZ;
uniform mat4 uProj;
// Full scale of the GS depth register, so aZ maps onto the same buffer the 3D passes wrote.
uniform float uZMax;
out vec2 vUV;
out vec4 vCol;
void main()
{
    vUV = aUV;
    vCol = aCol;
    gl_Position = uProj * vec4(aPos, 0, 1);
    // The GS depth register is an integer field, so a float that does not fit was never going to
    // reach the rasterizer as that value: games leave z unwritten on draws that do not test it.
    // Larger GS Z is nearer, which is the reverse of GL, hence the flip.
    gl_Position.z = uZMax > 0.0 ? 1.0 - 2.0 * clamp(aZ, 0.0, uZMax) / uZMax : 0.0;
}
