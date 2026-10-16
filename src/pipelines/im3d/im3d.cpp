// Im3D pipeline (A2): projected 3D primitives (the field character-shadow quad; shuffle-time cards).
// transform() stashes the object-space vertex array + object->world matrix; a render call expands it
// into a MeshDraw at the pass VP * ltm, unlit (color = texture x vertex color), depth-tested, current
// blend; end() clears the stash. renderPrimitive draws the verts in order; renderIndexedPrimitive
// draws through an index buffer (the shuffle cards use TRILIST + 6 idx). Reaches the queue and the
// texture cache through its injected Device.
#include "pipelines/im3d/im3d.h"

#include "device.h"
#include "common.h"
#include "object_manager.h"
#include "render_queue.h"
#include "renderer.h"
#include "rw_to_gl.h"
#include "texture_manager.h"

#include <cstdio>
#include <cstdlib>

namespace sky2
{
	// How much transient geometry the frame carries, and whether any of it is placed by a matrix rather
	// than by the vertex data. EE thread only.
	void Im3DPipeline::noteBatch(int count, const glm::mat4& model)
	{
		GeomStats& stats = device->geometry.stats;
		if (model == glm::mat4(1.0f))
		{
			bumpStat(stats.im3dIdentityModel);
		}
		bumpStat(stats.im3dBatches);
		bumpStat(stats.im3dBytes, static_cast<size_t>(count) * sizeof(RwIm3DVertex));
	}

	glm::mat4 Im3DPipeline::loadLtm(uint32_t ltmAddr)
	{
		if (!ltmAddr)
		{
			return glm::mat4(1.0f);
		}
		const RwMatrixTag& m = *guest::ptr<RwMatrixTag>(ltmAddr);
		return glm::mat4(m.right.x, m.right.y, m.right.z, 0.0f, m.up.x, m.up.y, m.up.z, 0.0f,
			m.at.x, m.at.y, m.at.z, 0.0f, m.pos.x, m.pos.y, m.pos.z, 1.0f);
	}

	void Im3DPipeline::transform(uint32_t vertsAddr, int numVerts, uint32_t ltmAddr, unsigned flags)
	{
		RenderQueue::Im3DBuild& stash = device->queue.im3d;
		stash.vertsAddr = vertsAddr;
		stash.count = numVerts;
		stash.haveUV = (flags & rwIM3D_VERTEXUV) != 0; // reported only; see the UV read below
		stash.ltmAddr = ltmAddr;
		stash.flags = flags;
		stash.model = loadLtm(ltmAddr); // object->world; the pass VP is applied on the render thread
	}

