#include "shader_manager.h"

#include "common.h"
#include "gl.h"
#include "object_manager.h"
#include "render_queue.h"
#include "renderer.h"
#include "rw_to_gl.h"
#include "texture_manager.h"

#include <glm/gtc/type_ptr.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>

#ifndef SKY2_SHADER_DIR
#define SKY2_SHADER_DIR "shaders" // CMake bakes in the absolute source path; env can override
#endif

namespace sky2
{
	void ShaderManager::setDebugChannel(int ch) { debugChannelValue.store(ch); }

	// Lazily seeded from the uv_debug config (channel 8 = texcoords) so P3FES_UVDEBUG still works, or
	// P3FES_DEBUG_CHANNEL directly for the others (headless captures have no number-key input).
	int ShaderManager::debugChannel()
	{
		int v = debugChannelValue.load();
		if (v < 0)
		{
			v = sky2::config().uvDebug ? 8 : 0;
			if (const char* ch = sky2::env("DEBUG_CHANNEL"))
			{
				v = std::atoi(ch);
			}
			debugChannelValue.store(v);
		}
		return v;
	}

	// Load a shader file's source once, cached by name.
	const std::string& ShaderManager::loadSource(const char* name)
	{
		auto it = sources.find(name);
		if (it != sources.end())
		{
			return it->second;
		}
		const char* env = sky2::env("SHADER_DIR");
		const std::string path = std::string(env ? env : SKY2_SHADER_DIR) + "/" + name;
		std::ifstream f(path, std::ios::binary);
		std::string src;
		if (f)
		{
			src.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		}
		else
		{
			SKY2_ERROR(false, "shader file not found: %s", path.c_str());
		}
		return sources.emplace(name, resolveIncludes(src)).first->second;
	}

	// GLSL has no #include, so inline `#include "file.glsl"` lines ourselves: each names a file in the
	// shader dir, loaded (and itself include-resolved) via loadSource. Runs before the GL compiler sees
	// the source, so the directive never reaches it.
	std::string ShaderManager::resolveIncludes(const std::string& src)
	{
		std::string out;
		std::istringstream in(src);
		std::string line;
		while (std::getline(in, line))
		{
			const size_t at = line.find_first_not_of(" \t");
			if (at != std::string::npos && line.compare(at, 8, "#include") == 0)
			{
				const size_t q1 = line.find('"', at + 8);
				const size_t q2 = q1 == std::string::npos ? std::string::npos : line.find('"', q1 + 1);
				if (q2 != std::string::npos)
				{
					out += loadSource(line.substr(q1 + 1, q2 - q1 - 1).c_str());
					if (!out.empty() && out.back() != '\n') out += '\n';
					continue;
				}
				SKY2_ERROR(false, "malformed #include in shader: %s", line.c_str());
			}
			// Drop an editor-only include #extension line (e.g. GL_GOOGLE_include_directive): it lets an
			// IDE resolve our #includes, but we inline them here so the GL driver never needs it.
			if (at != std::string::npos && line.compare(at, 10, "#extension") == 0 && line.find("include") != std::string::npos)
			{
				continue;
			}
			out += line;
			out += '\n';
		}
		return out;
	}

	// Inject the defines right after the #version line.
	static std::string withDefines(std::string src, const std::string& defines)
	{
		if (defines.empty())
		{
			return src;
		}
		// Match whatever version the file declares rather than a hardcoded copy of it: a mismatch here
		// would silently drop the defines from the 3D shaders only, since the 2D ones pass none.
		const size_t at = src.find("#version ");
		const size_t eol = at == std::string::npos ? std::string::npos : src.find('\n', at);
		if (eol == std::string::npos)
		{
			SKY2_ERROR(false, "shader source has no #version line");
			return src;
		}
		src.insert(eol + 1, defines);
		return src;
	}

