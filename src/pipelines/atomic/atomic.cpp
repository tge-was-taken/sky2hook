// Atomic pipeline: the RpAtomic model geometry (characters, props, the skybox) the game renders via
// AtomicDefaultRenderCallBack. Reads the atomic's geometry out of EE RAM into the shared
// ParsedGeometry (skinning, MatFX dual-texture, per-material submeshes) and enqueues its submeshes
// through the shared geometry back-end (enqueueSubMesh). Stateless per call.
#include "pipelines/atomic/atomic.h"

#include "device.h"
#include "common.h"
#include "object_manager.h"
#include "render_queue.h"
#include "renderer.h"
#include "rw_to_gl.h"
#include "texture_manager.h"

#include "sky2/config.h" // sky2::config().geomCache (debug toggle)
#include "geometry.h"
#include "material.h"
#include "pipelines/atomic/ptank.h"
#include "pipelines/sector/native_data.h"

#include <vector>

namespace sky2
{
	static uint32_t getGeometrySkin(uint32_t geometryAddr)
	{
		const uint32_t off = sky2::pluginOffset(rwGEOMETRYBASE, rwID_SKINPLUGIN, sky2::addresses().RpSkinGeometryDataOffset);
		return off ? *guest::ptr<uint32_t>(geometryAddr + off) : 0;
	}

	static uint32_t getAtomicHierarchy(uint32_t atomicAddr)
	{
		const uint32_t off = sky2::pluginOffset(rwATOMICBASE, rwID_SKINPLUGIN, sky2::addresses().RpSkinAtomicDataOffset);
		return off ? *guest::ptr<uint32_t>(atomicAddr + off) : 0;
	}

	static bool matfxAtomicEnabled(uint32_t atomicAddr)
	{
		const uint32_t aoff = sky2::pluginOffset(rwATOMICBASE, rwID_MATERIALEFFECTSPLUGIN, sky2::addresses().MatFXAtomicDataOffset);
		return aoff ? *guest::ptr<int32_t>(atomicAddr + aoff) != 0 : false;
	}

	// Per-atomic skinning: where this atomic's (currentPose * inverseBind) matrices landed in the
	// frame's bone arena. numBones 0 => the HAnim hierarchy didn't resolve, not necessarily unskinned:
	// the caller still gates on whether the geometry has a per-vertex binding to weight by.
	struct SkinBinding
	{
		uint32_t boneOffset = 0;
		int numBones = 0;
	};

	// Appends into frame.bones rather than returning a vector: the palette used to be built once, then
	// copied into the ParsedGeometry and again into every submesh of the object.
	static SkinBinding readSkin(uint32_t atomicAddr, uint32_t geometryAddr, Frame& frame)
	{
		SkinBinding sb;
		const uint32_t skinAddr = getGeometrySkin(geometryAddr);
		const uint32_t hierAddr = getAtomicHierarchy(atomicAddr);
		if (!skinAddr || !hierAddr)
		{
			return sb; // RpSkin plugin not attached to this atomic: not skinned, not an error
		}
		const auto skin = guest::ptr<RpSkin>(skinAddr);
		const auto hier = guest::ptr<RpHAnimHierarchy>(hierAddr);
		int numBones = static_cast<int>(skin->boneData.numBones);
		const uint32_t invMats = skin->boneData.invBoneToSkinMat.addr;
		const uint32_t poseMats = hier->pMatrixArray.addr;
		if (numBones <= 0 || !invMats || !poseMats)
		{
			SKY2_ERROR(false, "skin plugin attached but numBones=%d invMats=0x%X poseMats=0x%X",
				numBones, invMats, poseMats);
			return sb;
		}
		// MAX_BONES stays the per-skin cap even though the arena is now sized per frame: the per-vertex
		// bone index is clamped against it below, and raising it would change what an over-range skin
		// renders as.
		if (numBones > MAX_BONES)
		{
			SKY2_ERROR(false, "numBones %d exceeds MAX_BONES %d, clamping", numBones, MAX_BONES);
			numBones = MAX_BONES;
		}
		sb.boneOffset = static_cast<uint32_t>(frame.bones.size());
		frame.bones.resize(sb.boneOffset + static_cast<size_t>(numBones));
		if (frame.wantPoses)
		{
			frame.poses.resize(frame.bones.size());
			frame.invBinds.resize(frame.bones.size());
		}
		for (int b = 0; b < numBones; b++)
		{
			const glm::mat4 inv = toMat4(*guest::ptr<RwMatrixTag>(invMats + b * sizeof(RwMatrixTag)));
			const glm::mat4 pose = toMat4(*guest::ptr<RwMatrixTag>(poseMats + b * sizeof(RwMatrixTag)));
			const size_t at = sb.boneOffset + static_cast<size_t>(b);
			frame.bones[at] = pose * inv;
			if (frame.wantPoses)
			{
				frame.poses[at] = pose;
				frame.invBinds[at] = inv;
			}
		}
		sb.numBones = numBones;
		return sb;
	}

