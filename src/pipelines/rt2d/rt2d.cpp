#include "pipelines/rt2d/rt2d.h"

#include "device.h"
#include "common.h"
#include "frame.h"
#include "geometry.h"
#include "render_queue.h"
#include "renderer.h"
#include "texture_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sky2
{
	namespace
	{
		// Rt2d scratch geometry is small (a UI shape, a line of text). These bound a malformed guest
		// structure without capping anything the game legitimately draws.
		constexpr int MAX_PATH_NODES = 4096;
		constexpr int MAX_STRING_CHARS = 1024;
		constexpr int CURVE_MIN_STEPS = 2;
		constexpr int CURVE_MAX_STEPS = 32;

		uint32_t globalsBase() { return sky2::addresses().Rt2dGlobals; }

		template <class T>
		T readGlobal(uint32_t offset)
		{
			return *guest::ptr<T>(globalsBase() + offset);
		}

		bool logEnabled()
		{
			static const bool on = sky2::env("RT2D_LOG") != nullptr;
			return on;
		}

		// Where a submitted draw landed on screen, so a transform that is subtly wrong (off-screen,
		// mirrored, collapsed to a point) is visible without rendering it.
		void logDraw(const char* what, const Frame& f, const Im2DDraw& d, uint32_t texture)
		{
			glm::vec2 lo(1e9f), hi(-1e9f);
			for (uint32_t i = 0; i < d.vertCount; i++)
			{
				const Im2DVertex& v = f.im2dVerts[d.vertOffset + i];
				lo = glm::min(lo, glm::vec2(v.x, v.y));
				hi = glm::max(hi, glm::vec2(v.x, v.y));
			}
			const Im2DVertex& first = f.im2dVerts[d.vertOffset];
			std::fprintf(stderr,
				"[sky2][rt2d] %s verts=%u tex=0x%X bbox=(%.1f,%.1f)-(%.1f,%.1f) col=(%.2f,%.2f,%.2f,%.2f)\n",
				what, d.vertCount, texture, lo.x, lo.y, hi.x, hi.y, first.r, first.g, first.b, first.a);
		}

		// The four shade parameters a brush interpolates between, so the gradient formula can be read off
		// real values instead of inferred from documentation for a different RenderWare version.
		void logBrush(const Rt2dBrush& b)
		{
			const auto one = [](const char* tag, const rt2dShadeParameters& p) {
				std::fprintf(stderr, "[sky2][rt2d]   %-8s col=(%.1f %.1f %.1f %.1f) uv=(%.4f %.4f)\n", tag,
					p.col.red, p.col.green, p.col.blue, p.col.alpha, p.uv.x, p.uv.y);
			};
			one("top", b.top);
			one("dtop", b.dtop);
			one("bottom", b.bottom);
			one("dbottom", b.dbottom);
		}
	} // namespace

	// _rt2dCTMGet rebuilt. Both scales and the translate combine with rwCOMBINEPOSTCONCAT, so in RW's
	// row-vector order a point runs ctm -> metric -> scale(d) -> scale(-m,m) -> translate -> camera LTM.
	// Column-vector matrices multiply in the reverse order.
	Rt2DPipeline::Transform Rt2DPipeline::readTransform() const
	{
		Transform out;
		if (!globalsBase())
		{
			return out;
		}
		const uint32_t cameraAddr = readGlobal<uint32_t>(rt2dGlobalOffset::VIEW_CAMERA);
		if (!cameraAddr)
		{
			SKY2_ERROR(false, "Rt2d draw with no view camera");
			return out;
		}
		const RwCamera& camera = *guest::ptr<RwCamera>(cameraAddr);
		out.layerZ = readGlobal<float>(rt2dGlobalOffset::LAYER_DEPTH);
		// A parallel projection ignores the layer scale; a perspective one uses the layer depth so
		// deeper layers shrink with distance.
		const float d = camera.projectionType == RwCameraProjection::rwPARALLEL
			? 1.0f
			: out.layerZ;

		// Get top of matrix stack index
		const int32_t mtos = readGlobal<int32_t>(rt2dGlobalOffset::MTOS);
		if (mtos < 0 || mtos >= 32)
		{
			SKY2_ERROR(false, "Rt2d CTM stack depth %d out of range", mtos);
			return out;
		}
		const uint32_t topAddr =
			*guest::ptr<uint32_t>(globalsBase() + rt2dGlobalOffset::CTM_STACK + uint32_t(mtos) * 4);
		if (!topAddr)
		{
			SKY2_ERROR(false, "Rt2d CTM stack slot %d is null", mtos);
			return out;
		}
		const glm::mat4 top = toMat4(*guest::ptr<RwMatrixTag>(topAddr));
		const glm::mat4 metric = toMat4(*guest::ptr<RwMatrixTag>(globalsBase() + rt2dGlobalOffset::METRIC_MTX));

		const RwV2d vw = readGlobal<RwV2d>(rt2dGlobalOffset::VW);
		const float m = std::min(vw.x, vw.y);
		// scale(d,d,1), then scale(-m,m,1), then translate(0.5*vw.x*d, -0.5*vw.y*d, 0) collapse to one
		// matrix: a diagonal with the translation in the last column. The x flip and negated y offset
		// put the 2D origin at the view's top-left, the corner Rt2d coordinates are measured from.
		const glm::mat4 device2d(-m * d, 0.0f, 0.0f, 0.0f, 0.0f, m * d, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
			0.5f * vw.x * d, -0.5f * vw.y * d, 0.0f, 1.0f);
		const glm::mat4 ctm2d = device2d * metric * top;

		const uint32_t frameAddr = camera.object.object.parent.addr;
		const glm::mat4 cameraLtm =
			frameAddr ? toMat4(guest::ptr<RwFrame>(frameAddr)->ltm) : glm::mat4(1.0f);
		const glm::mat4 ctm = cameraLtm * ctm2d;

		if (logEnabled())
		{
			static bool dumped = false;
			if (!dumped)
			{
				dumped = true;
				const auto row = [](const char* tag, const glm::mat4& m) {
					std::fprintf(stderr, "[sky2][rt2d]   %-8s", tag);
					for (int c = 0; c < 4; c++)
						std::fprintf(stderr, " (%.3f %.3f %.3f %.3f)", m[c][0], m[c][1], m[c][2], m[c][3]);
					std::fprintf(stderr, "\n");
				};
				std::fprintf(stderr,
					"[sky2][rt2d] mtos=%d d=%.4f vw=(%.2f,%.2f) cam=0x%X passCam=0x%X valid=%d\n",
					mtos, d, vw.x, vw.y, cameraAddr, device->queue.activeCamera,
					readGlobal<int32_t>(rt2dGlobalOffset::CTM_VALID));
				row("top", top);
				row("metric", metric);
				row("ctm2d", ctm2d);
				row("camLtm", cameraLtm);
				row("ctm", ctm);
				row("passVp", device->queue.currentBuildPass().vp);
			}
		}
		// Self-check: when the guest's own copy is valid it must agree. The leaves fetch the CTM inside
		// the pipeline node, after this hook has returned, so the copy is usually stale and this only
		// fires on the calls where a comparison is meaningful.
		if (readGlobal<int32_t>(rt2dGlobalOffset::CTM_VALID))
		{
			const glm::mat4 stored = toMat4(*guest::ptr<RwMatrixTag>(globalsBase() + rt2dGlobalOffset::CTM));
			float worst = 0.0f;
			for (int c = 0; c < 4; c++)
			{
				for (int r = 0; r < 3; r++) // row 3 is the implicit 0,0,0,1
				{
					worst = std::max(worst, std::abs(stored[c][r] - ctm[c][r]));
				}
			}
			static bool warned = false;
			if (worst > 1e-3f && !warned)
			{
				warned = true;
				std::fprintf(stderr, "[sky2][rt2d] composed CTM differs from the guest's by %.4f\n", worst);
			}
		}
		out.ctm = ctm;
		out.valid = true;
		return out;
	}

	void Rt2DPipeline::dumpGuestCtm() const
	{
		if (!logEnabled() || !globalsBase())
		{
			return;
		}
		static int left = 3;
		if (left-- <= 0)
		{
			return;
		}
		const glm::mat4 stored = toMat4(*guest::ptr<RwMatrixTag>(globalsBase() + rt2dGlobalOffset::CTM));
		const glm::mat4 mine = readTransform().ctm;
		const auto row = [](const char* tag, const glm::mat4& m) {
			std::fprintf(stderr, "[sky2][rt2d]   %-8s", tag);
			for (int c = 0; c < 4; c++)
				std::fprintf(stderr, " (%.3f %.3f %.3f %.3f)", m[c][0], m[c][1], m[c][2], m[c][3]);
			std::fprintf(stderr, "\n");
		};
		std::fprintf(stderr, "[sky2][rt2d] after the call: valid=%d\n",
			readGlobal<int32_t>(rt2dGlobalOffset::CTM_VALID));
		row("guest", stored);
		row("mine", mine);
	}

	glm::vec2 Rt2DPipeline::project(const Transform& t, const glm::vec2& p) const
	{
		const FramePass& pass = device->queue.currentBuildPass();
		const glm::vec4 clip = pass.vp * t.ctm * glm::vec4(p.x, p.y, t.layerZ, 1.0f);
		const float w = std::abs(clip.w) > 1e-6f ? clip.w : 1.0f;
		const float width = pass.targetW > 0 ? float(pass.targetW) : GAME_WIDTH;
		const float height = pass.targetH > 0 ? float(pass.targetH) : GAME_HEIGHT;
		// Im2D positions are target pixels with y down, which is the ortho drawIm2D builds (T=0, B=h).
		return {(clip.x / w + 1.0f) * 0.5f * width, (1.0f - clip.y / w) * 0.5f * height};
	}

	void Rt2DPipeline::beginDraw(Im2DDraw& d, uint32_t textureAddr)
	{
		RenderQueue& queue = device->queue;
		d.source = Im2DSource::Rt2d;
		d.sampler = device->im2dFilterAddressing();
		// The brush (or the glyph page) owns the texture. The render-state mirror would be wrong here:
		// font show re-points it per glyph page as it batches.
		d.texKey = textureAddr ? device->textures.convertTexture(textureAddr) : std::nullopt;
		if (d.texKey)
		{
			const RwTexture& tex = *guest::ptr<RwTexture>(textureAddr);
			d.sampler = tex.filterAddressing;
		}
		DrawState st;
		st.blend = queue.blendState();
		st.stencil = queue.stencil;
		st.atest = queue.gsAtest;
		st.colorMask = queue.colorMask;
		st.clamp = queue.gsClamp;
		st.gsAlphaReg = queue.gsBlend.reg.U64;
		// Rt2d builds its own vertices and has no guest Z to test against, so it stays out of the
		// depth buffer entirely rather than defaulting to the far plane.
		st.ztest = false;
		st.zwrite = false;
		d.stateIndex = queue.stateIndexFor(st);
		d.vertOffset = static_cast<uint32_t>(queue.buildFrame().im2dVerts.size());
		d.vertCount = 0;
	}

	void Rt2DPipeline::pushVertex(Frame& f, Im2DDraw& d, const Im2DVertex& v)
	{
		f.im2dVerts.push_back(v);
		d.vertCount++;
	}

	Im2DVertex Rt2DPipeline::shadeAt(const Shade& s, const glm::vec2& pathPos, const glm::vec2& screenPos)
	{
		// A brush shades a gradient across the path's bounding box. top/bottom hold the value at the
		// left edge of each row and dtop/dbottom the delta across to the right edge, so a plain textured
		// fill reads as a rectangle: u from top.uv.x to top.uv.x + dtop.uv.x, v from top to bottom.
		// Rt2d's y axis points up, so the box's top edge is its MAXIMUM path y, not its minimum.
		const float t = std::clamp((s.bboxMin.y + s.bboxSpan.y - pathPos.y) / s.bboxSpan.y, 0.0f, 1.0f);
		const float u = std::clamp((pathPos.x - s.bboxMin.x) / s.bboxSpan.x, 0.0f, 1.0f);
		const rt2dShadeParameters& top = s.brush.top;
		const rt2dShadeParameters& bottom = s.brush.bottom;
		const rt2dShadeParameters& dtop = s.brush.dtop;
		const rt2dShadeParameters& dbottom = s.brush.dbottom;
		const float r0 = top.col.red + dtop.col.red * u, r1 = bottom.col.red + dbottom.col.red * u;
		const float g0 = top.col.green + dtop.col.green * u, g1 = bottom.col.green + dbottom.col.green * u;
		const float b0 = top.col.blue + dtop.col.blue * u, b1 = bottom.col.blue + dbottom.col.blue * u;
		const float a0 = top.col.alpha + dtop.col.alpha * u, a1 = bottom.col.alpha + dbottom.col.alpha * u;
		Im2DVertex v;
		v.x = screenPos.x;
		v.y = screenPos.y;
		constexpr float TO_UNIT = 1.0f / 255.0f;
		v.r = (r0 + (r1 - r0) * t) * TO_UNIT;
		v.g = (g0 + (g1 - g0) * t) * TO_UNIT;
		v.b = (b0 + (b1 - b0) * t) * TO_UNIT;
		v.a = (a0 + (a1 - a0) * t) * TO_UNIT;
		if (s.haveUV)
		{
			const float su0 = top.uv.x + dtop.uv.x * u, su1 = bottom.uv.x + dbottom.uv.x * u;
			const float sv0 = top.uv.y + dtop.uv.y * u, sv1 = bottom.uv.y + dbottom.uv.y * u;
			v.u = su0 + (su1 - su0) * t;
			v.v = sv0 + (sv1 - sv0) * t;
		}
		return v;
	}

	// A cubic Bezier as line nodes. rt2dCURVE carries the two control points and the end point in
	// three consecutive nodes; only the end point becomes an outline vertex on the guest side too.
	void Rt2DPipeline::flattenCurve(const RwV2d& from, const rt2dPathNode* ctrl, float tolerance,
		std::vector<rt2dPathNode>& out)
	{
		const glm::vec2 p0(from.x, from.y);
		const glm::vec2 p1(ctrl[0].pos.x, ctrl[0].pos.y);
		const glm::vec2 p2(ctrl[1].pos.x, ctrl[1].pos.y);
		const glm::vec2 p3(ctrl[2].pos.x, ctrl[2].pos.y);
		// Step count from the control polygon's length against the tolerance: a flatter curve needs
		// fewer segments to stay inside it.
		const float span = glm::length(p1 - p0) + glm::length(p2 - p1) + glm::length(p3 - p2);
		const float tol = tolerance > 1e-6f ? tolerance : 0.05f;
		int steps = int(std::sqrt(span / tol)) + 1;
		steps = std::clamp(steps, CURVE_MIN_STEPS, CURVE_MAX_STEPS);
		for (int i = 1; i <= steps; i++)
		{
			const float t = float(i) / float(steps);
			const float mt = 1.0f - t;
			const glm::vec2 p = mt * mt * mt * p0 + 3.0f * mt * mt * t * p1 + 3.0f * mt * t * t * p2 +
								t * t * t * p3;
			rt2dPathNode n = ctrl[2];
			n.type = rt2dLINE;
			n.pos.x = p.x;
			n.pos.y = p.y;
			out.push_back(n);
		}
	}

	bool Rt2DPipeline::readOutline(uint32_t pathAddr, std::vector<rt2dPathNode>& out) const
	{
		out.clear();
		const Rt2dPath& path = *guest::ptr<Rt2dPath>(pathAddr);
		const int32_t count = path.numSegment;
		if (count <= 0 || count > MAX_PATH_NODES || !path.segments)
		{
			SKY2_ERROR(count >= 0 && count <= MAX_PATH_NODES, "Rt2d path with %d segments", count);
			return false;
		}
		const auto nodes = path.segments;
		if (path.flat)
		{
			for (int32_t i = 0; i < count; i++)
			{
				out.push_back(nodes[i]);
			}
			return !out.empty();
		}
		// Not flat: `segments` holds curve control points. The guest flattens into a scratch path here;
		// do it locally instead, since calling back into the guest would not work under offline replay.
		const float tolerance = readGlobal<float>(rt2dGlobalOffset::TOLERANCE);
		RwV2d cursor{0.0f, 0.0f};
		for (int32_t i = 0; i < count; i++)
		{
			const rt2dPathNode n = nodes[i];
			if (n.type == rt2dCURVE && i + 2 < count)
			{
				const rt2dPathNode ctrl[3] = {n, nodes[i + 1], nodes[i + 2]};
				flattenCurve(cursor, ctrl, tolerance, out);
				cursor = ctrl[2].pos;
				i += 2;
			}
			else
			{
				out.push_back(n);
				cursor = n.pos;
			}
		}
		return !out.empty();
	}

	// _rt2dPS2PathFill(path, brush). Only the head sub-path: the guest recurses over path->next by
	// calling itself, so this fires again for each one.
	void Rt2DPipeline::pathFill(uint32_t pathAddr, uint32_t brushAddr)
	{
		if (!pathAddr || !brushAddr || !device->queue.haveCam)
		{
			return;
		}
		const Rt2dPath& path = *guest::ptr<Rt2dPath>(pathAddr);
		if (!path.closed) // the original early-outs on an open path and draws nothing
		{
			return;
		}
		std::vector<rt2dPathNode> outline;
		if (!readOutline(pathAddr, outline) || outline.size() < 3)
		{
			return;
		}

		Shade shade;
		shade.brush = *guest::ptr<Rt2dBrush>(brushAddr);
		shade.haveUV = shade.brush.texture.addr != 0;
		glm::vec2 lo(outline[0].pos.x, outline[0].pos.y), hi = lo;
		for (const rt2dPathNode& n : outline)
		{
			lo = glm::min(lo, glm::vec2(n.pos.x, n.pos.y));
			hi = glm::max(hi, glm::vec2(n.pos.x, n.pos.y));
		}
		shade.bboxMin = lo;
		shade.bboxSpan = glm::max(hi - lo, glm::vec2(1e-6f));

		const Transform xf = readTransform();
		if (!xf.valid)
		{
			return;
		}
		Frame& f = device->queue.buildFrame();
		Im2DDraw d;
		beginDraw(d, shade.brush.texture.addr);

		const auto vertexAt = [&](size_t i) {
			const glm::vec2 p(outline[i].pos.x, outline[i].pos.y);
			return shadeAt(shade, p, project(xf, p));
		};
		std::vector<Im2DVertex> strip;
		size_t front = 0, back = outline.size() - 1;
		while (front <= back)
		{
			strip.push_back(vertexAt(front));
			if (front == back)
			{
				break;
			}
			strip.push_back(vertexAt(back));
			front++;
			back--;
		}
		for (size_t i = 2; i < strip.size(); i++)
		{
			// Alternate winding so every triangle of the strip keeps the outline's orientation.
			const bool odd = (i & 1) != 0;
			pushVertex(f, d, strip[i - 2]);
			pushVertex(f, d, odd ? strip[i] : strip[i - 1]);
			pushVertex(f, d, odd ? strip[i - 1] : strip[i]);
		}
		if (d.vertCount != 0)
		{
			if (logEnabled())
			{
				logDraw("fill", f, d, shade.brush.texture.addr);
				static int brushes = 3;
				if (brushes-- > 0)
				{
					std::fprintf(stderr, "[sky2][rt2d]   pathBbox=(%.2f,%.2f) span=(%.2f,%.2f)\n",
						shade.bboxMin.x, shade.bboxMin.y, shade.bboxSpan.x, shade.bboxSpan.y);
					logBrush(shade.brush);
				}
			}
			device->queue.submitIm2D(std::move(d));
		}
	}

	// _rt2dPS2PathStroke(path, brush). Head sub-path only, same recursion issue as for fill.
	void Rt2DPipeline::pathStroke(uint32_t pathAddr, uint32_t brushAddr)
	{
		if (!pathAddr || !brushAddr || !device->queue.haveCam)
		{
			return;
		}
		const Rt2dPath& path = *guest::ptr<Rt2dPath>(pathAddr);
		std::vector<rt2dPathNode> outline;
		if (!readOutline(pathAddr, outline) || outline.size() < 2)
		{
			return;
		}

		Shade shade;
		shade.brush = *guest::ptr<Rt2dBrush>(brushAddr);
		shade.haveUV = shade.brush.texture.addr != 0;
		glm::vec2 lo(outline[0].pos.x, outline[0].pos.y), hi = lo;
		for (const rt2dPathNode& n : outline)
		{
			lo = glm::min(lo, glm::vec2(n.pos.x, n.pos.y));
			hi = glm::max(hi, glm::vec2(n.pos.x, n.pos.y));
		}
		shade.bboxMin = lo;
		shade.bboxSpan = glm::max(hi - lo, glm::vec2(1e-6f));

		const float half = shade.brush.halfwidth;
		const Transform xf = readTransform();
		if (!xf.valid)
		{
			return;
		}
		Frame& f = device->queue.buildFrame();
		Im2DDraw d;
		beginDraw(d, shade.brush.texture.addr);

		// Each node carries a precomputed outward normal, so a segment expands to a quad by offsetting
		// its two ends along their own normals.
		const auto edgePair = [&](size_t i, Im2DVertex& outer, Im2DVertex& inner) {
			const glm::vec2 p(outline[i].pos.x, outline[i].pos.y);
			const glm::vec2 n(outline[i].normal.x, outline[i].normal.y);
			// Shaded at the node's own path position, like a fill: the brush gradient is defined over
			// the path's bounding box, not along the line.
			outer = shadeAt(shade, p, project(xf, p + n * half));
			inner = shadeAt(shade, p, project(xf, p - n * half));
		};
		const size_t segments = path.closed ? outline.size() : outline.size() - 1;
		for (size_t i = 0; i < segments; i++)
		{
			const size_t j = (i + 1) % outline.size();
			// A move starts a new contour, so no segment spans into it. The wrap-around of a closed path
			// is exempt: node 0 is the contour's own start, and testing it there drops the closing edge.
			if (j != 0 && outline[j].type == rt2dMOVE)
			{
				continue;
			}
			Im2DVertex a0, a1, b0, b1;
			edgePair(i, a0, a1);
			edgePair(j, b0, b1);
			pushVertex(f, d, a0);
			pushVertex(f, d, a1);
			pushVertex(f, d, b0);
			pushVertex(f, d, a1);
			pushVertex(f, d, b1);
			pushVertex(f, d, b0);
		}
		if (d.vertCount != 0)
		{
			if (logEnabled())
			{
				logDraw("stroke", f, d, shade.brush.texture.addr);
			}
			device->queue.submitIm2D(std::move(d));
		}
	}

	// _rt2dPS2FontShow(font, string, height, anchor, brush).
	void Rt2DPipeline::fontShow(uint32_t fontAddr, uint32_t stringAddr, float height, uint32_t anchorAddr,
		uint32_t brushAddr)
	{
		if (!fontAddr || !stringAddr || !brushAddr || !device->queue.haveCam)
		{
			return;
		}
		const Rt2dFont& font = *guest::ptr<Rt2dFont>(fontAddr);
		const uint32_t charMapAddr = font.charMap.addr;
		if (!charMapAddr)
		{
			return;
		}
		const RwSList& charMap = *guest::ptr<RwSList>(charMapAddr);
		if (!charMap.listElements || charMap.numElementsFilled <= 0)
		{
			return;
		}
		const auto glyphs = guest::ptr<rt2dCharRect>(charMap.listElements.addr);

		// flag bit 1 selects 2-byte characters, which is what a Japanese font uses. Reading a wide
		// string one byte at a time indexes the glyph table with half a character.
		constexpr uint32_t FLAG_WIDE_CHARS = 2;
		const bool wide = (font.flag & FLAG_WIDE_CHARS) != 0;
		const auto str = guest::ptr<uint8_t>(stringAddr);

		// Low codes index the direct table, higher ones the extended range. An unmapped character is
		// skipped by the original function.
		const auto glyphIndex = [&](uint32_t code) -> int32_t {
			int32_t glyph = -1;
			if (code < 128)
			{
				glyph = font.aCharIndex[code];
			}
			else if (font.uCharIndex && int32_t(code) >= font.uCharOffset &&
					 int32_t(code) < font.uCharOffset + font.uCharIndexCount)
			{
				glyph = font.uCharIndex[int32_t(code) - font.uCharOffset];
			}
			return glyph >= 0 && glyph < charMap.numElementsFilled ? glyph : -1;
		};
		const auto codeAt = [&](int i) {
			return wide ? uint32_t(str[i * 2]) | (uint32_t(str[i * 2 + 1]) << 8) : uint32_t(str[i]);
		};

		const RwV2d anchor = anchorAddr ? *guest::ptr<RwV2d>(anchorAddr) : RwV2d{0.0f, 0.0f};
		const float yBottom = anchor.y - font.baseline * height;

		// The brush gradient spans the run being drawn, so the run has to be measured before any of it
		// is shaded. Without this every glyph shades at one point and a gradient brush comes out flat.
		float runWidth = 0.0f;
		for (int i = 0; i < MAX_STRING_CHARS; i++)
		{
			const uint32_t code = codeAt(i);
			if (code == 0)
			{
				break;
			}
			const int32_t glyph = glyphIndex(code);
			if (glyph >= 0)
			{
				runWidth += (glyphs[glyph].width + font.intergap) * height;
			}
		}

		Shade shade;
		shade.brush = *guest::ptr<Rt2dBrush>(brushAddr);
		shade.haveUV = false; // glyph UVs come from the char rect, not the brush gradient
		shade.bboxMin = glm::vec2(anchor.x, yBottom);
		shade.bboxSpan = glm::max(glm::vec2(runWidth, height), glm::vec2(1e-6f));

		const Transform xf = readTransform();
		if (!xf.valid)
		{
			return;
		}
		Frame& f = device->queue.buildFrame();

		Im2DDraw d;
		uint32_t drawTexture = 0;
		bool haveDraw = false;
		float pen = anchor.x;
		for (int i = 0; i < MAX_STRING_CHARS; i++)
		{
			const uint32_t code = codeAt(i);
			if (code == 0)
			{
				break;
			}
			const int32_t glyph = glyphIndex(code);
			if (glyph < 0)
			{
				continue;
			}
			const rt2dCharRect rect = glyphs[glyph];
			const float advance = (rect.width + font.intergap) * height;
			if (!rect.tex || rect.width <= 0.0f)
			{
				pen += advance;
				continue;
			}
			// A run can span several glyph pages; each page is its own draw.
			if (!haveDraw || rect.tex.addr != drawTexture)
			{
				if (haveDraw && d.vertCount != 0)
				{
					device->queue.submitIm2D(std::move(d));
				}
				d = Im2DDraw{};
				drawTexture = rect.tex.addr;
				beginDraw(d, drawTexture);
				haveDraw = true;
			}

			const float x0 = pen;
			const float x1 = pen + rect.width * height;
			const float yTop = yBottom + height;
			const glm::vec2 corner[4] = {{x0, yBottom}, {x1, yBottom}, {x1, yTop}, {x0, yTop}};
			// Rt2d's y grows upward but a raster's v grows downward, so we flip the order.
			const glm::vec2 uv[4] = {{rect.uv[0].x, rect.uv[1].y}, {rect.uv[1].x, rect.uv[1].y},
				{rect.uv[1].x, rect.uv[0].y}, {rect.uv[0].x, rect.uv[0].y}};
			Im2DVertex v[4];
			for (int c = 0; c < 4; c++)
			{
				v[c] = shadeAt(shade, corner[c], project(xf, corner[c]));
				v[c].u = uv[c].x;
				v[c].v = uv[c].y;
			}
			const int order[6] = {0, 1, 2, 0, 2, 3};
			for (const int c : order)
			{
				pushVertex(f, d, v[c]);
			}
			pen += advance;
		}
		if (haveDraw && d.vertCount != 0)
		{
			device->queue.submitIm2D(std::move(d));
		}
		if (logEnabled())
		{
			std::fprintf(stderr, "[sky2][rt2d] fontShow font=0x%X h=%.2f anchor=(%.1f,%.1f) wide=%d\n",
				fontAddr, height, anchor.x, anchor.y, wide ? 1 : 0);
		}
	}
} // namespace sky2
