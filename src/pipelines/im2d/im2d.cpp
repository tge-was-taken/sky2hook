// Im2D pipeline (2D immediate primitives: UI text, sprites, fades: the RwIm2DRender* funnel). Two
// halves: the build-thread funnel (readVertex/beginDraw parse into an Im2DDraw), and the
// render-thread GL (Im2DRenderer compiles the program + VAO and submits the batches). The build
// half consumes the device services through its Device; the render half takes the Renderer it draws
// with, plus the stateless GL helpers from gl.h.
#include "pipelines/im2d/im2d.h"

#include "device.h"
#include "common.h"
#include "enhancements/feedback_blur.h"
#include "gl.h"
#include "object_manager.h"
#include "render_queue.h"
#include "renderer.h"
#include "rw_to_gl.h"
#include "texture_manager.h"

#include "profile.h" // P3FES_PROFILE phase timing
#include "shader_manager.h" // getShaderProgram

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace sky2
{
	Im2DVertex Im2DPipeline::readVertex(uint32_t vertsAddr, int i)
	{
		const RwIm2DVertex& src = *guest::ptr<RwIm2DVertex>(
			vertsAddr + static_cast<uint32_t>(i) * sizeof(RwIm2DVertex));
		return {src.x, src.y, src.z, src.u, src.v,
			src.red / 255.0f, src.green / 255.0f, src.blue / 255.0f, src.alpha / 255.0f};
	}

	void Im2DPipeline::beginDraw(Im2DDraw& d)
	{
		RenderQueue& queue = device->queue;
		d.sampler = device->im2dFilterAddressing();
		d.texKey = device->textures.convertRaster(queue.renderState[rwRENDERSTATETEXTURERASTER], d.sampler);
		// Current GS blend + alpha test, set by the RW SRCBLEND/DESTBLEND states or the direct
		// RpSkyRenderStateSet(ALPHA_1/ATEST_1) path (blendState() resolves the two, GS wins).
		DrawState st;
		st.blend = queue.blendState();
		st.stencil = queue.stencil;
		st.atest = queue.gsAtest;
		st.colorMask = queue.colorMask; // mdlEffect mask pass restricts which channels are written
		st.clamp = queue.gsClamp;
		st.gsAlphaReg = queue.gsBlend.reg.U64;
		// 2D shares the GS depth test with 3D, and games clip UI panels by writing a Z barrier and
		// drawing the contents behind it. Same unhit-latch fallback as the 3D path.
		st.ztest = queue.rsHits[rwRENDERSTATEZTESTENABLE] ? queue.renderState[rwRENDERSTATEZTESTENABLE] != 0 : true;
		st.zwrite = queue.rsHits[rwRENDERSTATEZWRITEENABLE] ? queue.renderState[rwRENDERSTATEZWRITEENABLE] != 0 : true;
		d.stateIndex = queue.stateIndexFor(st);
		d.vertOffset = static_cast<uint32_t>(queue.buildFrame().im2dVerts.size());
		d.vertCount = 0;
	}

	void Im2DPipeline::pushVertex(Frame& f, Im2DDraw& d, const Im2DVertex& q)
	{
		f.im2dVerts.push_back(q);
		d.vertCount++;
	}

	void Im2DPipeline::pushLineQuad(Frame& f, Im2DDraw& d, const Im2DVertex& a, const Im2DVertex& b)
	{
		float nx = b.y - a.y, ny = a.x - b.x; // perpendicular to the segment
		const float len = std::sqrt(nx * nx + ny * ny);
		if (len < 1e-4f)
		{
			return;
		}
		const float halfWidth = 0.5f;
		nx = nx / len * halfWidth;
		ny = ny / len * halfWidth;
		Im2DVertex a0 = a, a1 = a, b0 = b, b1 = b;
		a0.x = a.x + nx;
		a0.y = a.y + ny;
		a1.x = a.x - nx;
		a1.y = a.y - ny;
		b0.x = b.x + nx;
		b0.y = b.y + ny;
		b1.x = b.x - nx;
		b1.y = b.y - ny;
		pushVertex(f, d, a0);
		pushVertex(f, d, a1);
		pushVertex(f, d, b0);
		pushVertex(f, d, a1);
		pushVertex(f, d, b1);
		pushVertex(f, d, b0);
	}

	void Im2DPipeline::renderPrimitive(uint32_t primType, uint32_t vertsAddr, int numVerts)
	{
		if (!vertsAddr || numVerts <= 0 || numVerts > 4096)
		{
			SKY2_ERROR(false, "bad args (verts=0x%X numVerts=%d)", vertsAddr, numVerts);
			return;
		}
		std::vector<Im2DVertex> vs(static_cast<size_t>(numVerts));
		for (int i = 0; i < numVerts; i++)
		{
			vs[i] = readVertex(vertsAddr, i);
		}
		Frame& f = device->queue.buildFrame();
		Im2DDraw d;
		beginDraw(d);
		d.srcPrim = static_cast<uint16_t>(primType);
		d.srcVerts = static_cast<uint16_t>(numVerts);
		if (isLinePrimitive(primType))
		{
			expandLinePrimitive(primType, numVerts, [&](int i0, int i1) {
				pushLineQuad(f, d, vs[static_cast<size_t>(i0)], vs[static_cast<size_t>(i1)]);
			});
		}
		else
		{
			expandTrianglePrimitive("Im2D", primType, numVerts,
				[&](int i) { pushVertex(f, d, vs[static_cast<size_t>(i)]); });
		}
		if (d.vertCount != 0)
		{
			device->queue.submitIm2D(std::move(d));
		}
	}

	void Im2DPipeline::renderTriangle(uint32_t vertsAddr, int numVerts, int vert1, int vert2, int vert3)
	{
		if (!vertsAddr || numVerts <= 0 || numVerts > 4096)
		{
			SKY2_ERROR(false, "bad args (verts=0x%X numVerts=%d)", vertsAddr, numVerts);
			return;
		}
		const int idx[3] = {vert1, vert2, vert3};
		for (const int vi : idx)
		{
			if (vi < 0 || vi >= numVerts)
			{
				SKY2_ERROR(false, "Im2D triangle index %d out of range (numVerts=%d)", vi, numVerts);
				return;
			}
		}
		Frame& f = device->queue.buildFrame();
		Im2DDraw d;
		beginDraw(d);
		d.srcPrim = rwPRIMTYPETRILIST;
		d.srcVerts = 3;
		for (const int vi : idx)
		{
			pushVertex(f, d, readVertex(vertsAddr, vi));
		}
		device->queue.submitIm2D(std::move(d));
	}

	void Im2DPipeline::renderIndexedPrimitive(uint32_t primType, uint32_t vertsAddr, int numVerts,
		uint32_t indicesAddr, int numIndices)
	{
		if (!vertsAddr || !indicesAddr || numVerts <= 0 || numVerts > 4096 ||
			numIndices <= 0 || numIndices > 8192)
		{
			SKY2_ERROR(false, "bad args (verts=0x%X idx=0x%X nv=%d ni=%d)",
				vertsAddr, indicesAddr, numVerts, numIndices);
			return;
		}
		std::vector<Im2DVertex> vs(static_cast<size_t>(numVerts));
		for (int i = 0; i < numVerts; i++)
		{
			vs[i] = readVertex(vertsAddr, i);
		}
		std::vector<uint16_t> indices(static_cast<size_t>(numIndices));
		const auto idxBuf = guest::ptr<uint16_t>(indicesAddr);
		for (int i = 0; i < numIndices; i++)
		{
			indices[static_cast<size_t>(i)] = idxBuf[i];
		}
		Frame& f = device->queue.buildFrame();
		Im2DDraw d;
		beginDraw(d);
		d.srcPrim = static_cast<uint16_t>(primType);
		d.srcVerts = static_cast<uint16_t>(numVerts);
		d.srcIndices = static_cast<uint16_t>(numIndices);
		if (isLinePrimitive(primType))
		{
			expandLinePrimitive(primType, numIndices, [&](int k0, int k1) {
				const uint16_t v0 = indices[static_cast<size_t>(k0)];
				const uint16_t v1 = indices[static_cast<size_t>(k1)];
				if (v0 >= numVerts || v1 >= numVerts)
				{
					SKY2_ERROR(false, "Im2D indexed line vertex (%u,%u) out of range (numVerts=%d)",
						v0, v1, numVerts);
					return;
				}
				pushLineQuad(f, d, vs[v0], vs[v1]);
			});
			if (d.vertCount != 0)
			{
				device->queue.submitIm2D(std::move(d));
			}
			return;
		}
		// expandTrianglePrimitive always calls emit() 3 times per triangle (one per corner, for every
		// topology). Buffer a triangle's 3 corners and only push all 3 if every index is in range:
		// dropping just the bad corner leaves d.verts not divisible by 3,
		// misaligning every triangle from that point on.
		std::array<Im2DVertex, 3> pending{};
		int pendingCount = 0;
		bool pendingBad = false;
		expandTrianglePrimitive("Im2D", primType, numIndices, [&](int i) {
			const int vi = indices[static_cast<size_t>(i)];
			if (vi < 0 || vi >= numVerts)
			{
				SKY2_ERROR(false, "Im2D indexed vertex %d out of range (numVerts=%d)", vi, numVerts);
				pendingBad = true;
			}
			else if (!pendingBad)
			{
				pending[static_cast<size_t>(pendingCount)] = vs[static_cast<size_t>(vi)];
			}
			if (++pendingCount == 3)
			{
				if (!pendingBad)
				{
					for (const Im2DVertex& c : pending)
					{
						pushVertex(f, d, c);
					}
				}
				pendingCount = 0;
				pendingBad = false;
			}
		});
		if (d.vertCount != 0)
		{
			device->queue.submitIm2D(std::move(d));
		}
	}

	void Im2DPipeline::renderLine(uint32_t vertsAddr, int numVerts, int vert1, int vert2)
	{
		if (!vertsAddr || numVerts <= 0 || numVerts > 4096)
		{
			SKY2_ERROR(false, "bad args (verts=0x%X numVerts=%d)", vertsAddr, numVerts);
			return;
		}
		if (vert1 < 0 || vert1 >= numVerts || vert2 < 0 || vert2 >= numVerts)
		{
			SKY2_ERROR(false, "Im2D line index out of range (%d,%d numVerts=%d)", vert1, vert2, numVerts);
			return;
		}
		Frame& f = device->queue.buildFrame();
		Im2DDraw d;
		beginDraw(d);
		pushLineQuad(f, d, readVertex(vertsAddr, vert1), readVertex(vertsAddr, vert2));
		if (d.vertCount != 0)
		{
			device->queue.submitIm2D(std::move(d));
		}
	}

	// The GL state Im2DRenderer::draw sets per batch, tracked so an unchanged value costs nothing. `set`
	// is false until the first batch writes it, since the 3D passes and FBO binds between draw calls
	// change this state without us seeing it.
	struct Im2dGlState
	{
		bool set = false;
		GLuint tex = 0;
		bool textured = false;
		Blend blend;
		int atestMode = -1;
		float atestRef = -1.0f, alphaPremul = -1.0f;
		glm::bvec4 colorMask{false};
		int depthFunc = 0; // 0 until the first batch sets one
		bool depthWrite = false;
		glm::vec4 uvClamp{2.0f}; // no batch can want this, so the first one always uploads
		int ignoreTexAlpha = -1;
		StencilState stencil;
	};

	// ---- render thread: the Im2D GL program + draw ------------------------------------------------
	// State lives in GlState::im2d so it is created and freed with the rest of the render-thread GL.

	// Split from init so a shader reload can re-run it: the VAO/VBO below outlive the program.
	void Im2DRenderer::initProgram(Renderer& renderer)
	{
		// im2d.frag uses the shared GS chunk (PS2_UNIT + GsFrag uGs + ps2AtestDiscards), injected here.
		prog = renderer.shaders->getShaderProgram("2d.vert", "im2d.frag"); // GS bits via im2d.frag's #includes
		shaderGeneration = renderer.shaders->shaderGeneration();
		if (!prog)
		{
			return; // a bad edit reloaded: draw() skips the pass until the next reload fixes it
		}
		uProj = glGetUniformLocation(prog, "uProj");
		uTex = glGetUniformLocation(prog, "uTex");
		uTextured = glGetUniformLocation(prog, "uTextured");
		uAtestMode = glGetUniformLocation(prog, "uGs.atestMode");
		uAtestRef = glGetUniformLocation(prog, "uGs.atestRef");
		uAlphaPremul = glGetUniformLocation(prog, "uGs.alphaPremul");
		uUvClamp = glGetUniformLocation(prog, "uUvClamp");
		uIgnoreTexAlpha = glGetUniformLocation(prog, "uIgnoreTexAlpha");
		feedbackBlurRemoval.init(prog);
		uZMax = glGetUniformLocation(prog, "uZMax");
		SKY2_ASSERT(uProj != -1, "uProj uniform not found");
		SKY2_ASSERT(uAtestMode != -1, "uAtestMode uniform not found");
		SKY2_ASSERT(uAtestRef != -1, "uAtestRef uniform not found");
		SKY2_ASSERT(uAlphaPremul != -1, "uAlphaPremul uniform not found");
	}

	void Im2DRenderer::init(Renderer& renderer)
	{
		initProgram(renderer);
		glGenVertexArrays(1, &vao);
		glGenBuffers(1, &vbo);
		glBindVertexArray(vao);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		gl::labelObject(GL_VERTEX_ARRAY, vao, "im2d.vao");
		gl::labelObject(GL_BUFFER, vbo, "im2d.vbo");
		const GLsizei st = sizeof(Im2DVertex);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(offsetof(Im2DVertex, x)));
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(offsetof(Im2DVertex, u)));
		glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(offsetof(Im2DVertex, r)));
		glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(offsetof(Im2DVertex, z)));
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
		glEnableVertexAttribArray(2);
		glEnableVertexAttribArray(3);
		gl::checkGlErrors("Im2DRenderer::init");
	}

	// One upload for the whole frame, before any pass draws. Vertex data does not depend on what has
	// been rendered, so hoisting it changes no draw's inputs or order.
	void Im2DRenderer::uploadVerts(Renderer& renderer, const Frame& frame)
	{
		if (frame.im2dVerts.empty())
		{
			return;
		}
		const std::vector<Im2DVertex>& verts = renderer.interp.im2dVertsFor(frame);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(Im2DVertex)),
			verts.data(), GL_STREAM_DRAW);
	}

	void Im2DRenderer::draw(Renderer& renderer, const Frame& frame, std::span<const uint32_t> order,
		float projW, float projH)
	{
		if (order.empty())
		{
			return;
		}
		if (shaderGeneration != renderer.shaders->shaderGeneration())
		{
			initProgram(renderer); // F5 dropped our program; re-fetch it and its uniform locations
		}
		if (!prog)
		{
			return; // shader edit failed to compile; nothing to draw with until the next reload
		}
		const float gameResW = projW > 0.0f ? projW : renderer.gameResW.load();
		const float gameResH = projH > 0.0f ? projH : renderer.gameResH.load();
		if (gameResW <= 0.0f || gameResH <= 0.0f)
		{
			SKY2_ERROR(false, "invalid game resolution %gx%g", gameResW, gameResH);
			return;
		}
		// Depth is per batch now; the state cache below owns GL_DEPTH_TEST for the whole loop.
		glDisable(GL_DEPTH_TEST);
		glDepthMask(GL_FALSE);
		glUseProgram(prog);
		const float L = 0, R = gameResW, T = 0, B = gameResH;
		const float proj[16] = {2.0f / (R - L), 0, 0, 0, 0, 2.0f / (T - B), 0, 0, 0, 0, -1, 0,
			-(R + L) / (R - L), -(T + B) / (T - B), 0, 1};
		glUniformMatrix4fv(uProj, 1, GL_FALSE, proj);
		glUniform1i(uTex, 0);
		// PSMZ16S, which is what every traced scene across the four discs uses. A disc with a wider Z
		// buffer would pin every 2D draw to the near plane, which is the no-depth behaviour, so the
		// failure is inert rather than wrong.
		glUniform1f(uZMax, sky2::config().depth2d ? 65535.0f : 0.0f);
		glBindVertexArray(vao);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		Im2dGlState cur;
		for (size_t bi = 0; bi < order.size(); bi++)
		{
			if (!drawEnabled(static_cast<int>(order[bi])))
			{
				continue;
			}
			const Im2DDraw& b = frame.im2dCmds[frame.commandList[order[bi]].data];
			const DrawState& bgs = frame.states[b.stateIndex];
			// Framebuffer feedback: snapshot the display now, capturing everything rendered up to this
			// draw (the full scene, all prior passes and batches), so the effect samples the finished
			// image rather than a partial one. Only the tap's UV window needs refreshing (vertex layout
			// x,y,u,v,...; u,v at offsets 2,3), so pass its bbox. Leaves the display FBO bound.
			const bool destReadable = renderer.passSamples == 1 && renderer.passColorTex != 0;
			const TextureId feedbackKey =
				b.texKey ? feedbackBlurRemoval.sourceFor(*b.texKey) : TextureId{0};
			// P3FES_NO_FEEDBACK: skip the copy to measure what the feedback dependency costs. The
			// effects then sample a stale capture, so the frame is wrong; measurement only.
			static const bool sNoFeedback = sky2::env("NO_FEEDBACK") != nullptr;
			if (b.texKey && feedbackKey == DRAW_BUFFER_FEEDBACK_TEX_ID && !sNoFeedback)
			{
				float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
				for (uint32_t vi = 0; vi < b.vertCount; vi++)
				{
					const Im2DVertex& q = frame.im2dVerts[b.vertOffset + vi];
					const float u = q.u, v = q.v;
					u0 = std::min(u0, u);
					u1 = std::max(u1, u);
					v0 = std::min(v0, v);
					v1 = std::max(v1, v);
				}
				if (u1 >= u0)
					renderer.snapshotDisplayForFeedback(u0, v0, u1, v1);
				else
					renderer.snapshotDisplayForFeedback();
			}
			const GLuint tex = b.texKey ? renderer.textures->lookup(feedbackKey) : 0;
			feedbackBlurRemoval.applyRescale(b.texKey ? *b.texKey : TextureId{0}, bgs.blend,
				destReadable, renderer.passColorTex);
			glBindSampler(0, tex && !isRendererOwnedTexId(*b.texKey) ? renderer.textures->samplerFor(b.sampler, renderer.magnifying()) : 0);
			const bool textured = tex != 0;
			if (b.texKey && !textured)
			{
				// The feedback ids carry the render-target bit too, so check them first. A missing copy
				// means the first frame, before there is anything to sample.
				if (isFrameBufferFeedbackTexId(*b.texKey))
				{
					continue;
				}
				if (isRenderTargetTexId(*b.texKey))
				{
					renderer.objects->rtSampleWanted.store(true); // RT with no FBO: let the host auto-capture (P3FES_RT_AUTOCAP)
					renderer.logRtFboMiss(*b.texKey);
					// Drawing it untextured samples flat white, which under an additive blend whites out
					// the screen. Sampling nothing is the honest degradation for a target we don't have.
					continue;
				}
				SKY2_ERROR(false, "texKey 0x%016llX not in texture cache",
					static_cast<unsigned long long>(*b.texKey));
			}
			if (!cur.set || cur.textured != textured)
			{
				glUniform1f(uTextured, textured ? 1.0f : 0.0f);
				cur.textured = textured;
			}
			if (textured && (!cur.set || cur.tex != tex))
			{
				glBindTexture(GL_TEXTURE_2D, tex);
				cur.tex = tex;
			}
			// Per-draw blend the game set (normal alpha for UI, additive for glows, subtractive for
			// shadows). The equation switches add vs reverse-subtract.
			if (!cur.set)
			{
				glEnable(GL_BLEND);
			}
			// The GS ALPHA equation ((A-B)*C+D) is RGB-only; framebuffer alpha is a straight copy of
			// the source alpha (masked by FRAME/AFAIL/FBA), never blended. GL blends all four channels
			// together, so write the alpha channel separately with ONE/ZERO to match. This is what lets
			// dest-alpha effects (godrays, door glow) read the mask a prior draw stamped: without it the
			// blended alpha never drops below ~0.75 and DST_ALPHA copies smear the whole region.
			// Display: straight source-alpha write (GS semantics; feeds dest-alpha effects). Off-screen
			// RT: RGB uses the game's blend, but the alpha channel MULTIPLIES (dst*srcAlpha) so the
			// gradient bands modulate the silhouette the meshes already wrote (silhouette x gradient)
			// instead of overwriting it - that overwrite is what painted shadow everywhere, not just
			// the silhouette footprint.
			// Alpha equation is always ADD: display writes src (ONE/ZERO), the RT multiplies
			// (dst*srcAlpha via ZERO/SRC_ALPHA). RGB keeps the draw's own equation.
			if (!cur.set || !(cur.blend == bgs.blend))
			{
				glBlendEquationSeparate(bgs.blend.equation, GL_FUNC_ADD);
				if (renderer.passIsDisplay)
					glBlendFuncSeparate(bgs.blend.src, bgs.blend.dst, GL_ONE, GL_ZERO);
				else
					glBlendFuncSeparate(bgs.blend.src, bgs.blend.dst, GL_ZERO, GL_SRC_ALPHA);
				// Only meaningful when srcBlend/dstBlend reference GL_CONSTANT_ALPHA (GS ALPHA's C=FIX
				// case); harmless to always set otherwise.
				glBlendColor(0.0f, 0.0f, 0.0f, bgs.blend.constAlpha);
				cur.blend = bgs.blend;
				if (const GLenum err = glGetError(); err != GL_NO_ERROR)
				{
					SKY2_ERROR(false, "GL error 0x%X from glBlendEquation/glBlendFunc "
									   "(eq=0x%X src=0x%X dst=0x%X)",
						err, bgs.blend.equation, bgs.blend.src, bgs.blend.dst);
				}
			}
			// GS depth. GL ties writes to GL_DEPTH_TEST, so write-without-test keeps the test on with
			// GL_ALWAYS. The GS compares with GEQUAL on nearer-is-larger Z; 2d.vert flips that into GL's
			// direction, which makes the comparison LEQUAL here.
			const bool depthOn = sky2::config().depth2d && (bgs.ztest || bgs.zwrite);
			const int depthFunc = !depthOn ? 0 : (bgs.ztest ? GL_LEQUAL : GL_ALWAYS);
			const bool depthWrite = depthOn && bgs.zwrite;
			if (!cur.set || cur.depthFunc != depthFunc)
			{
				if (depthFunc)
				{
					glEnable(GL_DEPTH_TEST);
					glDepthFunc(static_cast<GLenum>(depthFunc));
				}
				else
				{
					glDisable(GL_DEPTH_TEST);
				}
				cur.depthFunc = depthFunc;
			}
			if (!cur.set || cur.depthWrite != depthWrite)
			{
				glDepthMask(depthWrite ? GL_TRUE : GL_FALSE);
				cur.depthWrite = depthWrite;
			}
			// Per-draw GS alpha test (as a shader discard); ATST_ALWAYS (pass) when off. The
			// shader's if/else-if discard chain has no else, so an out-of-range mode silently means
			// "pass" - validate here instead of trusting the parsed value.
			SKY2_ERROR(bgs.atest.method >= 0 && bgs.atest.method <= 7,
				"atestMethod %d out of range (0..7)", bgs.atest.method);
			// AFAIL says which of colour, alpha and Z a failing fragment still writes. Those are three
			// independent outputs, so the general form is two draws over disjoint outputs: the passing
			// half writes all three, the failing half writes what AFAIL names. Only RGB_ONLY needed
			// that while 2D had no depth; a game writes its UI clip barriers with ATST=NEVER +
			// ZB_ONLY, which is a draw whose entire purpose is the depth write.
			const bool failColor = bgs.atest.afail == AFAIL_FB_ONLY || bgs.atest.afail == AFAIL_RGB_ONLY;
			const bool failAlpha = bgs.atest.afail == AFAIL_FB_ONLY;
			const bool failDepth = bgs.atest.afail == AFAIL_ZB_ONLY && depthWrite;
			// The failing half writes exactly what the passing half would: one draw, no test.
			const bool failWritesAll = failColor && failAlpha && failDepth == depthWrite;
			// It writes nothing: one draw that discards, which is the common cutout case.
			const bool failWritesNothing = !failColor && !failAlpha && !failDepth;
			const bool afailSplit = bgs.atest.enabled && !failWritesAll && !failWritesNothing;
			const int atestMode = bgs.atest.enabled && !failWritesAll ? bgs.atest.method : ATST_ALWAYS;
			if (!cur.set || cur.atestMode != atestMode)
			{
				glUniform1i(uAtestMode, atestMode);
				cur.atestMode = atestMode;
			}
			if (!cur.set || cur.atestRef != bgs.atest.ref)
			{
				glUniform1f(uAtestRef, bgs.atest.ref);
				cur.atestRef = bgs.atest.ref;
			}
			if (!cur.set || cur.alphaPremul != bgs.blend.alphaPremul)
			{
				glUniform1f(uAlphaPremul, bgs.blend.alphaPremul);
				cur.alphaPremul = bgs.blend.alphaPremul;
			}
			// GS FRAME write mask: the mdlEffect mask pass writes only framebuffer alpha (RGB masked),
			// so the later dest-alpha composite blends through it instead of painting the scene black.
			if (!cur.set || cur.colorMask != bgs.colorMask)
			{
				glColorMask(bgs.colorMask.r, bgs.colorMask.g, bgs.colorMask.b, bgs.colorMask.a);
				cur.colorMask = bgs.colorMask;
			}
			// The CLAMP_1 region is in GS frame-buffer texel space, so it only means anything for a draw
			// that samples the frame buffer. Applied to an ordinary texture it would clamp UVs that
			// legitimately span 0..1. An empty rect is the shader's no-clamp case.
			const bool clampApplies = bgs.clamp.active && b.texKey && isFrameBufferFeedbackTexId(*b.texKey);
			const glm::vec4 uvClamp =
				clampApplies ? glm::vec4(bgs.clamp.uvMin, bgs.clamp.uvMax) : glm::vec4(0.0f);
			if (!cur.set || cur.uvClamp != uvClamp)
			{
				glUniform4f(uUvClamp, uvClamp.x, uvClamp.y, uvClamp.z, uvClamp.w);
				cur.uvClamp = uvClamp;
			}
			// The frame-buffer tap is the one texture the driver programs with TEX0.TCC=0: every
			// PSMCT32 batch in the GS trace that samples it has the texture alpha out of the texture
			// function, so the draw's alpha is its vertex alpha. Feeding the frame buffer's own alpha
			// back in instead corrupts the destination alpha that the next effect draw blends against.
			const int ignoreTexAlpha = b.texKey && isFrameBufferFeedbackTexId(*b.texKey) ? 1 : 0;
			if (!cur.set || cur.ignoreTexAlpha != ignoreTexAlpha)
			{
				glUniform1f(uIgnoreTexAlpha, float(ignoreTexAlpha));
				cur.ignoreTexAlpha = ignoreTexAlpha;
			}
			if (!cur.set || !(cur.stencil == bgs.stencil))
			{
				gl::applyStencil(bgs.stencil);
				cur.stencil = bgs.stencil;
			}
			cur.set = true;
			// Positions are already in source (640x448) space; the ortho projection above and the
			// letterboxed viewport (see letterboxViewport) do the only scaling to window pixels.
			const auto draw = [&]() {
				glDrawArrays(GL_TRIANGLES, static_cast<GLint>(b.vertOffset),
					static_cast<GLsizei>(b.vertCount));
			};
			draw(); // the passing half, already set up above
			if (afailSplit)
			{
				// GL cannot vary the write mask per fragment, so the failing half is a second draw
				// over the outputs AFAIL leaves writable, selected by the complementary test so the
				// two halves cover disjoint fragments and nothing is blended twice.
				const glm::bvec4& cm = bgs.colorMask;
				glColorMask(failColor && cm.r, failColor && cm.g, failColor && cm.b, failAlpha && cm.a);
				glDepthMask(failDepth ? GL_TRUE : GL_FALSE);
				glUniform1i(uAtestMode, ATST_COMPLEMENT[bgs.atest.method & 7]);
				draw();
				cur.set = false; // the split left its own masks and uniforms behind
			}
			renderer.traceDrawRegion(static_cast<int>(order[bi]));
		}
		glBlendEquation(GL_FUNC_ADD); // restore so the next pass/frame is not left on subtract
		glDisable(GL_STENCIL_TEST); // a 2D draw may have left the stencil test on
		glStencilMask(0xFF);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); // restore full writes after any mask pass
		glDisable(GL_DEPTH_TEST); // the 3D path sets its own depth state per mesh
		glDepthMask(GL_TRUE);
	}

	void Im2DRenderer::destroy()
	{
		if (vao) glDeleteVertexArrays(1, &vao);
		if (vbo) glDeleteBuffers(1, &vbo);
		*this = {}; // the program is owned by the shader cache
	}
} // namespace sky2