	// Position of a material in the geometry's list, which is how CachedGeom::Sub names it. Native
	// meshes carry a material pointer instead of an index.
	static int materialIndex(const RpMaterialList& matList, uint32_t materialAddr)
	{
		for (int i = 0; materialAddr && i < matList.numMaterials; i++)
		{
			if (matList.materials[i].addr == materialAddr)
			{
				return i;
			}
		}
		return -1;
	}

	bool AtomicPipeline::parseGeomNative(uint32_t geomAddr, uint32_t nativeTable, ObjectManager::CachedGeom& out, bool perFrame)
	{
		const auto geometry = guest::ptr<RpGeometry>(geomAddr);
		const auto mh = geometry->mesh;
		const int numMeshes = mh ? mh->numMeshes : 0;
		if (numMeshes == 0)
		{
			return false;
		}
		out.native = true;
		std::vector<Vertex3D> vbuf = device->geometry.verts.scratch();
		std::vector<std::pair<uint32_t, uint32_t>> ranges(static_cast<size_t>(numMeshes)); // base, count
		bool anyColor = false, anyNormal = false, anyUV = false, anyBone = false;
		// Bone count for clamping only; the matrices themselves are per-frame (readSkin, at render time).
		int skinNumBones = 0;
		if (const uint32_t skinAddr = getGeometrySkin(geomAddr))
		{
			skinNumBones = static_cast<int>(guest::ptr<RpSkin>(skinAddr)->boneData.numBones);
			if (skinNumBones > MAX_BONES)
			{
				SKY2_ERROR(false, "numBones %d exceeds MAX_BONES %d, clamping", skinNumBones, MAX_BONES);
				skinNumBones = MAX_BONES;
			}
		}
		for (int m = 0; m < numMeshes; m++)
		{
			NativeMesh nm;
			const uint32_t base = static_cast<uint32_t>(vbuf.size());
			if (!decodeNativeMesh(nativeTable, m, nm) || base + nm.pos.size() > 0xFFFF)
			{
				// Silently dropping these hid nine tenths of Sonic Heroes' geometry: 13905 indices
				// against the GS's 138360, with nothing in the log to say a mesh had been skipped.
				// The unpack formats say why, so print a handful of chains alongside.
				SKY2_TRACE_ONCE(geomAddr, "geometry 0x%X: native mesh %d of %d did not decode",
					geomAddr, m, numMeshes);
				if (static int logged = 0; logged < 8)
				{
					logged++;
					logNativeMesh(nativeTable, m);
				}
				ranges[m] = {base, 0}; // undecodable, or the geometry overflows 16-bit indices
				continue;
			}
			anyColor = anyColor || !nm.color.empty();
			anyNormal = anyNormal || !nm.normal.empty();
			anyUV = anyUV || !nm.uv.empty();
			anyBone = anyBone || (!nm.boneIndex.empty() && skinNumBones > 0);
			for (size_t i = 0; i < nm.pos.size(); i++)
			{
				Vertex3D vtx{};
				vtx.pos = nm.pos[i];
				if (i < nm.uv.size())
				{
					vtx.uv = nm.uv[i];
				}
				if (i < nm.color.size())
				{
					vtx.color = nm.color[i];
				}
				if (i < nm.normal.size())
				{
					vtx.normal = nm.normal[i];
				}
				if (i < nm.boneIndex.size() && skinNumBones > 0)
				{
					// Clamp against this skin's own bone count, like the static path: a guest index
					// between numBones and MAX_BONES would otherwise read an OOB bone matrix.
					vtx.boneIndex = glm::clamp(nm.boneIndex[i], 0.0f, static_cast<float>(skinNumBones - 1));
					vtx.boneWeight = nm.boneWeight[i];
				}
				vbuf.push_back(vtx);
			}
			ranges[m] = {base, static_cast<uint32_t>(nm.pos.size())};
		}
		if (vbuf.empty())
		{
			return false;
		}

		out.geomFlags = geometry->flags;
		out.lit = (geometry->flags & rpGEOMETRYLIGHT) != 0;
		out.hasNormals = anyNormal;
		out.hasVertexColor = anyColor;
		out.hasSkinData = anyBone;
		out.numTexCoordSets = anyUV ? 1 : 0;
		out.vcount = static_cast<int>(vbuf.size());

		const RpMaterialList& matList = geometry->matList;
		const NativePrim prim = nativePrimFor(mh->flags);
		const uint32_t firstMesh = mh.addr + sizeof(RpMeshHeader) + mh->firstMeshOffset;
		for (int m = 0; m < numMeshes; m++)
		{
			if (ranges[m].second < 3)
			{
				continue;
			}
			const auto mesh = guest::ptr<RpMesh>(firstMesh + m * static_cast<int>(sizeof(RpMesh)));
			std::vector<uint16_t> idx = device->geometry.indices.scratch();
			appendNativePrims(idx, vbuf, ranges[m].first, ranges[m].second, prim);
			if (!idx.empty())
			{
				out.subs.push_back({materialIndex(matList, mesh->material.addr),
					device->geometry.indices.create(std::move(idx), perFrame)});
			}
		}
		if (out.subs.empty())
		{
			return false; // nothing referenced the vertices, so do not hand them to the pool
		}
		out.verts = device->geometry.verts.create(std::move(vbuf), perFrame);
		// RW's vertex-alpha term, computed once here rather than rescanned per draw every frame.
		if (out.hasVertexColor)
		{
			const std::span<const Vertex3D> pv = device->geometry.verts.view(out.verts);
			for (auto& sub : out.subs)
				sub.prelitAlpha = anyPrelitAlpha(pv, device->geometry.indices.view(sub.indices));
		}
		return true;
	}

