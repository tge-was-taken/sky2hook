// sky2 frame-capture tracing: the JSONL draw manifest (debugWriteManifest), the PPM
// screenshot (writePPM), and the per-pass RGBA dump (dumpPassImage). Diagnostic only, off the
// render path; all three are Renderer members reading its render-thread state.
#include "common.h"
#include "sky2/host.h"
#include "object_manager.h"
#include "render_queue.h"
#include "renderer.h"
#include "rw_to_gl.h"
#include "texture_manager.h"


#include <glm/glm.hpp>

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace sky2
{
	// A 4x4 as 16 numbers in column-major order, matching glm's storage and what a shader would get.
	// "The object is in the wrong place" is a whole class of bug (this sky, the two map-clip scenes,
	// the dungeon-map offset) and the manifest could only ever show where it landed, not why.
	static void writeMat4(FILE* f, const char* key, const glm::mat4& m)
	{
		std::fprintf(f, "\"%s\":[", key);
		for (int c = 0; c < 4; c++)
		{
			for (int r = 0; r < 4; r++)
			{
				std::fprintf(f, "%s%.4g", (c || r) ? "," : "", m[c][r]);
			}
		}
		std::fprintf(f, "],");
	}

	static const char* sourceName(MeshSource s)
	{
		switch (s)
		{
			case MeshSource::Atomic: return "atomic";
			case MeshSource::Sector: return "sector";
			case MeshSource::Im3D: return "im3d";
		}
		return "?";
	}

	static void writeMeshJson(const Renderer& renderer, const Frame& frame, FILE* f, const MeshDraw& m,
		int i, int pass, const glm::mat4& vp)
	{
		const DrawState& gs = frame.states[m.stateIndex];
		const FrameLights& lights = frame.lights[m.lightsIndex];
		float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
		float x0 = 1e9f, y0 = 1e9f, z0 = 1e9f, x1 = -1e9f, y1 = -1e9f, z1 = -1e9f;
		float sx0 = 1e9f, sy0 = 1e9f, sx1 = -1e9f, sy1 = -1e9f; // screen-space bbox (px)
		bool scrClipped = false; // the box is a bound, not the extent: something crossed the near plane
		float fz0 = 1e9f, fz1 = -1e9f; // range of the depth the fog term uses
		float va0 = 1e9f, va1 = -1e9f, vaSum = 0.0f;
		int van = 0; // prelit vertex alpha (0..1)
		float vr = 0.0f, vg = 0.0f, vb = 0.0f; // prelit vertex color mean (0..1)
		const float sw = renderer.gameResW.load(), sh = renderer.gameResH.load();
		// A transient mesh names a slice of the frame arena rather than pool buffers, so the manifest
		// keeps describing the same geometry either way.
		const bool arenaMesh = m.verts == GEOM_ID_NONE;
		const std::span<const Vertex3D> verts = arenaMesh ? std::span<const Vertex3D>(frame.transientVerts).subspan(m.vertOffset, m.vertCount) : renderer.geometry->verts.view(m.verts);
		const std::span<const uint16_t> indices = arenaMesh ? std::span<const uint16_t>(frame.transientIdx).subspan(m.idxOffset, m.idxCount) : renderer.geometry->indices.view(m.idx);
		// A skinned draw's screen extent has to be measured through the bone palette, exactly as the
		// vertex shader does. Transforming the bind pose instead put P4's title atomics at
		// x=-550944, which reads as a broken transform and is only a broken measurement.
		const std::span<const glm::mat4> bones = m.boneCount > 0 ? std::span<const glm::mat4>(frame.bones).subspan(m.boneOffset, m.boneCount) : std::span<const glm::mat4>();
		const auto skin = [&](const Vertex3D& v) {
			if (bones.empty()) return glm::vec4(v.pos, 1.0f);
			glm::mat4 sm(0.0f);
			for (int k = 0; k < 4; k++)
			{
				const int b = int(v.boneIndex[k]);
				if (b >= 0 && size_t(b) < bones.size()) sm += bones[b] * v.boneWeight[k];
			}
			return sm * glm::vec4(v.pos, 1.0f);
		};
		for (uint32_t vi : indices) // the verts this submesh uses, not all
		{
			if (vi >= verts.size()) continue;
			const Vertex3D& v = verts[vi];
			va0 = std::min(va0, v.color.a);
			va1 = std::max(va1, v.color.a);
			vaSum += v.color.a;
			van++;
			vr += v.color.r;
			vg += v.color.g;
			vb += v.color.b;
			u0 = std::min(u0, v.uv.x);
			u1 = std::max(u1, v.uv.x);
			v0 = std::min(v0, v.uv.y);
			v1 = std::max(v1, v.uv.y);
			x0 = std::min(x0, v.pos.x);
			x1 = std::max(x1, v.pos.x);
			y0 = std::min(y0, v.pos.y);
			y1 = std::max(y1, v.pos.y);
			z0 = std::min(z0, v.pos.z);
			z1 = std::max(z1, v.pos.z);
			const glm::vec4 clip = vp * m.model * skin(v);
			fz0 = std::min(fz0, clip.w); // what the shader fogs against (3d.vert: vFogZ)
			fz1 = std::max(fz1, clip.w);
			if (clip.w > 1e-4f) // in front of the camera: project to screen pixels
			{
				// A vertex just in front of the near plane projects to millions, and an unbounded box
				// is worse than a coarse one: it reads as a draw in the wrong place and cannot be
				// matched to the GS batch it became. Bound it and say so through scrClipped.
				const float px = (clip.x / clip.w * 0.5f + 0.5f) * sw;
				const float py = (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * sh;
				scrClipped = scrClipped || px < -sw || px > 2.0f * sw || py < -sh || py > 2.0f * sh;
				sx0 = std::min(sx0, std::clamp(px, -sw, 2.0f * sw));
				sx1 = std::max(sx1, std::clamp(px, -sw, 2.0f * sw));
				sy0 = std::min(sy0, std::clamp(py, -sh, 2.0f * sh));
				sy1 = std::max(sy1, std::clamp(py, -sh, 2.0f * sh));
			}
			else
			{
				scrClipped = true; // behind the camera: the on-screen extent needs a real near clip
			}
		}
		// The prelit colour banded by depth, near to far over this draw's own fogZ range. A per-draw
		// mean says "0.90" for a floor whose far end is 0.55, which reads as flat shading when the
		// gradient is the whole point; four bands make the trend visible without a per-vertex dump.
		constexpr int BANDS = 4;
		glm::vec4 bandSum[BANDS] = {};
		int bandN[BANDS] = {};
		const float bandSpan = fz1 > fz0 ? (fz1 - fz0) : 0.0f;
		for (uint32_t vi : indices)
		{
			if (vi >= verts.size()) continue;
			const Vertex3D& v = verts[vi];
			const glm::vec4 clip = vp * m.model * skin(v);
			const int b = bandSpan > 0.0f ? std::clamp(int((clip.w - fz0) / bandSpan * BANDS), 0, BANDS - 1) : 0;
			bandSum[b] += v.color;
			bandN[b]++;
		}
		const auto bandMean = [&](int b) {
			return bandN[b] ? bandSum[b] / float(bandN[b]) : glm::vec4(0.0f);
		};
		// The first submitted vertex, in the GS's own units (128 = 1.0), so it lines up with the batch
		// row's RGBAQ without a conversion in the reader. A mean cannot say which side scaled wrong.
		const glm::vec4 vtx0 = indices.empty() || indices[0] >= verts.size() ? glm::vec4(0.0f) : verts[indices[0]].color * 128.0f;
		const bool textured = m.material.texKey && renderer.textures->lookup(*m.material.texKey) != 0;
		// The object->world transform, before anything else on the row, so a draw that landed in the
		// wrong place can be recomputed rather than guessed at from its bounding box.
		std::fprintf(f, "{\"type\":\"mesh\",\"i\":%d,", i);
		writeMat4(f, "model", m.model);
		std::fprintf(f,
			"\"ltm\":\"0x%08X\",\"im3dFlags\":\"0x%X\",\"i\":%d,\"pass\":%d,\"tex\":\"0x%016llX\",\"texPx\":\"0x%08X\","
			"\"pipe\":\"%s\",\"native\":%s,\"mat\":\"0x%08X\",\"gen\":%u,\"ord\":%u,"
			"\"objPipe\":\"0x%08X\",\"matPipe\":\"0x%08X\",\"objVu\":\"0x%016llX\",\"matVu\":\"0x%016llX\","
			"\"textured\":%s,\"nv\":%zu,\"ni\":%zu,"
			"\"uv\":[%.3f,%.3f,%.3f,%.3f],\"pos\":[%.1f,%.1f,%.1f,%.1f,%.1f,%.1f],"
			"\"scr\":[%.0f,%.0f,%.0f,%.0f],\"scrClipped\":%s,"
			"\"src\":\"0x%X\",\"dst\":\"0x%X\",\"lit\":%s,\"vcolor\":%s,\"skinned\":%s,\"blend\":%s,"
			"\"cull\":%d,\"rsCull\":%d,\"atomic\":\"0x%08X\",\"geomFlags\":\"0x%X\",\"numTexSets\":%d,"
			"\"texCoords\":\"0x%08X\",\"matfx\":%d,\"matfxEn\":%s,\"pass2\":%s,\"pass2Env\":%s,"
			"\"pass2Src\":\"0x%X\",\"pass2Dst\":\"0x%X\",\"pass2Alpha\":\"0x%llX\",\"pass2Coef\":%.3f,\"ate\":%s,\"afail\":%d,"
			"\"cmask\":[%d,%d,%d,%d],\"samp\":\"0x%04X\",\"gsAlpha\":\"0x%llX\",\"gsTest\":\"0x%llX\","
			"\"texAlpha\":%s,"
			"\"matRGB\":[%.3f,%.3f,%.3f],\"matA\":%.2f,\"modulate\":%s,\"valpha\":[%.2f,%.2f,%.2f],\"vrgb\":[%.2f,%.2f,%.2f],"
			"\"ztest\":%s,\"zwrite\":%s,\"fog\":%s,"
			"\"fogRange\":[%.1f,%.1f],\"fogZ\":[%.1f,%.1f],\"fogColor\":[%.3f,%.3f,%.3f],"
			"\"surfA\":%.2f,\"surfD\":%.2f,"
			"\"amb\":[%.3f,%.3f,%.3f],\"dirCount\":%d,\"dir0\":[%.3f,%.3f,%.3f],"
			"\"v0\":[%.1f,%.1f,%.1f,%.1f],"
			"\"vbands\":[[%d,%.3f,%.3f,%.3f,%.3f],[%d,%.3f,%.3f,%.3f,%.3f],"
			"[%d,%.3f,%.3f,%.3f,%.3f],[%d,%.3f,%.3f,%.3f,%.3f]]}\n",
			m.diag.ltmAddr, m.diag.im3dFlags,
			i, pass, static_cast<unsigned long long>(m.material.texKey.value_or(0)),
			m.material.texKey ? renderer.textures->sourceAddress(*m.material.texKey) : 0u,
			sourceName(m.diag.source), m.diag.native ? "true" : "false",
			m.diag.materialKey, m.diag.generation, m.diag.drawOrdinal,
			m.diag.objPipeline, m.diag.matPipeline,
			static_cast<unsigned long long>(m.diag.objVuHash),
			static_cast<unsigned long long>(m.diag.matVuHash),
			textured ? "true" : "false", verts.size(), indices.size(),
			u0, v0, u1, v1, x0, y0, z0, x1, y1, z1,
			sx0, sy0, sx1, sy1, scrClipped ? "true" : "false",
			gs.blend.src, gs.blend.dst, m.material.lit ? "true" : "false", m.material.vcolor ? "true" : "false",
			m.boneCount > 0 ? "true" : "false", gs.blend.enable ? "true" : "false", gs.cull, m.diag.rsCull,
			m.diag.atomicAddr, m.diag.geomFlags, m.diag.numTexCoordSets, m.diag.texCoordsAddr,
			static_cast<int>(m.matfx.effect), m.matfx.enabled ? "true" : "false",
			m.matfx.hasPass2 ? "true" : "false", m.matfx.pass2Env ? "true" : "false",
			m.matfx.pass2Blend.src, m.matfx.pass2Blend.dst,
			static_cast<unsigned long long>(m.matfx.pass2Blend.reg.U64), m.matfx.pass2Coef,
			gs.atest.enabled ? "true" : "false", gs.atest.afail,
			gs.colorMask.r, gs.colorMask.g, gs.colorMask.b, gs.colorMask.a, m.material.sampler, static_cast<unsigned long long>(gs.gsAlphaReg), static_cast<unsigned long long>(gs.atest.reg.U64),
			m.material.texKey && renderer.textures->hasTranslucentTexels(*m.material.texKey) ? "true" : "false",
			m.material.color.r, m.material.color.g, m.material.color.b, // the light scale, not the texture's
			m.material.color.a, m.material.modulate ? "true" : "false",
			van ? va0 : 0.0f, van ? vaSum / van : 0.0f, van ? va1 : 0.0f,
			van ? vr / van : 0.0f, van ? vg / van : 0.0f, van ? vb / van : 0.0f,
			gs.ztest ? "true" : "false", gs.zwrite ? "true" : "false", gs.fog ? "true" : "false",
			lights.fogStart, lights.fogEnd, fz0, fz1, lights.fogColor.r, lights.fogColor.g, lights.fogColor.b,
			m.material.surfAmbient, m.material.surfDiffuse,
			lights.ambient.r, lights.ambient.g, lights.ambient.b, lights.dirCount,
			lights.dirColor[0].r, lights.dirColor[0].g, lights.dirColor[0].b,
			vtx0.r, vtx0.g, vtx0.b, vtx0.a,
			bandN[0], bandMean(0).r, bandMean(0).g, bandMean(0).b, bandMean(0).a,
			bandN[1], bandMean(1).r, bandMean(1).g, bandMean(1).b, bandMean(1).a,
			bandN[2], bandMean(2).r, bandMean(2).g, bandMean(2).b, bandMean(2).a,
			bandN[3], bandMean(3).r, bandMean(3).g, bandMean(3).b, bandMean(3).a);
	}

	// The MatFX second pass draws the same geometry again through GS context 2, so a diff against the
	// GS trace only balances if it appears as its own row. It carries ALPHA_2 and no alpha test, which
	// is what context 2 shows: sky2 runs it with ATST_ALWAYS.
	static void writePass2Json(const Frame& frame, FILE* f, const MeshDraw& m, int i, int pass)
	{
		if (!m.matfx.hasPass2 || !m.matfx.pass2TexKey)
		{
			return;
		}
		const DrawState& gs = frame.states[m.stateIndex];
		std::fprintf(f,
			"{\"type\":\"pass2\",\"i\":%d,\"pass\":%d,\"env\":%s,\"tex\":\"0x%016llX\","
			"\"textured\":true,\"blend\":true,\"gsAlpha\":\"0x%llX\",\"gsTest\":\"0x0\","
			"\"cmask\":[%d,%d,%d,%d]}\n",
			i, pass, m.matfx.pass2Env ? "true" : "false",
			static_cast<unsigned long long>(*m.matfx.pass2TexKey),
			static_cast<unsigned long long>(m.matfx.pass2Blend.reg.U64),
			gs.colorMask.r, gs.colorMask.g, gs.colorMask.b, gs.colorMask.a);
	}

	// One row per camera pass, before its draws. The pass is what a draw's coordinates and target mean
	// anything relative to, and its clear is a real contributor to the frame that no draw row records:
	// a scene whose background comes from the clear looked, in the manifest, like a background nothing
	// drew at all.
	static void writePassJson(const Frame& frame, FILE* f, const FramePass& p, int pi, bool dumped)
	{
		const CameraParts& c = p.cam;
		// The per-pass RGBA dump beside this manifest, or null for a pass that drew nothing and so
		// left its target exactly as the previous one did.
		char dumpName[32] = "null";
		if (dumped)
		{
			std::snprintf(dumpName, sizeof(dumpName), "\"pass%02d.png\"", pi);
		}
		// The VP every screen box on this pass's draws was projected through.
		std::fprintf(f, "{\"type\":\"pass\",\"pass\":%d,", pi);
		writeMat4(f, "vp", p.vp);
		std::fprintf(f,
			"\"pass\":%d,\"dump\":%s,\"target\":\"0x%08X\",\"camera\":\"0x%08X\","
			"\"size\":[%d,%d],\"display\":%s,\"draws\":%u,"
			"\"clear\":{\"color\":%s,\"depth\":%s,\"stencil\":%s,\"rgba\":[%.3f,%.3f,%.3f,%.3f]},"
			"\"cam\":{\"valid\":%s,\"proj\":%d,\"near\":%.1f,\"far\":%.1f,\"viewWindow\":[%.4f,%.4f],"
			"\"pos\":[%.1f,%.1f,%.1f]},"
			"\"fog\":{\"range\":[%.1f,%.1f],\"color\":[%.3f,%.3f,%.3f]},"
			"\"amb\":[%.3f,%.3f,%.3f],\"dirCount\":%d}\n",
			pi, dumpName, p.target, p.camera, p.targetW, p.targetH, p.isDisplay ? "true" : "false",
			p.cmdEnd - p.cmdBegin,
			p.clear.color ? "true" : "false", p.clear.depth ? "true" : "false",
			p.clear.stencil ? "true" : "false",
			p.clear.colorValue.r, p.clear.colorValue.g, p.clear.colorValue.b, p.clear.colorValue.a,
			c.valid ? "true" : "false", c.projectionType, c.nearPlane, c.farPlane,
			c.viewWindowX, c.viewWindowY, c.camWorld[3][0], c.camWorld[3][1], c.camWorld[3][2],
			p.lights.fogStart, p.lights.fogEnd, p.lights.fogColor.r, p.lights.fogColor.g,
			p.lights.fogColor.b, p.lights.ambient.r, p.lights.ambient.g, p.lights.ambient.b,
			p.lights.dirCount);
	}

	static void writeIm2dJson(const Renderer& renderer, const Frame& frame, FILE* f, const Im2DDraw& b,
		int i, int pass)
	{
		const DrawState& gs = frame.states[b.stateIndex];
		float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
		float aMin = 1e9f, aMax = -1e9f, aSum = 0.0f; // vertex alpha (0..1)
		float zMin = 1e30f, zMax = -1e30f; // the GS depth the game submitted
		float rSum = 0.0f, gSum = 0.0f, bSum = 0.0f; // vertex color mean (0..1)
		int nv = 0;
		for (uint32_t vi = 0; vi < b.vertCount; vi++)
		{
			const Im2DVertex& q = frame.im2dVerts[b.vertOffset + vi];
			x0 = std::min(x0, q.x);
			x1 = std::max(x1, q.x);
			y0 = std::min(y0, q.y);
			y1 = std::max(y1, q.y);
			aMin = std::min(aMin, q.a);
			aMax = std::max(aMax, q.a);
			zMin = std::min(zMin, q.z);
			zMax = std::max(zMax, q.z);
			aSum += q.a;
			nv++;
			rSum += q.r;
			gSum += q.g;
			bSum += q.b;
		}
		const float aMean = nv ? aSum / nv : 0.0f;
		const float rMean = nv ? rSum / nv : 0.0f, gMean = nv ? gSum / nv : 0.0f, bMean = nv ? bSum / nv : 0.0f;
		const bool textured = b.texKey && renderer.textures->lookup(*b.texKey) != 0;
		std::fprintf(f,
			"{\"type\":\"im2d\",\"i\":%d,\"pass\":%d,\"tex\":\"0x%016llX\",\"texPx\":\"0x%08X\","
			"\"pipe\":\"%s\",\"srcPrim\":%u,\"srcVerts\":%u,\"srcIdx\":%u,\"textured\":%s,\"nverts\":%u,"
			"\"bbox\":[%.1f,%.1f,%.1f,%.1f],\"blendEq\":\"0x%X\",\"src\":\"0x%X\",\"dst\":\"0x%X\","
			"\"atest\":%s,\"afail\":%d,\"tcc\":%d,\"cmask\":[%d,%d,%d,%d],\"samp\":\"0x%04X\",\"gsAlpha\":\"0x%llX\",\"gsTest\":\"0x%llX\",\"uvClamp\":[%.4f,%.4f,%.4f,%.4f],"
			"\"valpha\":[%.2f,%.2f,%.2f],\"vcol\":[%.2f,%.2f,%.2f],\"apremul\":%.2f,\"constA\":%.2f,"
			"\"z\":[%.1f,%.1f],\"ztest\":%s,\"zwrite\":%s}\n",
			i, pass, static_cast<unsigned long long>(b.texKey.value_or(0)),
			b.texKey ? renderer.textures->sourceAddress(*b.texKey) : 0u,
			b.source == Im2DSource::Rt2d ? "rt2d" : "im2d", b.srcPrim, b.srcVerts, b.srcIndices,
			textured ? "true" : "false", b.vertCount, x0, y0, x1, y1,
			gs.blend.equation, gs.blend.src, gs.blend.dst, gs.atest.enabled ? "true" : "false",
			gs.atest.afail,
			b.texKey && isFrameBufferFeedbackTexId(*b.texKey) ? 0 : 1, // GS TEX0.TCC
			gs.colorMask.r, gs.colorMask.g, gs.colorMask.b, gs.colorMask.a, b.sampler, static_cast<unsigned long long>(gs.gsAlphaReg), static_cast<unsigned long long>(gs.atest.reg.U64),
			gs.clamp.active ? gs.clamp.uvMin.x : 0.0f, gs.clamp.active ? gs.clamp.uvMin.y : 0.0f,
			gs.clamp.active ? gs.clamp.uvMax.x : 1.0f, gs.clamp.active ? gs.clamp.uvMax.y : 1.0f,
			aMin, aMean, aMax, rMean, gMean, bMean, gs.blend.alphaPremul, gs.blend.constAlpha,
			nv ? zMin : 0.0f, nv ? zMax : 0.0f, gs.ztest ? "true" : "false",
			gs.zwrite ? "true" : "false");
	}

	// Write a JSONL frame manifest (one object per line) to experiments/dumps/skygl_manifest.jsonl: every 3D
	// atomic draw + 2D draw with its texture, UV/pos bbox, blend, and geometry fields, in the same
	// submission order (pass by pass, meshes then 2D) that the unified draw id "i" and the render
	// walk use. One capture then explains what is bound where (bad UVs on a wall atomic pinpoint the
	// failing draw).
	void Renderer::debugWriteManifest(const Frame& frame, const char* path) const
	{
		const std::vector<FramePass>& passes = frame.passes;
		FILE* f = std::fopen(path, "w");
		if (!f)
		{
			return;
		}
		// v2: one line per command in submission order, with "i" the command's position in
		// Frame::commandList. That is the same id P3FES_DRAW_ONLY takes, and it is stable across the
		// whole frame rather than restarting per pass.
		// Per-pipeline totals in the header, because "the world did not draw" and "the world drew wrong"
		// look the same in a list of mesh rows: a scene with zero sector draws says which it is.
		size_t nMesh = 0, nIm2d = 0;
		size_t nAtomic = 0, nSector = 0, nIm3d = 0, nRt2d = 0, nNative = 0;
		for (const FramePass& p : passes)
		{
			for (uint32_t ci = p.cmdBegin; ci < p.cmdEnd; ci++)
			{
				const FrameCmd& c = frame.commandList[ci];
				if (c.type == CmdType::Im2d)
				{
					nIm2d++;
					if (frame.im2dCmds[c.data].source == Im2DSource::Rt2d)
						nRt2d++;
				}
				else
				{
					nMesh++;
					switch (frame.meshCmds[c.data].diag.source)
					{
						case MeshSource::Atomic: nAtomic++; break;
						case MeshSource::Sector: nSector++; break;
						case MeshSource::Im3D: nIm3d++; break;
					}
					if (frame.meshCmds[c.data].diag.native)
						nNative++;
				}
			}
		}
		std::fprintf(f,
			"{\"type\":\"frame\",\"v\":4,\"passes\":%zu,\"meshes\":%zu,\"im2d\":%zu,"
			"\"atomic\":%zu,\"sector\":%zu,\"im3d\":%zu,\"rt2d\":%zu,\"native\":%zu,"
			"\"im3dDropped\":{\"noCamera\":%u,\"noVerts\":%u,\"badCount\":%u,\"degenerate\":%u},"
			"\"calls\":{\"atomics\":%u,\"sectors\":%u,\"atomicsNoDraw\":%u,\"sectorsNoDraw\":%u}}\n",
			passes.size(), nMesh, nIm2d, nAtomic, nSector, nIm3d, nRt2d, nNative,
			frame.im3dDrops.noCamera, frame.im3dDrops.noVerts, frame.im3dDrops.badCount,
			frame.im3dDrops.degenerate, frame.objectCalls.atomics, frame.objectCalls.sectors,
			frame.objectCalls.atomicsNoDraw, frame.objectCalls.sectorsNoDraw);
		for (size_t pi = 0; pi < passes.size(); pi++)
		{
			const FramePass& p = passes[pi];
			const bool dumped = p.cmdEnd > p.cmdBegin || p.clear.color || p.clear.depth ||
			                    p.clear.stencil || !p.isDisplay;
			writePassJson(frame, f, p, static_cast<int>(pi), dumped);
			for (uint32_t ci = p.cmdBegin; ci < p.cmdEnd; ci++)
			{
				const FrameCmd& c = frame.commandList[ci];
				if (c.type == CmdType::Mesh)
				{
					writeMeshJson(*this, frame, f, frame.meshCmds[c.data], static_cast<int>(ci),
						static_cast<int>(pi), p.vp);
					// Straight after its base pass, in the order the renderer draws them.
					writePass2Json(frame, f, frame.meshCmds[c.data], static_cast<int>(ci), static_cast<int>(pi));
				}
				else
					writeIm2dJson(*this, frame, f, frame.im2dCmds[c.data], static_cast<int>(ci),
						static_cast<int>(pi));
			}
		}
		// The emulated GS's own record of the same frame, appended as "gs" rows. Having both in one
		// file is what makes a comparison a read rather than a correlation: the two used to be
		// separate files whose frames had to be lined up by hand.
		std::fclose(f);
		// Appended by path rather than through the open handle: a FILE* does not cross a library
		// boundary safely, since the two halves need not share a C runtime.
		const size_t nGs = sky2::host().writeGsRows(path);
		std::fprintf(stderr, "[sky2] wrote %s (%zu passes, %zu meshes, %zu 2D, %zu GS batches)\n",
			path, passes.size(), nMesh, nIm2d, nGs);
	}

	namespace
	{
		void putBE32(std::vector<uint8_t>& out, uint32_t v)
		{
			out.push_back(uint8_t(v >> 24));
			out.push_back(uint8_t(v >> 16));
			out.push_back(uint8_t(v >> 8));
			out.push_back(uint8_t(v));
		}

		void putChunk(std::FILE* f, const char tag[4], const std::vector<uint8_t>& body)
		{
			std::vector<uint8_t> head;
			putBE32(head, uint32_t(body.size()));
			std::fwrite(head.data(), 1, head.size(), f);
			std::fwrite(tag, 1, 4, f);
			std::fwrite(body.data(), 1, body.size(), f);
			uLong crc = crc32(0, reinterpret_cast<const Bytef*>(tag), 4);
			crc = crc32(crc, body.data(), uInt(body.size()));
			std::vector<uint8_t> tail;
			putBE32(tail, uint32_t(crc));
			std::fwrite(tail.data(), 1, tail.size(), f);
		}
	} // namespace

	// The currently-bound framebuffer as an RGBA PNG, at the end of a pass. Written for every pass a
	// capture renders, because the alpha channel has no other record: it is what dest-alpha effects
	// blend against, it never reaches the screenshot, and a wrong one reads as a colour bug. PNG
	// rather than a raw format because a 49-scene sweep writes a few hundred of these; uncompressed
	// they came to over a gigabyte and filled the disk.
	void Renderer::dumpPassImage(const std::string& base, int pass, int w, int h) const
	{
		if (w <= 0 || h <= 0)
		{
			return;
		}
		const size_t pixels = static_cast<size_t>(w) * h;
		std::vector<uint8_t> rgba(pixels * 4);
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
		// PNG rows are top-down and each carries a filter byte; GL reads bottom-up.
		const size_t stride = static_cast<size_t>(w) * 4;
		std::vector<uint8_t> raw;
		raw.reserve((stride + 1) * static_cast<size_t>(h));
		for (int y = h - 1; y >= 0; y--)
		{
			raw.push_back(0); // filter: none
			raw.insert(raw.end(), rgba.begin() + static_cast<size_t>(y) * stride,
				rgba.begin() + static_cast<size_t>(y) * stride + stride);
		}
		uLongf bound = compressBound(uLong(raw.size()));
		std::vector<uint8_t> z(bound);
		if (compress2(z.data(), &bound, raw.data(), uLong(raw.size()), Z_BEST_SPEED) != Z_OK)
		{
			SKY2_ERROR(false, "zlib compress failed for pass %d", pass);
			return;
		}
		z.resize(bound);
		char path[512];
		std::snprintf(path, sizeof(path), "%s-pass%02d.png", base.c_str(), pass);
		FILE* f = std::fopen(path, "wb");
		if (!f)
		{
			SKY2_ERROR(false, "fopen(%s) failed", path);
			return;
		}
		static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
		std::fwrite(SIG, 1, sizeof(SIG), f);
		std::vector<uint8_t> ihdr;
		putBE32(ihdr, uint32_t(w));
		putBE32(ihdr, uint32_t(h));
		ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit, RGBA, deflate, no filter, no interlace
		putChunk(f, "IHDR", ihdr);
		putChunk(f, "IDAT", z);
		putChunk(f, "IEND", {});
		std::fclose(f);
	}

	void Renderer::writePPM(const char* path) const
	{
		// Read the display FBO, not the default framebuffer. The present blit has just left the window
		// bound, and the window drawable does not always have the render's size: a PAL disc renders
		// 640x512 into a drawable still 640x448 tall, and reading the taller rect off it returned 64
		// rows of black. What the capture is comparing is the render, which is what this holds.
		const int w = gl.displayW, h = gl.displayH;
		glBindFramebuffer(GL_READ_FRAMEBUFFER, gl.displayFbo);
		std::vector<uint8_t> px(static_cast<size_t>(w) * h * 3);
		glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		if (FILE* f = std::fopen(path, "wb"))
		{
			std::fprintf(f, "P6\n%d %d\n255\n", w, h);
			for (int y = h - 1; y >= 0; y--)
				std::fwrite(&px[static_cast<size_t>(y) * w * 3], 1, w * 3, f);
			std::fclose(f);
			std::fprintf(stderr, "[sky2] wrote %s (%dx%d)\n", path, w, h);
		}
		else
		{
			SKY2_ERROR(false, "fopen(%s) failed", path);
		}
	}
} // namespace sky2