	GLuint ShaderManager::getShaderProgram(const char* vsFile, const char* fsFile, const std::string& defines,
		const char* gsFile)
	{
		const std::string key = std::string(vsFile) + "|" + fsFile + "|" + (gsFile ? gsFile : "") + "|" + defines;
		auto it = programs.find(key);
		if (it != programs.end())
		{
			return it->second;
		}
		const std::string vs = withDefines(loadSource(vsFile), defines);
		const std::string fs = withDefines(loadSource(fsFile), defines);
		const GLuint vsObj = gl::compileShader(GL_VERTEX_SHADER, vs.c_str());
		const GLuint fsObj = gl::compileShader(GL_FRAGMENT_SHADER, fs.c_str());
		const GLuint gsObj = gsFile ? gl::compileShader(GL_GEOMETRY_SHADER, withDefines(loadSource(gsFile), defines).c_str()) : 0;
		if (!vsObj || !fsObj || (gsFile && !gsObj)) // a bad edit: leave nothing cached so the next reload retries
		{
			glDeleteShader(vsObj);
			glDeleteShader(fsObj);
			glDeleteShader(gsObj);
			return 0;
		}
		GLuint prog = glCreateProgram();
		glAttachShader(prog, vsObj);
		glAttachShader(prog, fsObj);
		if (gsObj)
		{
			glAttachShader(prog, gsObj);
		}
		glLinkProgram(prog);
		glDeleteShader(vsObj); // flagged for deletion once the program owns them
		glDeleteShader(fsObj);
		glDeleteShader(gsObj);
		if (!gl::checkProgramLinked(prog, vsFile))
		{
			glDeleteProgram(prog);
			return 0;
		}
		gl::labelObject(GL_PROGRAM, prog, vsFile);
		programs.emplace(key, prog);
		return prog;
	}

	void ShaderManager::clearShaderCache()
	{
		for (auto& [key, prog] : programs)
			if (prog) glDeleteProgram(prog);
		programs.clear();
	}

	// Render thread only (GL context current). Re-read every shader file and drop every program so it
	// recompiles from disk. A holder that caches a program id past this must re-fetch it, which
	// `generation` is for; the 3D variants need no such check, being fetched per draw already. A bad
	// edit compiles to nothing and drawOneMesh skips that geometry (no crash); fix and reload again.
	void ShaderManager::reloadShaders()
	{
		sources.clear(); // force loadSource to re-read from disk (includes too)
		destroyProg3dCache(); // drop the per-variant uniform-location cache
		clearShaderCache();
		generation++;
		std::fprintf(stderr, "[sky2] shaders reloaded\n");
	}

	// Variant flags, in bit order; each emitted as `#define <name> 0|1`.
	static const struct
	{
		const char* name;
		uint32_t bit;
	} PROG3D_FLAGS[] = {
		{"SKINNED", P3_SKINNED}, {"UNLIT", P3_UNLIT}, {"LIT", P3_LIT}, {"HAS_NORMALS", P3_NORMALS},
		{"VCOLOR", P3_VCOLOR}, {"MODULATE", P3_MODULATE}, {"FOG", P3_FOG}, {"BLEND", P3_BLEND},
		{"DUAL", P3_DUAL}, {"TEXTURED", P3_TEXTURED}, {"ENVMAP", P3_ENVMAP},
		{"PER_PIXEL_LIT", P3_PERPIXELLIT}};

	// The #define block for a variant: the array sizes + the variant flags. The PS2 GS bits (PS2_UNIT,
	// the alpha test) come from the shaders' own #includes (gs_unit.glsl / gs_atest.glsl).
	static std::string definesFor(uint32_t variant)
	{
		char sizes[128];
		std::snprintf(sizes, sizeof(sizes),
			"#define MAX_DIR_LIGHTS %d\n#define MAX_LOCAL_LIGHTS %d\n",
			MAX_DIR_LIGHTS, MAX_LOCAL_LIGHTS);
		std::string defs = sizes;
		for (const auto& f : PROG3D_FLAGS)
			defs += std::string("#define ") + f.name + ((variant & f.bit) ? " 1\n" : " 0\n");
		return defs;
	}