	bool AtomicPipeline::parseGeomStatic(uint32_t geomAddr, ObjectManager::CachedGeom& out, bool perFrame)
	{
		const auto geometry = guest::ptr<RpGeometry>(geomAddr);
		const int numVertices = geometry->numVertices;
		const int numTriangles = geometry->numTriangles;
		if (numVertices == 0 && numTriangles == 0)
		{
			// Pre-instanced geometry keeps its triangles only in the VIF table and zeroes these counts,
			// so the counts alone cannot tell it from a trigger or marker atomic that draws nothing.
			if (const uint32_t nativeTable = geometryNativeTable(geomAddr))
			{
				return parseGeomNative(geomAddr, nativeTable, out, perFrame);
			}
			if (sky2::env("LOG_NODRAW"))
			{
				SKY2_TRACE_ONCE(geomAddr,
					"geometry 0x%X empty: flags=0x%X morphTargets=%d mats=%d mt=0x%X tris=0x%X repEntry=0x%X",
					geomAddr, geometry->flags, static_cast<int>(geometry->numMorphTargets),
					geometry->matList.numMaterials, geometry->morphTarget.addr,
					geometry->triangles.addr, geometry->repEntry.addr);
			}
			return false;
		}
		if (numVertices <= 0 || numVertices > 200000 || numTriangles <= 0 || numTriangles > 400000)
		{
			SKY2_ERROR(false, "geometry 0x%X has out-of-range counts numVertices=%d numTriangles=%d",
				geomAddr, numVertices, numTriangles);
			return false;
		}
		const auto morphTarget = geometry->morphTarget;
		if (!morphTarget)
		{
			SKY2_ERROR(false, "geometry 0x%X has no morphTarget", geomAddr);
			return false;
		}
		const auto verts = morphTarget->verts;
		const auto normals = morphTarget->normals;
		const auto triangles = geometry->triangles;
		if (!verts || !triangles)
		{
			// No platform-independent arrays: the geometry is pre-instanced, if this build ships it.
			if (const uint32_t nativeTable = geometryNativeTable(geomAddr))
			{
				return parseGeomNative(geomAddr, nativeTable, out, perFrame);
			}
			SKY2_ERROR(false, "geometry 0x%X missing verts/triangles pointers", geomAddr);
			return false;
		}
		// Lit regime is gated by rpGEOMETRYLIGHT (RW 7.2), not by the mere presence of normals; the
		// scene always carries at least a default ambient light, so the ">=1 light" clause holds.
		// Normals separately gate the directional term inside the lit regime.
		out.hasNormals = static_cast<bool>(normals);
		out.lit = (geometry->flags & rpGEOMETRYLIGHT) != 0;
		const auto texCoords = geometry->texCoords[0];
		const bool hasUV = geometry->numTexCoordSets > 0 && static_cast<bool>(texCoords);
		const auto texCoords1 = geometry->texCoords[1]; // second set, for the MatFX dual pass
		const bool hasUV2 = geometry->numTexCoordSets > 1 && static_cast<bool>(texCoords1);
		out.geomFlags = geometry->flags;
		out.numTexCoordSets = geometry->numTexCoordSets;
		out.texCoordsAddr = texCoords.addr;
		const auto preLit = geometry->preLitLum;
		out.hasVertexColor = (geometry->flags & rpGEOMETRYPRELIT) && static_cast<bool>(preLit);

		// Geometry skin plugin: the per-vertex bone indices/weights are geometry-static (the bone matrices
		// are per-frame, read in render). Write them whenever the plugin is present; a non-skinned atomic
		// using this geometry leaves MeshDraw.skinned off, so the shader ignores them.
		int skinNumBones = 0;
		guest::ptr<uint32_t> skinIndices{0};
		guest::ptr<RwMatrixWeights> skinWeights{0};
		if (const uint32_t skinAddr = getGeometrySkin(geomAddr))
		{
			const auto skin = guest::ptr<RpSkin>(skinAddr);
			skinNumBones = static_cast<int>(skin->boneData.numBones);
			if (skinNumBones > MAX_BONES)
			{
				SKY2_ERROR(false, "numBones %d exceeds MAX_BONES %d, clamping", skinNumBones, MAX_BONES);
				skinNumBones = MAX_BONES;
			}
			skinIndices = skin->vertexMaps.matrixIndices;
			skinWeights = skin->vertexMaps.matrixWeights;
		}
		out.hasSkinData = skinIndices && skinWeights && skinNumBones > 0;

		// No V flip: decode row 0 is the image top and GL samples t=0 from data row 0 (RW top-left).
		out.vcount = numVertices;
		// assign, not resize: a recycled buffer keeps its old elements, and the loop below writes
		// normals/uv/color/skin only when the geometry has them.
		std::vector<Vertex3D> vbuf = device->geometry.verts.scratch();
		vbuf.assign(static_cast<size_t>(numVertices), Vertex3D{});
		for (int i = 0; i < numVertices; i++)
		{
			Vertex3D& vtx = vbuf[i];
			const RwV3d& p = verts[i];
			vtx.pos = glm::vec3(p.x, p.y, p.z);
			if (out.hasNormals)
			{
				const RwV3d& n = normals[i];
				vtx.normal = glm::vec3(n.x, n.y, n.z);
			}
			if (hasUV)
			{
				const RwTexCoords& uv = texCoords[i];
				vtx.uv = glm::vec2(uv.u, uv.v);
			}
			if (hasUV2)
			{
				const RwTexCoords& uv = texCoords1[i];
				vtx.uv2 = glm::vec2(uv.u, uv.v);
			}
			if (out.hasVertexColor)
			{
				// Raw normalized color; the PS2 0x80=1.0 scaling is applied in the shader (init3D),
				// so all color scaling lives in one place.
				vtx.color = toVec4(preLit[i]);
			}
			if (out.hasSkinData)
			{
				const uint32_t packed = skinIndices[i];
				const RwMatrixWeights& w = skinWeights[i];
				// Clamp to numBones-1 (the last matrix actually uploaded for THIS skin), not
				// MAX_BONES-1: when numBones < MAX_BONES a guest index between them would pass a
				// MAX_BONES-1 clamp but still read an OOB bone matrix in the shader.
				const uint32_t maxBoneIdx = static_cast<uint32_t>(skinNumBones - 1);
				auto clampBoneIdx = [&](uint32_t bi) -> uint32_t {
					if (bi > maxBoneIdx)
					{
						SKY2_ERROR(false, "vertex bone index %u exceeds numBones-1 (%u)",
							bi, maxBoneIdx);
						return maxBoneIdx;
					}
					return bi;
				};
				vtx.boneIndex = glm::vec4(
					clampBoneIdx(packed & 0xFF),
					clampBoneIdx((packed >> 8) & 0xFF),
					clampBoneIdx((packed >> 16) & 0xFF),
					clampBoneIdx((packed >> 24) & 0xFF));
				vtx.boneWeight = glm::vec4(w.w0, w.w1, w.w2, w.w3);
			}
		}
		out.verts = device->geometry.verts.create(std::move(vbuf), perFrame);

		RpMaterialList& matList = geometry->matList;
		const int numMat = matList.numMaterials;
		// numMat drives both a loop bound and a materials[] index; cap against a garbage-large guest count.
		constexpr int MAX_MATERIALS = 1024;
		if (numMat < 0 || numMat > MAX_MATERIALS)
		{
			SKY2_ERROR(false, "matList.numMaterials %d out of sane range (0..%d)", numMat, MAX_MATERIALS);
			// The vertex slot is already claimed and the caller discards `out` on false, so hand it back
			// here: a cached parse is not per-frame, and the pool would hold it for the run.
			device->objects.releaseGeom(out);
			return false;
		}

		// Group triangles into per-material index lists (matIndex -1 = the single/no-material case).
		auto appendTri = [&](std::vector<uint16_t>& idx, int i) {
			const RpTriangle& t = triangles[i];
			const uint32_t i0 = t.vertIndex[0], i1 = t.vertIndex[1], i2 = t.vertIndex[2];
			if (i0 >= static_cast<uint32_t>(numVertices) || i1 >= static_cast<uint32_t>(numVertices) || i2 >= static_cast<uint32_t>(numVertices))
			{
				SKY2_ERROR(false, "triangle %d has OOB vertex index (%u,%u,%u) numVertices=%d",
					i, i0, i1, i2, numVertices);
				return; // drop this triangle, keep the rest of the mesh
			}
			idx.push_back(static_cast<uint16_t>(i0));
			idx.push_back(static_cast<uint16_t>(i1));
			idx.push_back(static_cast<uint16_t>(i2));
		};

		if (numMat <= 1)
		{
			std::vector<uint16_t> idx = device->geometry.indices.scratch();
			idx.reserve(static_cast<size_t>(numTriangles) * 3);
			for (int i = 0; i < numTriangles; i++)
				appendTri(idx, i);
			out.subs.push_back({numMat == 1 ? 0 : -1, device->geometry.indices.create(std::move(idx), perFrame)});
			// RW's vertex-alpha term, computed once here rather than rescanned per draw every frame.
			if (out.hasVertexColor)
			{
				const std::span<const Vertex3D> pv = device->geometry.verts.view(out.verts);
				for (auto& sub : out.subs)
					sub.prelitAlpha = anyPrelitAlpha(pv, device->geometry.indices.view(sub.indices));
			}
			return true;
		}
		// Multi-material: one submesh per material, triangles grouped by matIndex.
		for (int m = 0; m < numMat; m++)
		{
			std::vector<uint16_t> idx = device->geometry.indices.scratch();
			idx.reserve(static_cast<size_t>(numTriangles) * 3 / static_cast<size_t>(numMat) + 3);
			for (int i = 0; i < numTriangles; i++)
				if (static_cast<int>(triangles[i].matIndex) == m)
					appendTri(idx, i);
			if (!idx.empty())
				out.subs.push_back({m, device->geometry.indices.create(std::move(idx), perFrame)});
		}
		// RW's vertex-alpha term, computed once here rather than rescanned per draw every frame.
		if (out.hasVertexColor)
		{
			const std::span<const Vertex3D> pv = device->geometry.verts.view(out.verts);
			for (auto& sub : out.subs)
				sub.prelitAlpha = anyPrelitAlpha(pv, device->geometry.indices.view(sub.indices));
		}
		return true;
	}

