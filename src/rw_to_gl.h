// Straight RenderWare-enum and GS-register -> OpenGL conversions: blend factors, stencil op/func,
// and texture address + filter modes. Pure mapping functions, no state.
#pragma once

#include "sky2/gs.h"

#include <glad/gl.h>

namespace sky2
{
	struct GsBlend;
	struct GsAtest;

	// GS ALPHA register (Cv = (A-B)*C + D) -> GL equation + factors. Reports the equations GL cannot
	// express rather than dropping them. Leaves GsBlend::active alone; the caller owns that.
	GsBlend gsAlphaToBlend(GIFRegALPHA reg);

	// GS TEST register -> the alpha test we apply as a shader discard. Only ATE with AFAIL=KEEP
	// discards; the other fail modes still write some of the pixel.
	GsAtest gsTestToAtest(GIFRegTEST reg);

	GLenum rwBlendToGL(unsigned b, const char* what); // RwBlendFunction -> GL blend factor
	GLenum rwStencilOpToGL(unsigned op); // RwStencilOperation -> GL stencil op
	GLenum rwStencilFuncToGL(unsigned f); // RwStencilFunction -> GL compare func
	GLint rwAddressToGL(unsigned a); // RwTextureAddressMode -> GL wrap
	void rwFilterToGL(unsigned f, GLint& min, GLint& mag); // RwTextureFilterMode -> GL min/mag
} // namespace sky2
