// sky2 shader manager: owns all GL programs. Shader sources load from files once (P3FES_SHADER_DIR,
// else the CMake-baked SKY2_SHADER_DIR), so they are editable without a rebuild; the 3D program is a
// set of compile-time variants.
#pragma once

#include "common.h" // MeshDraw, FrameLights, glad, glm

#include <atomic>
#include <map>
#include <string>
#include <unordered_map>

namespace sky2
{
	// 3D program variant flags; each maps to a shader #define of the same name.
	enum Prog3dBit : uint32_t
	{
		P3_SKINNED = 1u << 0,
		P3_UNLIT = 1u << 1,
		P3_LIT = 1u << 2,
		P3_NORMALS = 1u << 3,
		P3_VCOLOR = 1u << 4,
		P3_MODULATE = 1u << 5,
		P3_FOG = 1u << 6,
		P3_BLEND = 1u << 7,
		P3_DUAL = 1u << 8,
		P3_TEXTURED = 1u << 9,
		P3_ENVMAP = 1u << 10, // with DUAL: the second pass generates texcoords from the normal
		P3_PERPIXELLIT = 1u << 11, // enhancement: Phong instead of the GS's own per-vertex Gouraud
	};

	// A compiled 3D variant + its uniform locations (an unused uniform is -1, which glUniform* ignores).
	struct Prog3d
	{
		GLuint prog = 0;
		GLint uMVP, uModel, uColor, uMatfxUVRow0, uMatfxUVRow1, uMatfxDualUVRow0, uMatfxDualUVRow1,
			uSurfAmbient, uSurfDiffuse, uAmbient, uDirColor, uDirDir, uDirCount,
			uLocalColor, uLocalPos, uLocalAxis, uLocalParams, uLocalCount,
			uFogColor, uFogStart, uFogEnd, uDebugChannel, uAtestMode, uAtestRef, uAlphaPremul,
			uBoneOffset, uEnvView, uEnvCoef, uDateMode, uFarPlane;
	};

	// Pass-scope shading state, the same for every mesh in a pass.
	// Owns every GL program. Shader sources load from files once (P3FES_SHADER_DIR, else the CMake-baked
	// SKY2_SHADER_DIR), editable without a rebuild; the 3D program is a set of compile-time variants.
	class ShaderManager
	{
	public:
		// Compile-or-return a cached program for the two shader files, `defines` injected after #version.
		GLuint getShaderProgram(const char* vsFile, const char* fsFile, const std::string& defines = {},
			const char* gsFile = nullptr);
		void clearShaderCache();
		void reloadShaders(); // drop cached sources + programs so they recompile from disk (F5 hot-reload)
		// Bumped by reloadShaders. A holder that caches a program id compares this and re-fetches.
		uint32_t shaderGeneration() const { return generation; }
		uint32_t variantFor(const MeshDraw& m, const DrawState& gs, bool textured, bool dual);
		const Prog3d& getProg3d(uint32_t variant);
		void setProg3dUniforms(const Prog3d& p, const MeshDraw& m, const DrawState& gs,
			const FrameLights& lights, const glm::mat4& mvp);
		void destroyProg3dCache(); // free the variant location cache (programs freed by clearShaderCache)
		void setDebugChannel(int ch);
		int debugChannel();

	private:
		const std::string& loadSource(const char* name); // read a shader file once (includes inlined), cached by name
		std::string resolveIncludes(const std::string& src); // inline `#include "file.glsl"` from the shader dir
		Prog3d buildProg3d(uint32_t variant);
		std::unordered_map<std::string, std::string> sources; // shader file -> source
		std::map<std::string, GLuint> programs; // "vs|fs|defines" -> program
		std::unordered_map<uint32_t, Prog3d> prog3d; // per-variant uniform locations
		uint32_t generation = 0;
		// 3D debug channel: written by the input thread (number keys) and the render thread (imgui
		// overlay), read by the render thread per draw.
		std::atomic<int> debugChannelValue{-1};
	};
} // namespace sky2
