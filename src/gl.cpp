// Stateless GL helpers: shader/program build reporting, debug labels, error checks, stencil state.
#include "gl.h"

#include <string>

namespace sky2::gl
{
	// Returns 0 on failure (logged, not fatal) so a bad shader edit can be hot-reloaded and recovered
	// from instead of aborting.
	GLuint compileShader(GLenum type, const char* src)
	{
		GLuint s = glCreateShader(type);
		glShaderSource(s, 1, &src, nullptr);
		glCompileShader(s);
		GLint status = GL_FALSE;
		glGetShaderiv(s, GL_COMPILE_STATUS, &status);
		if (status != GL_TRUE)
		{
			GLint len = 0;
			glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
			std::string log(static_cast<size_t>(len), '\0');
			if (len > 0)
			{
				glGetShaderInfoLog(s, len, nullptr, log.data());
			}
			SKY2_ERROR(false, "shader compile failed (type=0x%X): %s", type, log.c_str());
			glDeleteShader(s);
			return 0;
		}
		return s;
	}

	// Returns whether the program linked; failures are logged, not fatal (see compileShader).
	bool checkProgramLinked(GLuint prog, const char* name)
	{
		GLint status = GL_FALSE;
		glGetProgramiv(prog, GL_LINK_STATUS, &status);
		if (status != GL_TRUE)
		{
			GLint len = 0;
			glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
			std::string log(static_cast<size_t>(len), '\0');
			if (len > 0)
			{
				glGetProgramInfoLog(prog, len, nullptr, log.data());
			}
			SKY2_ERROR(false, "program link failed (%s): %s", name, log.c_str());
			return false;
		}
		return true;
	}

	void labelObject(GLenum identifier, GLuint name, const char* label)
	{
		if (GLAD_GL_KHR_debug)
		{
			glObjectLabel(identifier, name, -1, label);
		}
	}

	void checkGlErrors(const char* where)
	{
		for (GLenum err = glGetError(); err != GL_NO_ERROR; err = glGetError())
		{
			SKY2_ASSERT(false, "%s: GL error 0x%X during setup", where, err);
		}
	}

	// Apply a per-draw stencil snapshot. Fields already hold GL enums (see updateStencilFromRwState).
	void applyStencil(const StencilState& s)
	{
		if (!s.enable)
		{
			glDisable(GL_STENCIL_TEST);
			return;
		}
		glEnable(GL_STENCIL_TEST);
		glStencilFunc(s.func, s.ref, s.readMask);
		glStencilOp(s.sfail, s.zfail, s.zpass);
		glStencilMask(s.writeMask);
	}
} // namespace sky2::gl