	bool Im3DPipeline::beginMesh(MeshDraw& m)
	{
		RenderQueue& queue = device->queue;
		RenderQueue::Im3DBuild& stash = queue.im3d;
		Frame::Im3DDrops& drops = queue.buildFrame().im3dDrops;
		if (!queue.haveCam)
		{
			drops.noCamera++;
			return false;
		}
		if (!stash.vertsAddr)
		{
			drops.noVerts++;
			return false;
		}
		if (stash.count <= 0 || stash.count > 4096)
		{
			drops.badCount++;
			return false;
		}
		m.model = stash.model;
		m.diag.source = MeshSource::Im3D;
		m.diag.ltmAddr = stash.ltmAddr;
		m.diag.im3dFlags = stash.flags;
		m.material.unlit = true; // color = texture x vertex color, no scene lighting
		m.material.lit = false;
		m.material.vcolor = false;
		m.boneCount = 0; // Im3D primitives are never skinned
		m.material.color = glm::vec4(1.0f);
		// Im3D primitives (shadow/card/effect quads) composite with the current reconciled GS blend.
		DrawState st;
		st.blend = queue.blendState();
		st.blend.enable = true;
		st.stencil = queue.stencil;
		// Im3D has no geometry of its own to carry a cull mode, so it takes the render state like every
		// other path. Hardcoding CULLNONE here drew the shuffle cards' back art over their fronts: they
		// are stacked quads at one screen box with no depth test, so whichever draws last wins.
		st.cull = queue.rsHits[rwRENDERSTATECULLMODE]
			? static_cast<int>(queue.renderState[rwRENDERSTATECULLMODE])
			: rwCULLMODECULLNONE;
		st.ztest = queue.rsHits[rwRENDERSTATEZTESTENABLE] ? queue.renderState[rwRENDERSTATEZTESTENABLE] != 0 : true;
		st.zwrite = queue.rsHits[rwRENDERSTATEZWRITEENABLE] ? queue.renderState[rwRENDERSTATEZWRITEENABLE] != 0 : false;
		st.fog = false;
		st.colorMask = queue.colorMask;
		st.clamp = queue.gsClamp;
		st.gsAlphaReg = queue.gsBlend.reg.U64;
		m.stateIndex = queue.stateIndexFor(st);
		m.lightsIndex = queue.lightsIndexFor(FrameLights{}); // unlit: the shader ignores these
		m.material.sampler = device->im2dFilterAddressing();
		m.material.texKey = device->textures.convertRaster(queue.renderState[rwRENDERSTATETEXTURERASTER],
			m.material.sampler);
		noteBatch(stash.count, stash.model);
		// The arena path appends straight into the frame; the reference path fills a pool buffer per
		// batch, which is a GPU buffer per batch per frame.
		const bool arena = sky2::config().geomArena;
		Frame& frame = queue.buildFrame();
		std::vector<Vertex3D> scratch;
		if (arena)
		{
			m.vertOffset = static_cast<uint32_t>(frame.transientVerts.size());
			m.vertCount = static_cast<uint32_t>(stash.count);
			frame.transientVerts.resize(m.vertOffset + m.vertCount, Vertex3D{});
		}
		else
		{
			scratch = device->geometry.verts.scratch();
			scratch.assign(static_cast<size_t>(stash.count), Vertex3D{});
		}
		std::vector<Vertex3D>& vbuf = arena ? frame.transientVerts : scratch;
		const size_t vbase = arena ? m.vertOffset : 0;
		for (int i = 0; i < stash.count; i++)
		{
			const RwIm3DVertex& s =
				*guest::ptr<RwIm3DVertex>(stash.vertsAddr + static_cast<uint32_t>(i) * sizeof(RwIm3DVertex));
			Vertex3D& v = vbuf[vbase + static_cast<size_t>(i)];
			v.pos = glm::vec3(s.objVertex.x, s.objVertex.y, s.objVertex.z);
			// Unconditionally, not gated on rwIM3D_VERTEXUV: the PS2 Im3D vertex always carries u/v
			// and the Sky pipeline passes them through whatever the transform flags say. P4's shadow
			// projection submits flags 0x19, no UV bit, and still needs its texcoords. Gating on the
			// flag was tried and is worse on exactly the pixels it moves: p3-tartarus-entrance 14.07
			// to 16.49, p3-font 0.667 to 1.000.
			v.uv = glm::vec2(s.u, s.v);
			v.color = toVec4(s.color); // 0..255 app-space, like Im2D
		}
		static const bool sLogIm3D = sky2::env("LOG_IM3D") != nullptr;
		if (sLogIm3D)
		{
			std::fprintf(stderr, "[sky2][im3d] n=%d tex=0x%016llX uv=%d src=0x%X dst=0x%X:\n", stash.count,
				static_cast<unsigned long long>(m.material.texKey.value_or(0)), stash.haveUV ? 1 : 0,
				st.blend.src, st.blend.dst);
			for (size_t vi = 0; vi < static_cast<size_t>(stash.count) && vi < 8; vi++)
			{
				const Vertex3D& v = vbuf[vbase + vi];
				std::fprintf(stderr, "    v%zu pos=(%.1f,%.1f,%.1f) uv=(%.3f,%.3f) col=(%.2f,%.2f,%.2f,%.2f)\n",
					vi, v.pos.x, v.pos.y, v.pos.z, v.uv.x, v.uv.y, v.color.r, v.color.g, v.color.b, v.color.a);
			}
		}
		// Arena meshes leave verts at GEOM_ID_NONE: the slice fields are what names them.
		if (!arena)
		{
			m.verts = device->geometry.verts.create(std::move(scratch), true);
		}
		return true;
	}

	// A batch abandoned after beginMesh already appended its vertices. Nothing else has appended since,
	// so give the arena space back rather than leaving it to be uploaded and never drawn.
	void Im3DPipeline::discardMesh(const MeshDraw& m)
	{
		if (sky2::config().geomArena)
		{
			device->queue.buildFrame().transientVerts.resize(m.vertOffset);
		}
	}

