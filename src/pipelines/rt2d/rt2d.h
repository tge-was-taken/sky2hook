// Rt2d pipeline: the 2D vector toolkit (filled paths, stroked outlines, bitmap text) that menus and
// the HUD are built from. Its PS2 leaves reach the GS through RxPipelineExecute on a PS2 Manager node,
// so none of it arrives through the Im2D funnel. Hooked at the leaves, which still carry the path,
// brush and string; the geometry is rebuilt here and submitted as Im2D draws. Owned by Device.
#pragma once

#include "common.h"

#include <cstdint>
#include <vector>

namespace sky2
{
	class Device;
	struct Frame;

	struct Rt2DPipeline
	{
		Device* device = nullptr; // injected by Device (queue, textures)

		// _rt2dPS2PathFill(path, brush) / _rt2dPS2PathStroke(path, brush). Both recurse over the
		// sub-path chain by calling themselves, so each call handles its head sub-path only.
		void pathFill(uint32_t pathAddr, uint32_t brushAddr);
		void pathStroke(uint32_t pathAddr, uint32_t brushAddr);
		// _rt2dPS2FontShow(font, string, height, anchor, brush). The pen starts at *anchor; the guest
		// original advances it, so this must not write back.
		void fontShow(uint32_t fontAddr, uint32_t stringAddr, float height, uint32_t anchorAddr,
			uint32_t brushAddr);
		// Diagnostic: the guest's own CTM once a leaf has returned, next to the composed one.
		void dumpGuestCtm() const;

	private:
		// The Rt2d transform for one draw. The CTM's third column is the camera's forward direction, so
		// feeding `layerZ` as the vertex z places the 2D plane that far in front of the camera; the CTM
		// scales x and y by the same depth, so the perspective divide cancels it and apparent size stays
		// fixed. With z left at 0 the plane lands on the camera and every vertex divides by ~0.
		struct Transform
		{
			glm::mat4 ctm{1.0f};
			float layerZ = 0.0f;
			// False when the globals could not be read (no view camera, bad CTM stack). The identity
			// fallback would push raw path coordinates through the pass projection, so callers skip the
			// draw instead of submitting geometry at an arbitrary place.
			bool valid = false;
		};
		// Rebuilt from the globals rather than read from them: the leaves fetch the CTM inside the
		// pipeline node, after a hook at their entry has already returned, so the stored copy still
		// holds the previous draw's transform.
		Transform readTransform() const;
		// 2D path space -> the 640x448 (or render-target) pixel space Im2DDraw carries.
		glm::vec2 project(const Transform& t, const glm::vec2& p) const;

		// The head sub-path's outline, flattened if it held curve control points.
		bool readOutline(uint32_t pathAddr, std::vector<rt2dPathNode>& out) const;
		static void flattenCurve(const RwV2d& from, const rt2dPathNode* ctrl, float tolerance,
			std::vector<rt2dPathNode>& out);

		// Texture + interned GS state for one draw, with the texture taken from the brush rather than
		// from the render-state mirror (font show re-points the mirror per glyph page).
		void beginDraw(Im2DDraw& d, uint32_t textureAddr);
		static void pushVertex(Frame& f, Im2DDraw& d, const Im2DVertex& v);

		// Colour and UV at a point, from the brush's top-to-bottom gradient over the path bbox.
		struct Shade
		{
			Rt2dBrush brush;
			glm::vec2 bboxMin{0.0f}, bboxSpan{1.0f};
			bool haveUV = false;
		};
		static Im2DVertex shadeAt(const Shade& s, const glm::vec2& pathPos, const glm::vec2& screenPos);
	};
} // namespace sky2
