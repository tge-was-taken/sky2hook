// sky2 Renderer: the render-thread consumer. The 2D/3D GL programs and passes, the
// display/off-screen FBOs, and the present loop. All GL calls run on the render thread.
#include "common.h"
#include "gl.h"
#include "object_manager.h"
#include "render_queue.h"
#include "renderer.h"
#include "rw_to_gl.h"
#include "texture_manager.h"

#include "render_queue.h"
#include "renderer.h"
#include "shader_manager.h"
#include "texture_manager.h"

#include "enhancements/msaa.h"
#include "enhancements/shadow_detail.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace sky2;

namespace sky2
{

	// Hand the finished frame to the render thread (called on the EE build thread at end-of-frame).
	// The ring owns the storage, so this neither moves nor frees anything.
	void Renderer::submit(Frame* frame)
	{
		frames->publish(frame);
	}
} // namespace sky2

// Free all render-thread GL resources and clear the caches, then reset to defaults. Must run on the
// render thread (needs the GL context). Called on device teardown; also usable for a full reset.
void GlState::destroy(ShaderManager& shaders, TextureManager& textures)
{
	shaders.destroyProg3dCache();
	shaders.clearShaderCache(); // owns every program (font/Im2D/prog3d)
	im2d.destroy(); // Im2D VAO/VBO live in the Im2D pipeline
	for (GpuGeom& g : gpuGeom)
	{
		if (g.vao) glDeleteVertexArrays(1, &g.vao);
		if (g.vbo) glDeleteBuffers(1, &g.vbo);
	}
	for (GpuIndexBuf& ib : gpuIdx)
		if (ib.ebo) glDeleteBuffers(1, &ib.ebo);
	if (boneSSBO) glDeleteBuffers(1, &boneSSBO);
	boneSSBOBones = 0;
	if (transientVao) glDeleteVertexArrays(1, &transientVao);
	for (GLuint b : {transientVbo, transientEbo})
		if (b) glDeleteBuffers(1, &b);
	for (GLuint t : {displayColor, capture.tex, prevFrame.tex})
		if (t) glDeleteTextures(1, &t);
	for (GLuint f : {capture.fbo, prevFrame.fbo})
		if (f) glDeleteFramebuffers(1, &f);
	textures.destroyGl(); // free the decoded/RT GL textures (the cache lives in TextureManager)
	for (auto& [key, t] : targets)
	{
		glDeleteFramebuffers(1, &t.fbo);
		glDeleteTextures(1, &t.colorTex);
		glDeleteRenderbuffers(1, &t.depthRbo);
		if (t.flipFbo) glDeleteFramebuffers(1, &t.flipFbo);
		if (t.flipTex) glDeleteTextures(1, &t.flipTex);
		if (t.msFbo) glDeleteFramebuffers(1, &t.msFbo);
		if (t.msColor) glDeleteRenderbuffers(1, &t.msColor);
		if (t.msDepth) glDeleteRenderbuffers(1, &t.msDepth);
	}
	if (displayFbo) glDeleteFramebuffers(1, &displayFbo);
	if (displayDepth) glDeleteRenderbuffers(1, &displayDepth);
	if (displayMsFbo) glDeleteFramebuffers(1, &displayMsFbo);
	if (displayMsColor) glDeleteRenderbuffers(1, &displayMsColor);
	if (displayMsDepth) glDeleteRenderbuffers(1, &displayMsDepth);
	*this = GlState{}; // handles back to 0/-1, caches cleared
}

namespace sky2
{
	// Current display-FBO dimensions, for the trace unit's per-pass dump.
	void Renderer::displayFboSize(int& w, int& h)
	{
		w = gl.displayW;
		h = gl.displayH;
	}