	// Append this batch's indices, either to the frame arena or to a pool buffer of its own. Indices
	// stay mesh-local either way; the arena draw adds the base vertex.
	bool Im3DPipeline::finishMesh(MeshDraw& m, std::vector<uint16_t>&& idx)
	{
		if (idx.empty())
		{
			device->queue.buildFrame().im3dDrops.degenerate++;
			discardMesh(m);
			return false;
		}
		if (sky2::config().geomArena)
		{
			Frame& frame = device->queue.buildFrame();
			m.idxOffset = static_cast<uint32_t>(frame.transientIdx.size());
			m.idxCount = static_cast<uint32_t>(idx.size());
			frame.transientIdx.insert(frame.transientIdx.end(), idx.begin(), idx.end());
		}
		else
		{
			m.idx = device->geometry.indices.create(std::move(idx), true);
		}
		return true;
	}

	void Im3DPipeline::renderPrimitive(uint32_t primType)
	{
		MeshDraw m;
		if (!beginMesh(m))
		{
			return;
		}
		std::vector<uint16_t> idx = device->geometry.indices.scratch();
		expandTrianglePrimitive("Im3D", primType, device->queue.im3d.count,
			[&](int i) { idx.push_back(static_cast<uint16_t>(i)); });
		if (finishMesh(m, std::move(idx)))
		{
			device->queue.submitMesh(std::move(m));
		}
	}

	// RwIm3DRenderLine(v1, v2): one line by index into the stashed vertices. No primitive type and no
	// index buffer, so it cannot go through the expansion the other two share. The game draws debug
	// lines with it (primLine3D transforms two vertices and calls this with 0 and 1).
	void Im3DPipeline::renderLine(int vert1, int vert2)
	{
		MeshDraw m;
		if (!beginMesh(m))
		{
			return;
		}
		m.lineList = true;
		const int vertCount = device->queue.im3d.count;
		std::vector<uint16_t> idx = device->geometry.indices.scratch();
		for (const int vi : {vert1, vert2})
		{
			if (vi < 0 || vi >= vertCount)
			{
				SKY2_ERROR(false, "Im3D line index %d out of range (numVerts=%d)", vi, vertCount);
				discardMesh(m);
				return;
			}
			idx.push_back(static_cast<uint16_t>(vi));
		}
		if (finishMesh(m, std::move(idx)))
		{
			device->queue.submitMesh(std::move(m));
		}
	}

	// RwIm3DRenderIndexedPrimitive(primType, indices, numIndices): draw the stashed verts through an
	// index buffer (RwImVertexIndex = uint16). The shuffle-time cards use this (TRILIST, 6 indices).
	void Im3DPipeline::renderIndexedPrimitive(uint32_t primType, uint32_t indicesAddr, int numIndices)
	{
		MeshDraw m;
		if (!beginMesh(m))
		{
			return;
		}
		if (!indicesAddr || numIndices <= 0 || numIndices > 8192)
		{
			SKY2_ERROR(false, "Im3D indexed bad args (idx=0x%X n=%d)", indicesAddr, numIndices);
			discardMesh(m);
			return;
		}
		const auto idxBuf = guest::ptr<uint16_t>(indicesAddr);
		const int vertCount = device->queue.im3d.count;
		std::vector<uint16_t> idx = device->geometry.indices.scratch();
		expandTrianglePrimitive("Im3D", primType, numIndices, [&](int k) {
			const uint16_t vi = idxBuf[k];
			if (vi < vertCount)
			{
				idx.push_back(vi);
			}
			else
			{
				SKY2_ERROR(false, "Im3D index %u out of range (numVerts=%d)", vi, vertCount);
			}
		});
		// expandTrianglePrimitive emits corners one at a time; a dropped OOB index would misalign the
		// triangle list. Only push if the count stayed a whole number of triangles.
		if (idx.size() % 3 != 0)
		{
			discardMesh(m);
			return;
		}
		if (finishMesh(m, std::move(idx)))
		{
			device->queue.submitMesh(std::move(m));
		}
	}

	void Im3DPipeline::end()
	{
		RenderQueue::Im3DBuild& stash = device->queue.im3d;
		stash.vertsAddr = 0;
		stash.count = 0;
	}
} // namespace sky2
