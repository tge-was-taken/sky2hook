// Stateless GL helpers shared by the renderer, the shader/texture managers and the pipeline render
// code. No renderer state, so they are plain functions in sky2::gl.
#pragma once

#include "common.h" // StencilState, glad/gl.h

namespace sky2::gl
{
	GLuint compileShader(GLenum type, const char* src);
	bool checkProgramLinked(GLuint prog, const char* name); // false = link failed (logged, non-fatal)
	void labelObject(GLenum identifier, GLuint name, const char* label);
	void checkGlErrors(const char* where);
	void applyStencil(const StencilState& s);
} // namespace sky2::gl