	// Global GL defaults set once at startup; per-draw code overrides blend/depth as needed.
	void Renderer::initGL()
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		if (const GLubyte* r = glGetString(GL_RENDERER))
			glRenderer = reinterpret_cast<const char*>(r);
		sky2::gl::checkGlErrors("initGL");
	}

	Renderer::PassPreview Renderer::passPreview(const PassInfo& p) const
	{
		PassPreview pv;
		if (const auto it = gl.targets.find(p.target); it != gl.targets.end())
		{
			// flipTex is what the game samples; fall back to the raw attachment before the first publish.
			pv.tex = it->second.flipTex ? it->second.flipTex : it->second.colorTex;
			pv.flipV = it->second.flipTex == 0;
			pv.w = it->second.w;
			pv.h = it->second.h;
		}
		// Deliberately nothing for a display pass: that target is the window, and binding a live render
		// target as a texture every frame costs a transition for something you can already see.
		return pv;
	}

	// GPU bytes we hold, by kind. Walks the resident caches, so call it once a frame at most.
	Renderer::MemoryUse Renderer::memoryUse() const
	{
		MemoryUse m;
		m.textureBytes = textures->uploadedBytes();
		m.textureCount = textures->textureCount();
		for (const GpuGeom& g : gl.gpuGeom)
		{
			if (!g.vao) continue; // the slot vector is dense, so most entries are holes
			m.geometryBytes += geometry->verts.view(g.id).size() * sizeof(Vertex3D);
			m.geometryCount++;
		}
		for (const GpuIndexBuf& ib : gl.gpuIdx)
		{
			if (!ib.ebo) continue;
			m.indexBytes += geometry->indices.view(ib.id).size() * sizeof(uint16_t);
			m.indexCount++;
		}
		for (const auto& [key, t] : gl.targets)
		{
			m.targetBytes += size_t(t.w) * t.h * (t.flipTex ? 12 : 8); // color + depth (+ flip copy)
			m.targetCount++;
		}
		m.screenBytes = size_t(gl.displayW) * gl.displayH * 8; // display color + depth
		m.screenBytes += size_t(gl.capture.w) * gl.capture.h * 4;
		m.screenBytes += size_t(gl.prevFrame.w) * gl.prevFrame.h * 4;
		return m;
	}


	// Set up the Vertex3D attribute layout on the currently-bound VAO, reading from the bound array
	// buffer. Called once per geometry when its resident VAO/VBO is created (bindMeshBuffers).
	static void setupVertex3DAttribs()
	{
		const GLsizei st = sizeof(Vertex3D);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(offsetof(Vertex3D, pos)));
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(offsetof(Vertex3D, normal)));
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(offsetof(Vertex3D, uv)));
		glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(offsetof(Vertex3D, color)));
		glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(offsetof(Vertex3D, boneIndex)));
		glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(offsetof(Vertex3D, boneWeight)));
		glVertexAttribPointer(6, 2, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(offsetof(Vertex3D, uv2)));
		for (GLuint i = 0; i <= 6; i++)
			glEnableVertexAttribArray(i);
	}

	void Renderer::init3D()
	{
		// The 3D programs are compiled lazily per variant (getProg3d), and geometry buffers are created
		// per geometry on first use and kept resident (bindMeshBuffers). Here we set up only the bone
		// SSBO (binding 0). It holds the whole frame's arena, so its size follows the scene rather
		// than a fixed MAX_BONES; growth happens in uploadBones.
		glCreateBuffers(1, &gl.boneSSBO);
		sky2::gl::labelObject(GL_BUFFER, gl.boneSSBO, "gl.boneSSBO");
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, gl.boneSSBO);

		// The transient-geometry arena. Its attribute state is declared once here: the buffer names
		// never change, and glNamedBufferData reallocating their storage does not disturb a VAO.
		glGenVertexArrays(1, &gl.transientVao);
		glCreateBuffers(1, &gl.transientVbo);
		glCreateBuffers(1, &gl.transientEbo);
		sky2::gl::labelObject(GL_BUFFER, gl.transientVbo, "gl.transientVbo");
		sky2::gl::labelObject(GL_BUFFER, gl.transientEbo, "gl.transientEbo");
		glBindVertexArray(gl.transientVao);
		glBindBuffer(GL_ARRAY_BUFFER, gl.transientVbo);
		setupVertex3DAttribs();
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl.transientEbo); // recorded into transientVao
		glBindVertexArray(0);
		sky2::gl::checkGlErrors("init3D");
	}

	// One upload per frame for every Im3D batch, in place of a GPU buffer per batch. Named-buffer
	// uploads because binding the element buffer would record into whatever VAO is current.
	void Renderer::uploadTransientGeometry(const Frame& frame)
	{
		if (frame.transientVerts.empty())
		{
			return;
		}
		ScopedNs timer{geometry->stats.uploadNs};
		const std::vector<Vertex3D>& verts = interp.transientVertsFor(frame);
		const size_t vBytes = verts.size() * sizeof(Vertex3D);
		const size_t iBytes = frame.transientIdx.size() * sizeof(uint16_t);
		glNamedBufferData(gl.transientVbo, static_cast<GLsizeiptr>(vBytes), verts.data(), GL_STREAM_DRAW);
		glNamedBufferData(gl.transientEbo, static_cast<GLsizeiptr>(iBytes), frame.transientIdx.data(),
			GL_STREAM_DRAW);
		bumpStat(geometry->stats.vertUploads);
		bumpStat(geometry->stats.idxUploads);
		bumpStat(geometry->stats.uploadBytes, vBytes + iBytes);
	}


	// Free the GL resources of rasters the game destroyed (B3): the decoded texture keyed by pixel
	// address, and any render-target FBO keyed by raster address (its color attachment is also the
	// RT-tagged gl.texCache entry, so erase that without a second delete).
	void Renderer::evictDestroyedRasters()
	{
		auto& tm = *textures;
		for (const auto& [rasterAddr, pxAddr] : tm.takePendingDestroy())
		{
			tm.evictTexture(pxAddr); // the decoded texture keyed by pixel address
			if (const auto it = gl.targets.find(rasterAddr); it != gl.targets.end())
			{
				glDeleteFramebuffers(1, &it->second.fbo);
				glDeleteTextures(1, &it->second.colorTex);
				glDeleteRenderbuffers(1, &it->second.depthRbo);
				if (it->second.flipFbo) glDeleteFramebuffers(1, &it->second.flipFbo);
				if (it->second.flipTex) glDeleteTextures(1, &it->second.flipTex);
				gl.targets.erase(it);
				tm.eraseRenderTargetTex(rasterAddr); // colorTex/flipTex already deleted above
			}
		}
	}

	// Render thread, per frame: free the GL resources of destroyed rasters, then drain the upload queue
	// into the texture cache. The texture cache itself lives in TextureManager; the render-target FBOs
	// (gl.targets) stay here, so the destroy join runs in evictDestroyedRasters.
	void Renderer::uploadPendingTextures()
	{
		evictDestroyedRasters();
		textures->uploadPending();
	}


	// Attach an RGBA8 color texture + depth24 renderbuffer to a fresh FBO, sized w x h. The color
	// texture is left bound; the caller keeps the ids. Aborts if the FBO is incomplete.
	static void createColorDepthFbo(GLuint& fbo, GLuint& colorTex, GLuint& depthRbo, int w, int h,
		const char* label)
	{
		glGenTextures(1, &colorTex);
		glBindTexture(GL_TEXTURE_2D, colorTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		// Combined depth+stencil: the RW stencil render states need a real stencil buffer on the
		// scene FBO (the default framebuffer's stencil doesn't back an FBO render target). 32F depth
		// (not 24-unorm) for the precision headroom: PS2 near/far ratios z-fight coplanar geometry
		// across passes at 24-bit, which flickers as the camera moves.
		glGenRenderbuffers(1, &depthRbo);
		glBindRenderbuffer(GL_RENDERBUFFER, depthRbo);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, w, h);
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		gl::labelObject(GL_FRAMEBUFFER, fbo, label);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthRbo);
		const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		SKY2_ASSERT(status == GL_FRAMEBUFFER_COMPLETE, "%s FBO incomplete (0x%X, %dx%d)",
			label, status, w, h);
	}

	// Multisampled storage that resolves into a single-sample attachment, so everything downstream
	// reads the resolved one and only the pass itself knows it was multisampled.
	static void createMultisampleFbo(GLuint& fbo, GLuint& color, GLuint& depth, int w, int h,
		int samples, const char* label)
	{
		glGenRenderbuffers(1, &color);
		glBindRenderbuffer(GL_RENDERBUFFER, color);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
		glGenRenderbuffers(1, &depth);
		glBindRenderbuffer(GL_RENDERBUFFER, depth);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH32F_STENCIL8, w, h);
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		gl::labelObject(GL_FRAMEBUFFER, fbo, label);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		{
			SKY2_ERROR(false, "%s incomplete at %d samples; falling back to one", label, samples);
			glDeleteFramebuffers(1, &fbo);
			glDeleteRenderbuffers(1, &color);
			glDeleteRenderbuffers(1, &depth);
			fbo = color = depth = 0;
		}
	}

	void Renderer::ensureDisplayFbo(int w, int h)
	{
		w = std::max(w, 1);
		h = std::max(h, 1);
		const int samples = enhancements::displaySampleCount();
		if (gl.displayFbo && (gl.displayW != w || gl.displayH != h || gl.displaySamples != samples))
		{
			glDeleteFramebuffers(1, &gl.displayFbo);
			glDeleteTextures(1, &gl.displayColor);
			glDeleteRenderbuffers(1, &gl.displayDepth);
			if (gl.displayMsFbo) glDeleteFramebuffers(1, &gl.displayMsFbo);
			if (gl.displayMsColor) glDeleteRenderbuffers(1, &gl.displayMsColor);
			if (gl.displayMsDepth) glDeleteRenderbuffers(1, &gl.displayMsDepth);
			gl.displayFbo = gl.displayColor = gl.displayDepth = 0;
			gl.displayMsFbo = gl.displayMsColor = gl.displayMsDepth = 0;
		}
		if (!gl.displayFbo)
		{
			createColorDepthFbo(gl.displayFbo, gl.displayColor, gl.displayDepth, w, h, "gl.displayFbo");
			gl.displayW = w;
			gl.displayH = h;
			gl.displaySamples = samples;
			if (samples > 1)
			{
				createMultisampleFbo(gl.displayMsFbo, gl.displayMsColor, gl.displayMsDepth, w, h,
					samples, "gl.displayMsFbo");
				if (!gl.displayMsFbo)
				{
					gl.displaySamples = 1;
				}
			}
		}
	}

	// The FBO the display passes render into: the multisampled one when there is one, since resolving
	// is what makes displayFbo readable rather than where drawing goes.
	void Renderer::bindDisplayFbo()
	{
		glBindFramebuffer(GL_FRAMEBUFFER, gl.displayMsFbo ? gl.displayMsFbo : gl.displayFbo);
		passColorTex = gl.displayColor;
		passSamples = gl.displaySamples;
	}

	// Always resolves rather than tracking whether it is needed: a cached "already resolved" flag goes
	// stale the moment another draw lands, and a feedback tap reading a stale resolve is a bug that
	// looks like a rendering difference.
	void Renderer::resolveDisplay()
	{
		if (!gl.displayMsFbo)
		{
			return;
		}
		glDisable(GL_SCISSOR_TEST); // else a scissor left by a draw clips the resolve
		glBindFramebuffer(GL_READ_FRAMEBUFFER, gl.displayMsFbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gl.displayFbo);
		glBlitFramebuffer(0, 0, gl.displayW, gl.displayH, 0, 0, gl.displayW, gl.displayH,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_FRAMEBUFFER, gl.displayMsFbo);
	}

	// A feedback effect's UVs are normalized over the GS buffer (e.g. 1024x512) whose visible area is
	// gGameRes (640x448). Pin the capture to that original buffer resolution, times an optional integer
	// factor, NOT the upscaled display res, so screen-space effects stay at their authored resolution at
	// any window size (factor 1 = original, byte-identical to native; 2/4 = sharper integer multiples).
	bool Renderer::captureGeometry(CaptureGeometry& g) const
	{
		if (gl.displayW <= 0 || gl.displayH <= 0)
			return false;
		const float visW = gameResW.load() > 0.0f ? gameResW.load() : float(gl.displayW);
		const float visH = gameResH.load() > 0.0f ? gameResH.load() : float(gl.displayH);
		const int gsW = objects->frameBufferW > 0 ? objects->frameBufferW : int(visW);
		const int gsH = objects->frameBufferH > 0 ? objects->frameBufferH : int(visH);
		const int factor = std::max(1, sky2::config().feedbackScale);
		g.capW = std::max(1, gsW * factor);
		g.capH = std::max(1, gsH * factor);
		g.capVisW = g.capW * (visW / float(gsW)); // capture texels holding visible content
		g.capVisH = g.capH * (visH / float(gsH));
		return true;
	}

	void Renderer::ensureCaptureTarget(CaptureTarget& t, const CaptureGeometry& g, TextureId texId,
		const char* label)
	{
		if (t.tex && t.w == g.capW && t.h == g.capH)
		{
			return;
		}
		if (t.tex)
			glDeleteTextures(1, &t.tex);
		if (t.fbo)
			glDeleteFramebuffers(1, &t.fbo);
		glGenTextures(1, &t.tex);
		glBindTexture(GL_TEXTURE_2D, t.tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g.capW, g.capH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glGenFramebuffers(1, &t.fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
		sky2::gl::labelObject(GL_TEXTURE, t.tex, label);
		// Black-pad once, here at (re)creation: the blits only ever write the display region, so the pad
		// (rows/cols outside it, sampled when the GS buffer is larger than the display) stays black
		// without a full-screen clear on every capture (there are many per effect frame).
		glDisable(GL_SCISSOR_TEST);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		t.w = g.capW;
		t.h = g.capH;
		textures->putTexture(texId, t.tex);
	}

	void Renderer::blitDisplayRegion(const CaptureTarget& dst, const CaptureGeometry& g, float u0, float v0,
		float u1, float v1)
	{
		// Map UV -> capture content texels -> display letterbox pixels, so the copy reads the scene, not
		// the bars (else the effect is offset by the bar width). Refresh a guard-banded superset.
		GLint vpi[4];
		glGetIntegerv(GL_VIEWPORT, vpi); // the pass's letterbox viewport (CPU-side state, no GPU stall)
		const float vpX = float(vpi[0]), vpY = float(vpi[1]), vpW = float(vpi[2]), vpH = float(vpi[3]);
		if (vpW <= 0.0f || vpH <= 0.0f)
			return;
		if (u0 > u1) std::swap(u0, u1);
		if (v0 > v1) std::swap(v0, v1);
		const float guard = 2.0f;
		const float cx0 = std::max(0.0f, u0 * g.capW - guard), cx1 = std::min(g.capVisW, u1 * g.capW + guard);
		const float cy0 = std::max(0.0f, v0 * g.capH - guard), cy1 = std::min(g.capVisH, v1 * g.capH + guard);
		if (cx1 <= cx0 || cy1 <= cy0)
			return; // entirely in the black pad; nothing to refresh
		// Capture content fraction -> display letterbox pixels; Y flips (capture y=0 is the visible top).
		auto dX = [&](float cx) { return vpX + (cx / g.capVisW) * vpW; };
		auto dY = [&](float cy) { return vpY + vpH - (cy / g.capVisH) * vpH; };
		const int sx0 = std::max(int(vpX), int(std::floor(dX(cx0))));
		const int sx1 = std::min(int(vpX + vpW), int(std::ceil(dX(cx1))));
		const int syBot = std::max(int(vpY), int(std::floor(dY(cy1))));
		const int syTop = std::min(int(vpY + vpH), int(std::ceil(dY(cy0))));
		const int dx0 = int(std::floor(cx0)), dx1 = int(std::ceil(cx1));
		const int dy0 = int(std::floor(cy0)), dy1 = int(std::ceil(cy1));
		if (sx1 <= sx0 || syTop <= syBot)
			return;
		resolveDisplay(); // the tap reads the display, so multisampled passes have to land in it first
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst.fbo);
		glDisable(GL_SCISSOR_TEST); // else the blit is clipped by whatever scissor a prior draw left set
		glBindFramebuffer(GL_READ_FRAMEBUFFER, gl.displayFbo);
		// src top (syTop) -> dst bottom (dy0): the vertical flip that stores game-V=0 at capture y=0.
		glBlitFramebuffer(sx0, syTop, sx1, syBot, dx0, dy0, dx1, dy1, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		bindDisplayFbo(); // back to where the passes draw, which is not displayFbo under multisampling
	}

	// The frame so far, for a draw that bound the buffer being drawn into. Copies only the tap's UV
	// window, so a small tap skips most of the copy; a whole-screen tap clamps back to the full display.
	void Renderer::snapshotDisplayForFeedback(float u0, float v0, float u1, float v1)
	{
		CaptureGeometry g;
		if (!captureGeometry(g))
		{
			return;
		}
		ensureCaptureTarget(gl.capture, g, DRAW_BUFFER_FEEDBACK_TEX_ID, "gl.capture");
		blitDisplayRegion(gl.capture, g, u0, v0, u1, v1);
	}

	// The presented frame, for a draw that binds the displayed buffer. Whole region, since the sampling
	// draw comes a frame later and its UV window isn't known here; skipped until a draw asks for it.
	void Renderer::capturePreviousFrame()
	{
		if (!objects->prevFrameWanted.load())
		{
			return;
		}
		CaptureGeometry g;
		if (!captureGeometry(g))
		{
			return;
		}
		ensureCaptureTarget(gl.prevFrame, g, DISPLAY_BUFFER_FEEDBACK_TEX_ID, "gl.prevFrame");
		blitDisplayRegion(gl.prevFrame, g, 0.0f, 0.0f, 1.0f, 1.0f);
	}

	// Renders into multisampled storage that resolves into the target's own colorTex, so everything
	// downstream still reads colorTex and only this function knows the pass was multisampled.
	RenderTarget& Renderer::acquireTarget(uint32_t raster, int w, int h, int samples)
	{
		w = std::max(w, 1);
		h = std::max(h, 1);
		RenderTarget& t = gl.targets[raster];
		if (t.fbo && (t.w != w || t.h != h || t.samples != samples)) // resized or re-sampled: rebuild
		{
			glDeleteFramebuffers(1, &t.fbo);
			glDeleteTextures(1, &t.colorTex);
			glDeleteRenderbuffers(1, &t.depthRbo);
			if (t.flipFbo) glDeleteFramebuffers(1, &t.flipFbo);
			if (t.flipTex) glDeleteTextures(1, &t.flipTex);
			if (t.msFbo) glDeleteFramebuffers(1, &t.msFbo);
			if (t.msColor) glDeleteRenderbuffers(1, &t.msColor);
			if (t.msDepth) glDeleteRenderbuffers(1, &t.msDepth);
			t = RenderTarget{};
		}
		if (!t.fbo)
		{
			char label[40];
			std::snprintf(label, sizeof(label), "rt 0x%08X", raster);
			createColorDepthFbo(t.fbo, t.colorTex, t.depthRbo, w, h, label);
			t.w = w;
			t.h = h;
			t.samples = samples;
			if (samples > 1)
			{
				createMultisampleFbo(t.msFbo, t.msColor, t.msDepth, t.w, t.h, samples, label);
				if (!t.msFbo)
				{
					t.samples = 1;
				}
			}
		}
		return t;
	}

	// An enhancement may render this target larger than the guest asked for. acquireTarget rebuilds on
	// a size change, so the scale can move at runtime, and callers keep passing the guest's own size
	// for anything in its coordinate space (the Im2D extent): only the GL storage grew.
	void Renderer::bindOffscreenTarget(uint32_t rasterAddr, int w, int h)
	{
		const int scale = enhancements::offscreenTargetScale();
		RenderTarget& t = acquireTarget(rasterAddr, w * scale, h * scale,
			enhancements::offscreenSampleCount());
		glBindFramebuffer(GL_FRAMEBUFFER, t.msFbo ? t.msFbo : t.fbo);
		glViewport(0, 0, t.w, t.h);
		passColorTex = t.colorTex;
		passSamples = t.samples;
	}

	void Renderer::finishOffscreenTarget(uint32_t rasterAddr)
	{
		const auto it = gl.targets.find(rasterAddr);
		if (it != gl.targets.end())
		{
			RenderTarget& t = it->second;
			// A GL FBO is y-up; the game samples this raster with PS2 (y-down) UVs, so publish a
			// vertically-flipped copy (else a sampled RT like the shadow silhouette is upside-down).
			if (!t.flipTex)
			{
				glGenTextures(1, &t.flipTex);
				glBindTexture(GL_TEXTURE_2D, t.flipTex);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				glGenFramebuffers(1, &t.flipFbo);
				glBindFramebuffer(GL_FRAMEBUFFER, t.flipFbo);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.flipTex, 0);
			}
			// A multisampled read cannot be flipped in the same blit (the rectangles have to match), so
			// it resolves into colorTex first and the flip runs from there as usual.
			if (t.msFbo)
			{
				glBindFramebuffer(GL_READ_FRAMEBUFFER, t.msFbo);
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t.fbo);
				glBlitFramebuffer(0, 0, t.w, t.h, 0, 0, t.w, t.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			}
			glBindFramebuffer(GL_READ_FRAMEBUFFER, t.fbo);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t.flipFbo);
			glBlitFramebuffer(0, 0, t.w, t.h, 0, t.h, t.w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			textures->putTexture(rasterAddr | RENDER_TARGET_TEX_BIT, t.flipTex);
			static const bool sDumpRt = sky2::env("DUMP_RT") != nullptr;
			if (sDumpRt)
			{
				static std::unordered_set<uint32_t> sDumped;
				if (sDumped.insert(rasterAddr).second)
				{
					const int w = it->second.w, h = it->second.h;
					std::vector<uint8_t> px(size_t(w) * h * 4);
					glBindFramebuffer(GL_READ_FRAMEBUFFER, it->second.flipFbo); // the published (flipped) copy
					glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
					char path[128];
					std::snprintf(path, sizeof(path), "experiments/dumps/rt_%08X.pgm", rasterAddr);
					if (FILE* f = std::fopen(path, "wb"))
					{
						std::fprintf(f, "P5\n%d %d\n255\n", w, h); // alpha channel (shadow silhouette)
						for (int y = h - 1; y >= 0; y--)
							for (int x = 0; x < w; x++)
								std::fwrite(&px[(size_t(y) * w + x) * 4 + 3], 1, 1, f);
						std::fclose(f);
						std::fprintf(stderr, "[sky2] wrote %s (%dx%d)\n", path, w, h);
					}
				}
			}
		}
	}

	// A draw sampled an RT-tagged raster with no published FBO. Log (once per raster) whether we ever
	// built an off-screen FBO for it (gl.targets) and which RTs we did publish, to pin the cause:
	// never-rendered-this-session vs. classification/keying mismatch.
	// The frame as it stands after one draw, over a rect. Reads the resolved display FBO, so the
	// numbers line up with what a screenshot at that point would show.
	void Renderer::traceDrawRegion(int drawIndex)
	{
		if (!traceRegionArmed() || !traceRegionOut)
		{
			return;
		}
		const int x0 = std::max(0, traceRect[0]), y0 = std::max(0, traceRect[1]);
		const int x1 = std::min(gl.displayW, traceRect[2]), y1 = std::min(gl.displayH, traceRect[3]);
		if (x1 <= x0 || y1 <= y0)
		{
			return;
		}
		const GLint boundFbo = [] { GLint b = 0; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &b); return b; }();
		if (passIsDisplay)
		{
			resolveDisplay();
			glBindFramebuffer(GL_READ_FRAMEBUFFER, gl.displayFbo);
		}
		const int w = x1 - x0, h = y1 - y0;
		std::vector<uint8_t> px(size_t(w) * size_t(h) * 4);
		glReadPixels(x0, gl.displayH - y1, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
		double sum[4] = {0, 0, 0, 0};
		for (size_t i = 0; i + 3 < px.size(); i += 4)
			for (int k = 0; k < 4; k++)
				sum[k] += px[i + k];
		const double n = double(w) * double(h);
		std::fprintf(traceRegionOut, "{\"draw\":%d,\"rgba\":[%.2f,%.2f,%.2f,%.2f]}\n", drawIndex,
			sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n);
		glBindFramebuffer(GL_FRAMEBUFFER, GLuint(boundFbo));
	}

	void Renderer::logRtFboMiss(uint32_t texKey)
	{
		const uint32_t raster = texKey & ~RENDER_TARGET_TEX_BIT;
		std::string publishedRts;
		int nPublished = 0;
		for (const TextureId k : textures->renderTargetKeys())
		{
			if (nPublished++ < 8)
			{
				char b[16];
				std::snprintf(b, sizeof(b), "0x%X ", static_cast<unsigned>(k & ~RENDER_TARGET_TEX_BIT));
				publishedRts += b;
			}
		}
		std::string builtTargets;
		int nTargets = 0;
		for (const auto& [r, t] : gl.targets)
		{
			if (nTargets++ < 8)
			{
				char b[24];
				std::snprintf(b, sizeof(b), "0x%X(%dx%d) ", r, t.w, t.h);
				builtTargets += b;
			}
		}
		SKY2_TRACE_ONCE(raster,
			"RT FBO miss: raster=0x%X inTargets=%d inDisplaySet=%d inRTSet=%d | %d RTs built [%s] | %d published [%s]",
			raster, gl.targets.count(raster) ? 1 : 0, objects->displayRasters.count(raster) ? 1 : 0,
			objects->renderTargetRasters.count(raster) ? 1 : 0, nTargets, builtTargets.c_str(),
			nPublished, publishedRts.c_str());
	}

	void Renderer::blitDisplayToWindow(int winW, int winH)
	{
		resolveDisplay();
		glBindFramebuffer(GL_READ_FRAMEBUFFER, gl.displayFbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT); // in case the FBO is smaller than the window for a frame
		glBlitFramebuffer(0, 0, gl.displayW, gl.displayH, 0, 0, winW, winH,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	// One upload per frame for every skinned object, before any pass draws. A draw selects its own
	// object's matrices with uBoneOffset.
	void Renderer::uploadBones(const std::vector<glm::mat4>& bones)
	{
		if (bones.empty())
		{
			return; // no skinned object this frame; nothing indexes the buffer
		}
		const size_t bytes = bones.size() * sizeof(glm::mat4);
		if (bones.size() > gl.boneSSBOBones)
		{
			glNamedBufferData(gl.boneSSBO, static_cast<GLsizeiptr>(bytes), bones.data(), GL_DYNAMIC_DRAW);
			gl.boneSSBOBones = bones.size();
			return;
		}
		glNamedBufferSubData(gl.boneSSBO, 0, static_cast<GLsizeiptr>(bytes), bones.data());
	}


	// Bind the mesh's GPU-resident VAO (vertex buffer + attrib layout) and index buffer, uploading each
	// once on first sight and reusing it thereafter (across submeshes, passes and frames). Keyed by the
	// shared buffer identity, so a geometry shared by several atomics/passes uploads a single time.
	// The handle, not the slot, decides whether the resident copy is current: a recycled slot keeps its
	// GL objects but carries a new generation, so the contents are replaced rather than redrawn stale.
	void Renderer::bindMeshBuffers(const MeshDraw& m)
	{
		if (m.verts == GEOM_ID_NONE)
		{
			glBindVertexArray(gl.transientVao); // already uploaded for the whole frame
			return;
		}
		const uint32_t vslot = geomSlot(m.verts);
		if (gl.gpuGeom.size() <= vslot)
		{
			gl.gpuGeom.resize(vslot + 1);
		}
		GpuGeom& gv = gl.gpuGeom[vslot];
		const bool freshVao = gv.vao == 0;
		if (freshVao)
		{
			glGenVertexArrays(1, &gv.vao);
			glGenBuffers(1, &gv.vbo);
			bumpStat(geometry->stats.bufferCreates);
		}
		glBindVertexArray(gv.vao);
		gv.lastUsedFrame = gl.frameIndex;
		if (gv.id != m.verts)
		{
			const std::span<const Vertex3D> v = geometry->verts.view(m.verts);
			glBindBuffer(GL_ARRAY_BUFFER, gv.vbo);
			{
				ScopedNs timer{geometry->stats.uploadNs};
				glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(Vertex3D), v.data(), GL_STATIC_DRAW);
			}
			bumpStat(geometry->stats.vertUploads);
			bumpStat(geometry->stats.uploadBytes, v.size() * sizeof(Vertex3D));
			// Only for a new VAO: the attribute state records the buffer name and the Vertex3D format,
			// and glBufferData reallocating the store changes neither.
			if (freshVao)
			{
				setupVertex3DAttribs();
			}
			gv.id = m.verts;
		}
		const uint32_t islot = geomSlot(m.idx);
		if (gl.gpuIdx.size() <= islot)
		{
			gl.gpuIdx.resize(islot + 1);
		}
		GpuIndexBuf& gi = gl.gpuIdx[islot];
		if (!gi.ebo)
		{
			glGenBuffers(1, &gi.ebo);
			bumpStat(geometry->stats.bufferCreates);
		}
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gi.ebo); // records into gv.vao's element binding
		gi.lastUsedFrame = gl.frameIndex;
		if (gi.id != m.idx)
		{
			const std::span<const uint16_t> i = geometry->indices.view(m.idx);
			{
				ScopedNs timer{geometry->stats.uploadNs};
				glBufferData(GL_ELEMENT_ARRAY_BUFFER, i.size() * sizeof(uint16_t), i.data(), GL_STATIC_DRAW);
			}
			bumpStat(geometry->stats.idxUploads);
			bumpStat(geometry->stats.uploadBytes, i.size() * sizeof(uint16_t));
			gi.id = m.idx;
		}
	}

	// A stale handle means stale contents, so the entry drops its id and re-uploads on the next bind;
	// the GL objects go only once it has been unbound for a while. Freeing on the stale handle instead
	// deleted and recreated a VAO per Im3D batch every frame, stalling on work still in flight.
	// Runs at end-of-frame, where the just-drawn frame still pins its geometry.
	void Renderer::evictUnusedGeom()
	{
		constexpr uint64_t IDLE_FRAMES = 600; // ~10 seconds, as for the texture content cache
		// app::gpuResident off (debug) drops every id each frame, so geometry re-uploads every frame.
		const bool resident = sky2::config().gpuResident;
		const uint64_t now = gl.frameIndex;
		const auto idle = [&](uint64_t lastUsedFrame) { return now - lastUsedFrame >= IDLE_FRAMES; };
		for (GpuGeom& g : gl.gpuGeom)
		{
			if (!g.vao)
			{
				continue;
			}
			if (!resident || !geometry->verts.alive(g.id))
			{
				g.id = GEOM_ID_NONE;
			}
			if (idle(g.lastUsedFrame))
			{
				glDeleteVertexArrays(1, &g.vao);
				glDeleteBuffers(1, &g.vbo);
				g = GpuGeom{};
				bumpStat(geometry->stats.evictions);
			}
		}
		for (GpuIndexBuf& i : gl.gpuIdx)
		{
			if (!i.ebo)
			{
				continue;
			}
			if (!resident || !geometry->indices.alive(i.id))
			{
				i.id = GEOM_ID_NONE;
			}
			if (idle(i.lastUsedFrame))
			{
				glDeleteBuffers(1, &i.ebo);
				i = GpuIndexBuf{};
				bumpStat(geometry->stats.evictions);
			}
		}
	}

	// Apply a mesh's RwCullMode. The projection negates X (un-mirror), which flips triangle winding,
	// so RW's front faces are counter-clockwise in GL's post-projection space.
	void applyCull(int cull)
	{
		if (cull == rwCULLMODECULLBACK || cull == rwCULLMODECULLFRONT)
		{
			glEnable(GL_CULL_FACE);
			glFrontFace(GL_CCW);
			glCullFace(cull == rwCULLMODECULLBACK ? GL_BACK : GL_FRONT);
		}
		else
		{
			SKY2_ERROR(cull == rwCULLMODECULLNONE,
				"unhandled RwCullMode %d", cull);
			glDisable(GL_CULL_FACE);
		}
	}

	void Renderer::drawOneMesh(const Frame& frame, const MeshDraw& m, const glm::mat4& vp, uint32_t passIndex)
	{
		// Resolved once; the draw carries indices into the frame's deduped tables.
		const DrawState& gs = frame.states[m.stateIndex];
		const FrameLights& meshLights = frame.lights[m.lightsIndex];
		bindMeshBuffers(m); // resident VAO/VBO/EBO, uploaded once per geometry
		// The model is the frame's own unless interpolation is on and this draw matched last tick.
		const glm::mat4 mvp = vp * interp.modelFor(m, passIndex, m.model);
		// A pool mesh owns its buffers and draws all of them; an arena mesh draws its slice, with the
		// base vertex making its mesh-local indices absolute. Base 0 at offset 0 is the pool case, so
		// one call serves both.
		const bool arenaMesh = m.verts == GEOM_ID_NONE;
		const GLsizei idxCount = static_cast<GLsizei>(
			arenaMesh ? m.idxCount : geometry->indices.view(m.idx).size());
		const void* idxOffset = reinterpret_cast<void*>(
			static_cast<uintptr_t>(arenaMesh ? m.idxOffset * sizeof(uint16_t) : 0));
		const GLint baseVertex = arenaMesh ? static_cast<GLint>(m.vertOffset) : 0;
		const auto drawIndexed = [&] {
			glDrawElementsBaseVertex(m.lineList ? GL_LINES : GL_TRIANGLES, idxCount, GL_UNSIGNED_SHORT,
				idxOffset, baseVertex);
		};

		// Resolve the base texture; a bound texture selects the TEXTURED variant.
		const GLuint baseTex = m.material.texKey ? textures->lookup(*m.material.texKey) : 0;
		// Sampling comes from a sampler object for decoded textures, which content keying shares between
		// rasters that may want different filtering. Render targets and feedback copies are excluded:
		// the renderer created those with their own clamp, and a sampler object would override it.
		glBindSampler(0, baseTex && !isRendererOwnedTexId(*m.material.texKey)
							 ? textures->samplerFor(m.material.sampler, magnifying())
							 : 0);
		const bool textured = baseTex != 0;
		if (!textured && m.material.texKey)
		{
			if (isRenderTargetTexId(*m.material.texKey))
			{
				objects->rtSampleWanted.store(true); // RT with no FBO: let the host auto-capture (P3FES_RT_AUTOCAP)
				logRtFboMiss(*m.material.texKey);
			}
			SKY2_ERROR(false, "texKey 0x%016llX not in the texture cache",
				static_cast<unsigned long long>(*m.material.texKey));
		}
		const Prog3d& p = shaders->getProg3d(shaders->variantFor(m, gs, textured, false));
		if (!p.prog)
		{
			return; // shader failed to compile (e.g. a bad hot-reload edit): skip this mesh, don't crash
		}
		glUseProgram(p.prog);
		shaders->setProg3dUniforms(p, m, gs, meshLights, mvp);
		if (p.uFarPlane >= 0)
		{
			// Only a perspective camera; the parallel one (the shadow light) runs a different VU1
			// transform, and a zero leaves the rejection off.
			const CameraParts& cam = frame.passes[passIndex].cam;
			const bool cull = cam.valid && cam.projectionType == rwPERSPECTIVE;
			glUniform1f(p.uFarPlane, cull ? cam.farPlane : 0.0f);
		}
		// Per-mesh depth from ZTESTENABLE/ZWRITEENABLE. GL ties writes to GL_DEPTH_TEST, so to
		// write depth without testing (ztest off, zwrite on) keep the test enabled with GL_ALWAYS.
		if (gs.ztest || gs.zwrite)
		{
			glEnable(GL_DEPTH_TEST);
			// RW hardcodes LESSEQUAL; equal-depth re-draws (decals, detail overlays, seams) must pass.
			glDepthFunc(gs.ztest ? GL_LEQUAL : GL_ALWAYS);
		}
		else
		{
			glDisable(GL_DEPTH_TEST);
		}
		glDepthMask(gs.zwrite ? GL_TRUE : GL_FALSE);
		glColorMask(gs.colorMask.r, gs.colorMask.g, gs.colorMask.b, gs.colorMask.a);
		if (gs.clamp.active) // mdlEffect sets CLAMP_1 around its own Im2D draws, so a mesh seeing one
		{ // means the set/restore pair straddled a frame boundary, or another path pokes the register
			SKY2_TRACE_ONCE(gs.clamp.reg,
				"3D mesh drawn with GS CLAMP_1 0x%llX active; the region clamp is Im2D-only",
				static_cast<unsigned long long>(gs.clamp.reg));
		}
		applyCull(gs.cull);
		sky2::gl::applyStencil(gs.stencil);
		if (textured)
		{
			glBindTexture(GL_TEXTURE_2D, baseTex);
		}
		static const bool sDateEnabled = sky2::env("NO_DATE") == nullptr;
		const bool dateActive = sDateEnabled && gs.atest.date && passSamples == 1 && passColorTex != 0;
		if (gs.atest.date && passSamples != 1)
		{
			SKY2_TRACE_ONCE(passSamples, "GS DATE active on a %dx multisampled pass; skipped (needs "
				"a single-sample target)", passSamples);
		}
		glUniform1i(p.uDateMode, dateActive ? (gs.atest.datm ? 2 : 1) : 0);
		if (dateActive)
		{
			glTextureBarrier();
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, passColorTex);
			glActiveTexture(GL_TEXTURE0);
		}
		// GS alpha test with AFAIL != KEEP: a passing pixel writes full color+Z, a failing pixel writes a
		// mode-specific subset. GL can't vary write masks per fragment, so split into two masked draws:
		// one keeps only passing pixels (uAtestMode = method), the other keeps all (ATST_ALWAYS).
		static const bool sAfailSplit = sky2::env("NO_AFAIL_SPLIT") == nullptr;
		if (gs.atest.enabled && gs.atest.afail != AFAIL_KEEP && sAfailSplit)
		{
			const auto& cm = gs.colorMask;
			const GLboolean zw = gs.zwrite ? GL_TRUE : GL_FALSE;
			auto drawAll = [&]() { glUniform1i(p.uAtestMode, ATST_ALWAYS); drawIndexed(); };
			auto drawPass = [&]() { // draw only pixels that pass the alpha test
				glUniform1i(p.uAtestMode, gs.atest.method);
				glUniform1f(p.uAtestRef, gs.atest.ref);
				drawIndexed();
			};
			switch (gs.atest.afail)
			{
				case AFAIL_FB_ONLY: // fail keeps color, drops Z
					glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE); // Z for passing pixels
					glDepthMask(zw);
					drawPass();
					glColorMask(cm.r, cm.g, cm.b, cm.a); // color for every pixel, no Z
					glDepthMask(GL_FALSE);
					drawAll();
					break;
				case AFAIL_ZB_ONLY: // fail keeps Z, drops color
					glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE); // Z for every pixel
					glDepthMask(zw);
					drawAll();
					glColorMask(cm.r, cm.g, cm.b, cm.a); // color for passing pixels, no Z
					glDepthMask(GL_FALSE);
					drawPass();
					break;
				case AFAIL_RGB_ONLY: // fail keeps RGB, drops alpha and Z
					glColorMask(cm.r, cm.g, cm.b, GL_FALSE); // RGB for every pixel, no alpha, no Z
					glDepthMask(GL_FALSE);
					drawAll();
					glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, cm.a); // alpha + Z for passing pixels
					glDepthMask(zw);
					drawPass();
					break;
			}
			// Restore the normal per-mesh masks so the MatFX dual pass and next mesh see consistent state.
			glColorMask(cm.r, cm.g, cm.b, cm.a);
			glDepthMask(zw);
			glUniform1i(p.uAtestMode, ATST_ALWAYS);
		}
		else
		{
			// AFAIL=KEEP (or no test at all): one draw, with the register's own test as a discard.
			// The 2D path has always done this; 3D used to substitute a fixed GEQUAL 127 in the shader.
			glUniform1i(p.uAtestMode, gs.atest.on ? gs.atest.method : ATST_ALWAYS);
			glUniform1f(p.uAtestRef, gs.atest.ref);
			drawIndexed();
		}

		// Packed filterAddressing for the reflection pass, matching what the driver puts in TEX1_2 and
		// CLAMP_2: trilinear minification, wrap on both axes.
		constexpr unsigned ENV_SAMPLER =
			rwFILTERLINEARMIPLINEAR | (rwTEXTUREADDRESSWRAP << 8) | (rwTEXTUREADDRESSWRAP << 12);

		// MatFX second pass (RW 8.9): the geometry again through GS context 2, either the dual texture
		// on set-1 UVs or an envmap reflection. Depth test LEQUAL, no depth write. It is a different
		// shader variant, so setProg3dUniforms below re-sends uBoneOffset for it.
		const GLuint pass2Tex = m.matfx.hasPass2 && m.matfx.pass2TexKey ? textures->lookup(*m.matfx.pass2TexKey) : 0;
		if (pass2Tex)
		{
			const Prog3d& pd = shaders->getProg3d(shaders->variantFor(m, gs, true, true)); // always textured
			if (!pd.prog)
			{
				return; // shader failed to compile: skip the second pass rather than crash
			}
			const GsBlend& pb = m.matfx.pass2Blend;
			const GLboolean wasBlend = glIsEnabled(GL_BLEND);
			glUseProgram(pd.prog);
			shaders->setProg3dUniforms(pd, m, gs, meshLights, mvp);
			// setProg3dUniforms sends the base pass's values for both; ALPHA_2 is a separate register.
			glUniform1f(pd.uAlphaPremul, pb.alphaPremul);
			if (m.matfx.pass2Env)
			{
				glUniformMatrix3fv(pd.uEnvView, 1, GL_FALSE,
					glm::value_ptr(glm::mat3(glm::inverse(frame.passes[passIndex].cam.camWorld))));
				glUniform1f(pd.uEnvCoef, m.matfx.pass2Coef);
				// The reflection needs its own sampler rather than the base pass's: TEX1_2 programs
				// MMIN=LINEAR_MIPMAP_LINEAR where the base texture is MXL=0/LINEAR, and a non-mipmap
				// min filter pins the shader's explicit textureLod to level 0. CLAMP_2 is REPEAT.
				glBindSampler(0, textures->samplerFor(ENV_SAMPLER, false));
			}
			glBindTexture(GL_TEXTURE_2D, pass2Tex);
			glEnable(GL_BLEND);
			glBlendEquationSeparate(pb.equation, GL_FUNC_ADD);
			glBlendFuncSeparate(pb.src, pb.dst, GL_ONE, GL_ZERO); // alpha: straight source copy
			glBlendColor(0.0f, 0.0f, 0.0f, pb.constAlpha); // only read when a factor is CONSTANT_ALPHA
			glDepthMask(GL_FALSE);
			glDepthFunc(GL_LEQUAL);
			drawIndexed();
			glBlendEquation(GL_FUNC_ADD); // the caller only re-sets this for a blended mesh
			if (!wasBlend)
			{
				glDisable(GL_BLEND); // opaque partition draws with blend off; don't leak it
			}
		}
	}

	void Renderer::drawMeshes(const Frame& frame, std::span<const uint32_t> order, const FrameLights& lights,
		const glm::mat4& vp, uint32_t passIndex)
	{
		if (order.empty())
		{
			return; // as before: a 2D-only pass runs neither the prologue nor the epilogue below
		}
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LEQUAL); // RW LESSEQUAL; per-mesh drawOneMesh overrides this
		// Pass-scope shading state, now set per program in drawOneMesh (each variant is its own
		// program) rather than once here.
		static const bool sLogFog = sky2::env("LOG_FOG") != nullptr;
		if (sLogFog)
		{
			static bool sLoggedFog = false;
			if (!sLoggedFog)
			{
				std::fprintf(stderr, "[sky2][fog] color=(%.2f,%.2f,%.2f) start=%.1f end=%.1f "
									 "ambient=(%.2f,%.2f,%.2f) dirCount=%d dir0=(%.2f,%.2f,%.2f)\n",
					lights.fogColor[0], lights.fogColor[1], lights.fogColor[2], lights.fogStart, lights.fogEnd,
					lights.ambient[0], lights.ambient[1], lights.ambient[2], lights.dirCount,
					lights.dirColor[0][0], lights.dirColor[0][1], lights.dirColor[0][2]);
				sLoggedFog = true;
			}
		}
		glActiveTexture(GL_TEXTURE0); // each drawOneMesh binds its geometry's resident VAO

		// drawOrder already put the opaque meshes ahead of the translucent ones, so alpha-blended
		// surfaces still composite over the solid scene. Per-mesh depth test/write come from
		// ZTESTENABLE/ZWRITEENABLE (drawOneMesh). A draw's id is its position in the command list.
		//
		// The disable must happen before the first draw, not lazily at the partition boundary: the
		// MatFX dual pass reads glIsEnabled(GL_BLEND) and only re-disables when it was off, so an
		// opaque mesh reached with blend still enabled would composite its dual overlay instead of
		// replacing it.
		glDisable(GL_BLEND);
		bool blendOn = false;
		for (const uint32_t ci : order)
		{
			const FrameCmd& cmd = frame.commandList[ci];
			const MeshDraw& m = frame.meshCmds[cmd.data];
			const Blend& gb = frame.states[m.stateIndex].blend;
			if (gb.enable && !blendOn)
			{
				glEnable(GL_BLEND);
				blendOn = true;
			}
			if (!drawEnabled(static_cast<int>(ci)))
			{
				continue;
			}
			if (gb.enable)
			{
				// Full reconciled GS blend (equation/factors/const), same as the 2D path. Alpha channel
				// = straight source-alpha copy (GS writes it unblended); see Im2DRenderer::draw.
				glBlendEquationSeparate(gb.equation, GL_FUNC_ADD);
				glBlendFuncSeparate(gb.src, gb.dst, GL_ONE, GL_ZERO);
				glBlendColor(0.0f, 0.0f, 0.0f, gb.constAlpha); // only used when src/dst reference CONSTANT_ALPHA
			}
			drawOneMesh(frame, m, vp, passIndex);
			traceDrawRegion(static_cast<int>(ci));
		}
		if (!blendOn)
		{
			glEnable(GL_BLEND); // the old code enabled it unconditionally between the two loops
		}
		glBlendEquation(GL_FUNC_ADD); // restore so a subtractive mesh doesn't leak into the next pass
		glDisable(GL_BLEND);
		glDisable(GL_CULL_FACE);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_STENCIL_TEST); // a mesh may have left the stencil test on
		glStencilMask(0xFF); // restore full stencil writes for the next frame's clear
		glDepthMask(GL_TRUE); // restore so the next frame's depth clear works
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); // a mesh may have masked RGB (mdlEffect)
	}
} // namespace sky2
