// Im2D pipeline (2D immediate primitives: UI text, sprites, fades: the RwIm2DRender* funnel). The
// build half is owned by Device, the render half (Im2DRenderer) by the Renderer's GlState.
#pragma once

#include "common.h"
#include "enhancements/feedback_blur.h" // FeedbackBlurRemovalEnhancement
#include "frame.h" // Frame: the Im2D render half resolves stateIndex through it

#include <cstdint>
#include <span>
#include <vector>

namespace sky2
{
	class Device;
	class Renderer;

	// The render-thread half: the Im2D GL program, buffers and per-pass vertex scratch. Lives in
	// GlState so it is created and freed with the rest of the render-thread GL.
	struct Im2DRenderer
	{
		GLuint prog = 0, vao = 0, vbo = 0;
		GLint uProj = -1, uTex = -1, uTextured = -1, uAtestMode = -1, uAtestRef = -1, uAlphaPremul = -1;
		GLint uUvClamp = -1;
		GLint uIgnoreTexAlpha = -1;
		enhancements::FeedbackBlurRemovalEnhancement feedbackBlurRemoval;
		GLint uZMax = -1;

		uint32_t shaderGeneration = 0; // ShaderManager generation initProgram compiled against

		void init(Renderer& renderer);
		void initProgram(Renderer& renderer); // program + uniform locations; re-run on a reload
		void uploadVerts(Renderer& renderer, const Frame& frame); // once per frame, before the passes draw
		// `order` is this pass's Im2D slice of Frame::drawOrder.
		void draw(Renderer& renderer, const Frame& frame, std::span<const uint32_t> order,
			float projW = 0.0f, float projH = 0.0f);
		void destroy(); // free the VAO/VBO; the program is owned by the shader cache
	};

	struct Im2DPipeline
	{
		Device* device = nullptr; // injected by Device (queue)

		void renderPrimitive(uint32_t primType, uint32_t vertsAddr, int numVerts);
		void renderTriangle(uint32_t vertsAddr, int numVerts, int vert1, int vert2, int vert3);
		void renderIndexedPrimitive(uint32_t primType, uint32_t vertsAddr, int numVerts,
			uint32_t indicesAddr, int numIndices);
		void renderLine(uint32_t vertsAddr, int numVerts, int vert1, int vert2);

	private:
		static Im2DVertex readVertex(uint32_t vertsAddr, int i);
		// Fill an Im2DDraw's texture + blend from the current render state, so each 2D draw carries the
		// blend mode the game set (normal alpha for UI, additive for glows/fades, etc.).
		void beginDraw(Im2DDraw& d);
		// Vertices go straight into the frame's arena; the draw records where its slice starts.
		static void pushVertex(Frame& f, Im2DDraw& d, const Im2DVertex& q);
		// A line segment as a 1px-wide quad (2 triangles); the Im2D program only draws triangles.
		static void pushLineQuad(Frame& f, Im2DDraw& d, const Im2DVertex& a, const Im2DVertex& b);
	};
} // namespace sky2