	void AtomicPipeline::render(uint32_t atomicAddr)
	{
		RenderQueue& queue = device->queue;
		if (!queue.haveCam)
		{
			return;
		}
		Frame& frame = queue.buildFrame();
		frame.objectCalls.atomics++;
		// Anything that leaves without a draw is a silent drop, which is the one thing a manifest full
		// of draws cannot show. P3FES_LOG_NODRAW names them, with how far the parse got.
		struct CountNoDraw
		{
			Frame& f;
			size_t before;
			uint32_t atomic;
			uint32_t geom = 0;
			int subs = -1, vcount = -1, geomFlags = -1;
			uint32_t pipeline = 0, pipeId = 0, resEntry = 0;
			~CountNoDraw()
			{
				if (f.meshCmds.size() != before)
				{
					return;
				}
				f.objectCalls.atomicsNoDraw++;
				recordObjectNoDraw(pipeline);
				static const bool sLog = sky2::env("LOG_NODRAW") != nullptr;
				if (sLog)
				{
					SKY2_TRACE_ONCE(atomic,
						"atomic 0x%X drew nothing: geom=0x%X subs=%d verts=%d flags=0x%X pipe=0x%X id=0x%X res=0x%X",
						atomic, geom, subs, vcount, geomFlags, pipeline, pipeId, resEntry);
					SKY2_TRACE_ONCE(pipeline, "  pipeline 0x%X nodes: %s", pipeline,
						pipelineNodeNames(pipeline).c_str());
				}
			}
		} countNoDraw{frame, frame.meshCmds.size(), atomicAddr};
		// A PTank carries its particles in the plugin's buffers and an empty RpGeometry, so the mesh
		// path below has nothing to read and would drop it silently.
		if (PTankPipeline::isPTank(atomicAddr))
		{
			device->ptank.render(atomicAddr);
			return;
		}
		const auto atomic = guest::ptr<RpAtomic>(atomicAddr);
		const auto geometry = atomic->geometry;
		countNoDraw.geom = geometry.addr;
		countNoDraw.pipeline = atomic->pipeline.addr;
		countNoDraw.resEntry = atomic->repEntry.addr;
		countNoDraw.pipeId = pdsPipeIdOf(atomic->pipeline.addr);
		if (!geometry)
		{
			SKY2_ERROR(false, "atomic 0x%X has no geometry", atomicAddr);
			return;
		}
		// The geometry-static parse (verts + per-material index lists) is cached by geometry address and
		// reused every frame; only rebuilt on a cache miss. The cache is dropped when the geometry is
		// modified/destroyed (ObjectManager::invalidateGeom). With the tracker off (P3FES_NO_RWTRACK) those
		// invalidation hooks are not registered, so fall back to parsing fresh every frame.
		ObjectManager& objects = device->objects;
		ObjectManager::CachedGeom scratch;
		const ObjectManager::CachedGeom* cg = nullptr;
		GeomStats::Parse& cost = device->geometry.stats.atomics;
		// The cache needs the invalidation hooks (tracker) registered; app::geomCache off = parse fresh.
		if (objects.trackerEnabled() && sky2::config().geomCache)
		{
			cg = objects.findGeom(geometry.addr);
			// Native geometry is exempt from the cache: it lives in a plugin ResEntry the game writes
			// directly, so RpGeometryUnlock never fires for it and nothing would ever mark it dirty.
			// Effect billboards animate their vertices there, and a cached parse freezes them at
			// whatever the packet held the first frame the atomic drew.
			if (cg && !cg->dirty && !cg->native)
			{
				bumpStat(cost.hits);
			}
			else
			{
				// Dirty means "may have changed": re-parse, then let refreshGeom keep the cached buffers
				// if the bytes match, which is the common case for a mesh whose animation only moves
				// bones. Keeping them is what spares the GPU re-upload a new GeomId would force.
				ObjectManager::CachedGeom parsed;
				bool ok = false;
				{
					ScopedNs timer{cost.ns};
					ok = parseGeomStatic(geometry.addr, parsed, false);
				}
				if (!ok)
					return;
				device->geometry.noteParse(cost, parsed);
				cg = &objects.refreshGeom(geometry.addr, std::move(parsed));
			}
		}
		else
		{
			// Uncached: the buffers belong to this frame alone, so let the pool reclaim them once the
			// render thread is done with it.
			bool ok = false;
			{
				ScopedNs timer{cost.ns};
				ok = parseGeomStatic(geometry.addr, scratch, true);
			}
			if (!ok)
				return;
			device->geometry.noteParse(cost, scratch);
			cg = &scratch;
		}

		countNoDraw.subs = static_cast<int>(cg->subs.size());
		countNoDraw.vcount = static_cast<int>(cg->vcount);
		countNoDraw.geomFlags = static_cast<int>(cg->geomFlags);

		// Per-frame state on top of the cached geometry: transform, skin pose, render-state latches.
		pg.reset();
		pg.verts = cg->verts;
		pg.vcount = cg->vcount;
		pg.lit = cg->lit;
		pg.hasNormals = cg->hasNormals;
		pg.hasVertexColor = cg->hasVertexColor;
		pg.native = cg->native;
		pg.geomFlags = cg->geomFlags;
		pg.numTexCoordSets = cg->numTexCoordSets;
		pg.texCoordsAddr = cg->texCoordsAddr;
		pg.sourceAddr = atomicAddr;
		pg.source = MeshSource::Atomic;
		pg.matfxEnabled = matfxAtomicEnabled(atomicAddr);

		// A6: we only read morph target 0. If an atomic has multiple targets and a moving interpolator
		// (water, curtains, faces), we render its base pose forever - surface it once so the gap is visible.
		if (geometry->numMorphTargets > 1)
		{
			const RpInterpolator& in = atomic->interpolator;
			if (in.startMorphTarget != in.endMorphTarget)
			{
				SKY2_TRACE_ONCE(atomicAddr, "atomic 0x%X morphs (numMorphTargets=%d, interp %d->%d "
											 "pos=%g/%g): rendering base target 0 only (A6)",
					atomicAddr, static_cast<int>(geometry->numMorphTargets),
					static_cast<int>(in.startMorphTarget), static_cast<int>(in.endMorphTarget),
					in.position, in.time);
			}
		}

		// Bone matrices (pose * inverseBind per bone) are per-frame, appended to the frame's arena.
		// readSkin builds them whenever the HAnim hierarchy resolves; cg->hasSkinData is the separate,
		// per-parse check that this geometry actually carries a per-vertex bone binding to weight by.
		const SkinBinding skin = readSkin(atomicAddr, geometry.addr, queue.buildFrame());
		pg.skinned = skin.numBones > 0 && cg->hasSkinData;
		pg.boneOffset = skin.boneOffset;
		pg.boneCount = static_cast<uint16_t>(skin.numBones);
		pg.objPipeline = atomic->pipeline.addr;
		recordObjectPipeline(pg.objPipeline, false, pg.skinned);

		// Skinned atomics: the HAnim bone matrices already place vertices in world space, so no atomic
		// frame transform on top. Rigid atomics use their frame LTM.
		const uint32_t atomicFrame = atomic->object.object.parent.addr;
		pg.model = pg.skinned ? glm::mat4(1.0f) : toMat4(guest::ptr<RwFrame>(atomicFrame)->ltm);

		queue.readRenderStateInto(pg);
		SKY2_ERROR(queue.renderState[rwRENDERSTATESHADEMODE] != rwSHADEMODEFLAT,
			"SHADEMODE flat requested but only gouraud is implemented");

		// Gather this atomic's lights once (atomics test rpLIGHTLIGHTATOMICS), shared by its submeshes.
		queue.currentLights = queue.gatherLights(rpLIGHTLIGHTATOMICS, atomicAddr, true, pg.hasNormals);

		// Rebuild each submesh from the cached index list + the material's (possibly dynamic) config.
		RpMaterialList& matList = geometry->matList;
		for (const ObjectManager::CachedGeom::Sub& sub : cg->subs)
		{
			SubMesh sm;
			sm.materialBaseColor = DEFAULT_MATERIAL_COLOR;
			sm.indices = sub.indices;
			sm.prelitAlpha = sub.prelitAlpha; // decided at parse; the blend predicate reads it per draw
			// matIndex is stable for a given parse and unique within the object, which is all the
			// cross-frame match needs. +1 keeps 0 meaning "no material".
			sm.materialKey = static_cast<uint32_t>(sub.matIndex + 1);
			if (sub.matIndex >= 0)
			{
				const auto material = matList.materials[sub.matIndex];
				if (material)
				{
					configureMaterial(sm, material,
						{.geomFlags = pg.geomFlags, .matfxAtomicEnabled = pg.matfxEnabled, .applyEffects = true, .fromAtomic = true, .skinned = pg.skinned},
						device->textures);
				}
			}
			queue.enqueueSubMesh(pg, sm);
		}
	}
} // namespace sky2