	Prog3d ShaderManager::buildProg3d(uint32_t variant)
	{
		Prog3d p;
		// Rejecting whole triangles needs a geometry stage; no per-vertex or per-fragment test can
		// express it.
		p.prog = getShaderProgram("3d.vert", "3d.frag", definesFor(variant),
			sky2::config().farCull ? "3d.geom" : nullptr);
		if (!p.prog)
		{
			return p; // shader failed to build; drawOneMesh skips meshes with prog==0
		}
		auto loc = [&](const char* n) { return glGetUniformLocation(p.prog, n); };
		p.uMVP = loc("uMVP");
		p.uModel = loc("uModel");
		p.uColor = loc("uColor");
		p.uMatfxUVRow0 = loc("uMatfxUVRow0");
		p.uMatfxUVRow1 = loc("uMatfxUVRow1");
		p.uMatfxDualUVRow0 = loc("uMatfxDualUVRow0");
		p.uMatfxDualUVRow1 = loc("uMatfxDualUVRow1");
		p.uSurfAmbient = loc("uSurfAmbient");
		p.uSurfDiffuse = loc("uSurfDiffuse");
		p.uAmbient = loc("uAmbient");
		p.uDirColor = loc("uDirColor");
		p.uDirDir = loc("uDirDir");
		p.uDirCount = loc("uDirCount");
		p.uLocalColor = loc("uLocalColor");
		p.uLocalPos = loc("uLocalPos");
		p.uLocalAxis = loc("uLocalAxis");
		p.uLocalParams = loc("uLocalParams");
		p.uLocalCount = loc("uLocalCount");
		p.uFogColor = loc("uFogColor");
		p.uFogStart = loc("uFogStart");
		p.uFogEnd = loc("uFogEnd");
		p.uDebugChannel = loc("uDebugChannel");
		p.uAtestMode = loc("uGs.atestMode"); // GS alpha test (AFAIL split); default ALWAYS = no discard
		p.uAtestRef = loc("uGs.atestRef");
		p.uAlphaPremul = loc("uGs.alphaPremul"); // 3d blends map to GL factors, so premul is always 1.0
		p.uDateMode = loc("uDateMode"); // GS destination-alpha test (DATE/DATM); default 0 = no discard
		glUseProgram(p.prog);
		glUniform1i(loc("uTex"), 0); // base texture on unit 0
		glUniform1i(loc("uDateTex"), 1); // this pass's own color attachment (DATE read), unit 1
		// Bones is an std430 storage block with an explicit binding in the shader, so there is nothing
		// to bind here. uBoneOffset is per-program, which is why it is set in setProg3dUniforms rather
		// than once per mesh: the MatFX dual pass switches to a different variant mid-mesh.
		p.uBoneOffset = loc("uBoneOffset");
		// Envmap-only: the reflection basis and strength, set by the second pass rather than
		// setProg3dUniforms, which runs for the base pass too.
		p.uEnvView = loc("uEnvView");
		p.uEnvCoef = loc("uEnvCoef");
		p.uFarPlane = loc("uFarPlane"); // PS2All's VU1 far-plane triangle rejection; default 0 = off
		return p;
	}

	const Prog3d& ShaderManager::getProg3d(uint32_t variant)
	{
		auto it = prog3d.find(variant);
		if (it == prog3d.end())
			it = prog3d.emplace(variant, buildProg3d(variant)).first;
		return it->second;
	}

	void ShaderManager::destroyProg3dCache()
	{
		prog3d.clear(); // the programs are owned + freed by clearShaderCache
	}

	uint32_t ShaderManager::variantFor(const MeshDraw& m, const DrawState& gs, bool textured, bool dual)
	{
		uint32_t v = 0;
		if (m.boneCount > 0) v |= P3_SKINNED;
		if (m.material.unlit) v |= P3_UNLIT;
		if (m.material.lit) v |= P3_LIT;
		if (m.material.hasNormals) v |= P3_NORMALS;
		if (m.material.vcolor) v |= P3_VCOLOR;
		if (m.material.modulate) v |= P3_MODULATE;
		if (gs.fog) v |= P3_FOG;
		if (gs.blend.enable) v |= P3_BLEND;
		if (dual) v |= P3_DUAL;
		if (dual && m.matfx.pass2Env) v |= P3_ENVMAP;
		if (textured) v |= P3_TEXTURED;
		if (sky2::config().perPixelLighting) v |= P3_PERPIXELLIT;
		return v;
	}

	void ShaderManager::setProg3dUniforms(const Prog3d& p, const MeshDraw& m, const DrawState& gs,
		const FrameLights& lights, const glm::mat4& mvp)
	{
		glUniformMatrix4fv(p.uMVP, 1, GL_FALSE, glm::value_ptr(mvp));
		glUniformMatrix4fv(p.uModel, 1, GL_FALSE, glm::value_ptr(m.model));
		if (p.uBoneOffset != -1) glUniform1i(p.uBoneOffset, static_cast<GLint>(m.boneOffset));
		glUniform4fv(p.uColor, 1, glm::value_ptr(m.material.color));
		glUniform3fv(p.uMatfxUVRow0, 1, glm::value_ptr(m.matfx.uvRow0));
		glUniform3fv(p.uMatfxUVRow1, 1, glm::value_ptr(m.matfx.uvRow1));
		glUniform3fv(p.uMatfxDualUVRow0, 1, glm::value_ptr(m.matfx.dualUvRow0));
		glUniform3fv(p.uMatfxDualUVRow1, 1, glm::value_ptr(m.matfx.dualUvRow1));
		glUniform1f(p.uSurfAmbient, m.material.surfAmbient);
		glUniform1f(p.uSurfDiffuse, m.material.surfDiffuse);
		// Lights and fog are per-draw (m.lights): the game mutates the shared light objects between
		// draws in a frame (floor vs characters), so a per-pass value would be wrong for half of them.
		glUniform3fv(p.uAmbient, 1, &lights.ambient[0]);
		glUniform3fv(p.uDirColor, MAX_DIR_LIGHTS, &lights.dirColor[0][0]);
		glUniform3fv(p.uDirDir, MAX_DIR_LIGHTS, &lights.dirDir[0][0]);
		glUniform1i(p.uDirCount, lights.dirCount);
		glUniform1i(p.uLocalCount, lights.localCount); // positioned lights from sector ties (0 when unused)
		if (lights.localCount > 0)
		{
			glUniform3fv(p.uLocalColor, MAX_LOCAL_LIGHTS, &lights.localColor[0][0]);
			glUniform3fv(p.uLocalPos, MAX_LOCAL_LIGHTS, &lights.localPos[0][0]);
			glUniform3fv(p.uLocalAxis, MAX_LOCAL_LIGHTS, &lights.localAxis[0][0]);
			glUniform4fv(p.uLocalParams, MAX_LOCAL_LIGHTS, &lights.localParams[0][0]);
		}
		glUniform3fv(p.uFogColor, 1, &lights.fogColor[0]);
		glUniform1f(p.uFogStart, lights.fogStart);
		glUniform1f(p.uFogEnd, lights.fogEnd);
		glUniform1i(p.uDebugChannel, debugChannel());
		// Default the alpha test to no-discard; drawOneMesh overrides it per sub-pass for AFAIL splits.
		glUniform1i(p.uAtestMode, ATST_ALWAYS);
		glUniform1f(p.uAtestRef, 0.0f);
		glUniform1f(p.uAlphaPremul, gs.blend.alphaPremul); // GS ALPHA FIX combos need the shader-side prescale
	}
} // namespace sky2
