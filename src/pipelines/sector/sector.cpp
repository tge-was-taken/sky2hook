// World-sector pipeline: the BSP world geometry (RpWorldSector) the game renders via
// _rpSectorDefaultRenderCallBack. Like an atomic geometry but world-space (identity model) and split
// by the mesh header's per-material RpMesh entries. Parses the sector into the shared ParsedGeometry
// and enqueues its submeshes through the shared geometry back-end (enqueueSubMesh). Stateless per call.
#include "pipelines/sector/sector.h"

#include "device.h"
#include "common.h"
#include "object_manager.h"
#include "render_queue.h"
#include "renderer.h"
#include "rw_to_gl.h"
#include "texture_manager.h"

#include "geometry.h"
#include "material.h"
#include "native_data.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace sky2
{
	void SectorPipeline::logSector(uint32_t sectorAddr)
	{
		const SectorFields f = readSectorFields(sectorAddr);
		const int nv = f.numVertices, nt = f.numTriangles;
		const auto mh = guest::ptr<RpMeshHeader>(f.mesh);
		std::fprintf(stderr, "[sector] 0x%08X nv=%d nt=%d matBase=%d tris=0x%X verts=0x%X uv0=0x%X "
							 "prelit=0x%X mesh=0x%X\n",
			sectorAddr, nv, nt, f.matListWindowBase, f.triangles, f.vertices, f.texCoords0,
			f.preLitLum, f.mesh);
		if (mh)
		{
			const uint32_t first = mh.addr + 16 + mh->firstMeshOffset;
			std::fprintf(stderr, "  meshHdr flags=0x%X numMeshes=%d totalIdx=%u firstOff=%u\n",
				mh->flags, int(mh->numMeshes), mh->totalIndicesInMesh, mh->firstMeshOffset);
			for (int m = 0; m < mh->numMeshes && m < 8; m++)
			{
				const auto mesh = guest::ptr<RpMesh>(first + m * 12);
				const auto mat = mesh->material;
				std::fprintf(stderr, "  mesh[%d] mat=0x%X nIdx=%u idx=0x%X tex=0x%X\n", m, mat.addr,
					mesh->numIndices, uint32_t(mesh->indices.addr),
					mat ? uint32_t(mat->texture.addr) : 0);
			}
		}
		if (const uint32_t native = sectorNativeTable(sectorAddr))
		{
			for (int m = 0; mh && m < mh->numMeshes; m++)
			{
				logNativeMesh(native, m);
			}
		}
		else if (!f.vertices)
		{
			// Pre-instanced with no native table configured: the plugin offset is unknown for this
			// disc, so print the tail words a candidate has to be found among.
			std::fprintf(stderr, "  no native table; plugin words:");
			for (uint32_t off = 0x60; off < 0xC0; off += 4)
			{
				std::fprintf(stderr, " +0x%02X=0x%08X", off, *guest::ptr<uint32_t>(sectorAddr + off));
			}
			std::fprintf(stderr, "\n");
		}
		for (int i = 0; f.triangles && i < nt && i < 4; i++)
		{
			const RpTriangle& t = guest::ptr<RpTriangle>(f.triangles)[i];
			std::fprintf(stderr, "  tri[%d] v=(%u,%u,%u) mat=%u\n", i, t.vertIndex[0], t.vertIndex[1],
				t.vertIndex[2], t.matIndex);
		}
	}

	void SectorPipeline::appendMeshIndices(std::vector<uint16_t>& outIdx, const RpMesh& mesh, bool triStrip, int numVertices)
	{
		const uint32_t n = mesh.numIndices;
		if (!mesh.indices || n < 3 || n > 300000)
		{
			return;
		}
		const auto idx = mesh.indices;
		auto valid = [&](uint32_t a, uint32_t b, uint32_t c) {
			return a < uint32_t(numVertices) && b < uint32_t(numVertices) && c < uint32_t(numVertices) && a != b && b != c && a != c;
		};
		if (!triStrip)
		{
			for (uint32_t i = 0; i + 3 <= n; i += 3)
			{
				const uint32_t a = idx[int(i)], b = idx[int(i + 1)], c = idx[int(i + 2)];
				if (valid(a, b, c))
				{
					outIdx.push_back(uint16_t(a));
					outIdx.push_back(uint16_t(b));
					outIdx.push_back(uint16_t(c));
				}
			}
			return;
		}
		for (uint32_t i = 0; i + 2 < n; i++)
		{
			uint32_t a = idx[int(i)], b = idx[int(i + 1)], c = idx[int(i + 2)];
			if (i & 1)
			{
				std::swap(b, c); // keep winding consistent on odd strip steps
			}
			if (valid(a, b, c))
			{
				outIdx.push_back(uint16_t(a));
				outIdx.push_back(uint16_t(b));
				outIdx.push_back(uint16_t(c));
			}
		}
	}

	bool SectorPipeline::parseNative(uint32_t sectorAddr, uint32_t nativeTable, ObjectManager::CachedSector& out, bool perFrame)
	{
		const SectorFields f = readSectorFields(sectorAddr);
		const auto mh = guest::ptr<RpMeshHeader>(f.mesh);
		const int numMeshes = mh ? mh->numMeshes : 0;
		if (numMeshes == 0)
		{
			return false;
		}
		out.native = true;
		std::vector<Vertex3D> vbuf = device->geometry.verts.scratch();
		std::vector<std::pair<uint32_t, uint32_t>> ranges(static_cast<size_t>(numMeshes)); // base, count
		bool anyColor = false, anyNormal = false, anyUV = false;
		for (int m = 0; m < numMeshes; m++)
		{
			NativeMesh nm;
			const uint32_t base = static_cast<uint32_t>(vbuf.size());
			if (!decodeNativeMesh(nativeTable, m, nm) || base + nm.pos.size() > 0xFFFF)
			{
				SKY2_TRACE_ONCE(sectorAddr, "sector 0x%X: native mesh %d of %d did not decode",
					sectorAddr, m, numMeshes);
				ranges[m] = {base, 0}; // undecodable, or the sector overflows 16-bit indices
				continue;
			}
			anyColor = anyColor || !nm.color.empty();
			anyNormal = anyNormal || !nm.normal.empty();
			anyUV = anyUV || !nm.uv.empty();
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
				vbuf.push_back(vtx);
			}
			ranges[m] = {base, static_cast<uint32_t>(nm.pos.size())};
		}
		if (vbuf.empty())
		{
			return false;
		}

		out.hasNormals = anyNormal;
		out.hasVertexColor = anyColor;
		out.numTexCoordSets = anyUV ? 1 : 0;
		out.vcount = static_cast<int>(vbuf.size());
		out.verts = device->geometry.verts.create(std::move(vbuf), perFrame);

		const uint32_t firstMesh = mh.addr + sizeof(RpMeshHeader) + mh->firstMeshOffset;
		for (int m = 0; m < numMeshes; m++)
		{
			if (ranges[m].second < 3)
			{
				continue;
			}
			std::vector<uint16_t> idx = device->geometry.indices.scratch();
			appendNativePrims(idx, device->geometry.verts.view(out.verts), ranges[m].first,
				ranges[m].second, nativePrimFor(mh->flags));
			if (!idx.empty())
			{
				out.subs.push_back({guest::ptr<RpMesh>(firstMesh + m * static_cast<int>(sizeof(RpMesh)))->material.addr,
					device->geometry.indices.create(std::move(idx), perFrame)});
			}
		}
		if (out.subs.empty()) // every strip was degenerate: give the vertex slot back before failing
		{
			device->objects.releaseSector(out);
			return false;
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

	ObjectManager::SectorSignature SectorPipeline::signatureFor(uint32_t sectorAddr, const SectorFields& f)
	{
		ObjectManager::SectorSignature sig;
		// A pre-instanced sector has no `vertices` array: its geometry hangs off the native table, so
		// without this the signature would not see that half of the world change at all.
		sig.nativeTable = f.vertices ? 0 : sectorNativeTable(sectorAddr);
		sig.triangles = f.triangles;
		sig.vertices = f.vertices;
		sig.normals = f.normals;
		sig.texCoords0 = f.texCoords0;
		sig.preLitLum = f.preLitLum;
		sig.mesh = f.mesh;
		sig.numVertices = f.numVertices;
		sig.numTriangles = f.numTriangles;
		if (const auto mh = guest::ptr<RpMeshHeader>(f.mesh))
		{
			sig.meshFlags = mh->flags;
			sig.numMeshes = mh->numMeshes;
			sig.totalIndices = mh->totalIndicesInMesh;
		}
		return sig;
	}

	bool SectorPipeline::parseGeometry(uint32_t sectorAddr, const SectorFields& f,
		ObjectManager::CachedSector& out, bool perFrame)
	{
		const int numVertices = f.numVertices;
		const auto mh = guest::ptr<RpMeshHeader>(f.mesh);
		if (!f.vertices)
		{
			// No platform-independent arrays: the geometry is pre-instanced, if this build ships it.
			const uint32_t nativeTable = sectorNativeTable(sectorAddr);
			return nativeTable && parseNative(sectorAddr, nativeTable, out, perFrame);
		}
		if (numVertices <= 0 || numVertices > 200000 || !mh || mh->numMeshes == 0)
		{
			return false; // empty/degenerate sector (e.g. a container node) - nothing to draw
		}
		const auto verts = guest::ptr<RwV3d>(f.vertices);
		const auto texCoords = guest::ptr<RwTexCoords>(f.texCoords0);
		const bool hasUV = f.texCoords0 != 0;
		const auto preLit = guest::ptr<RwRGBA>(f.preLitLum);
		const auto normals = guest::ptr<RpVertexNormal>(f.normals);
		out.hasNormals = static_cast<bool>(normals);
		out.hasVertexColor = static_cast<bool>(preLit);
		out.numTexCoordSets = hasUV ? 1 : 0;
		out.vcount = numVertices;
		// assign, not resize: see the same spot in the atomic pipeline.
		std::vector<Vertex3D> vbuf = device->geometry.verts.scratch();
		vbuf.assign(static_cast<size_t>(numVertices), Vertex3D{});
		for (int i = 0; i < numVertices; i++)
		{
			Vertex3D& vtx = vbuf[i];
			const RwV3d& p = verts[i];
			vtx.pos = glm::vec3(p.x, p.y, p.z);
			if (out.hasNormals)
			{
				const RpVertexNormal& n = normals[i];
				vtx.normal = glm::vec3(n.x, n.y, n.z) * (1.0f / 127.0f);
			}
			if (hasUV)
			{
				const RwTexCoords& uv = texCoords[i];
				vtx.uv = glm::vec2(uv.u, uv.v);
			}
			if (out.hasVertexColor)
			{
				vtx.color = toVec4(preLit[i]);
			}
		}
		out.verts = device->geometry.verts.create(std::move(vbuf), perFrame);

		const bool triStrip = (mh->flags & 0x1) != 0; // rpMESHHEADERTRISTRIP
		const uint32_t firstMesh = mh.addr + sizeof(RpMeshHeader) + mh->firstMeshOffset;
		const int numMeshes = mh->numMeshes;
		for (int m = 0; m < numMeshes; m++)
		{
			const auto mesh = guest::ptr<RpMesh>(firstMesh + m * static_cast<int>(sizeof(RpMesh)));
			std::vector<uint16_t> idx = device->geometry.indices.scratch();
			appendMeshIndices(idx, *mesh, triStrip, numVertices);
			if (!idx.empty())
			{
				out.subs.push_back({mesh->material.addr,
					device->geometry.indices.create(std::move(idx), perFrame)});
			}
		}
		if (out.subs.empty()) // every mesh was degenerate: give the vertex slot back before failing
		{
			device->objects.releaseSector(out);
			return false;
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

	// Per-frame work on top of the cached geometry: the world's lit flag, the render-state latches and
	// the materials. Materials are re-read every frame because a texture can change under a sector
	// whose geometry did not, which is the same split the atomic pipeline uses.
	bool SectorPipeline::parse(uint32_t sectorAddr, ParsedGeometry& out)
	{
		const SectorFields f = readSectorFields(sectorAddr);
		ObjectManager& objects = device->objects;
		const uint32_t worldAddr = device->queue.curWorld;
		const bool useCache = sky2::config().sectorCache;

		ObjectManager::CachedSector scratch;
		const ObjectManager::CachedSector* cs = nullptr;
		GeomStats::Parse& cost = device->geometry.stats.sectors;
		if (useCache)
		{
			cs = objects.findSector(sectorAddr);
			// Revalidate on every call. This is the only defence that works on the serials whose
			// RpWorldDestroy/RpWorldUnlock addresses we do not know.
			if (cs && (!(cs->sig == signatureFor(sectorAddr, f)) || cs->world != worldAddr))
			{
				objects.invalidateSector(sectorAddr);
				cs = nullptr;
			}
			if (cs)
			{
				bumpStat(cost.hits);
			}
			else
			{
				ObjectManager::CachedSector built;
				bool ok = false;
				{
					ScopedNs timer{cost.ns};
					ok = parseGeometry(sectorAddr, f, built, false);
				}
				if (!ok)
				{
					return false;
				}
				device->geometry.noteParse(cost, built);
				built.sig = signatureFor(sectorAddr, f);
				built.world = worldAddr;
				built.pipeline = f.pipeline;
				cs = &objects.putSector(sectorAddr, std::move(built));
			}
		}
		else
		{
			bool ok = false;
			{
				ScopedNs timer{cost.ns};
				ok = parseGeometry(sectorAddr, f, scratch, true);
			}
			if (!ok)
			{
				return false;
			}
			device->geometry.noteParse(cost, scratch);
			scratch.pipeline = f.pipeline;
			cs = &scratch;
		}
		// A cache hit still feeds this, or the pipeline diagnostic silently stops seeing sectors.
		pg.objPipeline = cs->pipeline;
		recordObjectPipeline(pg.objPipeline, true, false);

		// The sector's shading regime is the world's flag word, not a geometry's: rpWORLDLIGHT and
		// rpWORLDMODULATEMATERIALCOLOR both live here. Prelit (rpWORLDPRELIT) carries most of the
		// dungeon's shading.
		const uint32_t worldFlags = worldAddr ? guest::ptr<RpWorld>(worldAddr)->flags : 0;
		out.lit = (worldFlags & rpWORLDLIGHT) != 0;
		out.verts = cs->verts;
		out.vcount = cs->vcount;
		out.hasNormals = cs->hasNormals;
		out.hasVertexColor = cs->hasVertexColor;
		out.native = cs->native;
		out.numTexCoordSets = cs->numTexCoordSets;
		out.texCoordsAddr = f.texCoords0;
		out.model = glm::mat4(1.0f); // vertices already in world space
		out.sourceAddr = sectorAddr;
		out.source = MeshSource::Sector;
		device->queue.readRenderStateInto(out);

		for (const ObjectManager::CachedSector::Sub& sub : cs->subs)
		{
			SubMesh sm;
			sm.materialBaseColor = DEFAULT_MATERIAL_COLOR;
			if (sub.materialAddr)
			{
				// applyEffects=false: MatFX is atomic-scope. The modulate bit is the world's, not a
				// geometry's, and dropping it left the floor lit at full material colour.
				configureMaterial(sm, guest::ptr<RpMaterial>(sub.materialAddr), {worldFlags},
					device->textures);
			}
			sm.indices = sub.indices;
			sm.prelitAlpha = sub.prelitAlpha; // decided at parse; the blend predicate reads it per draw
			sm.materialKey = sub.materialAddr; // the guest material, stable across frames
			out.subs.push_back(std::move(sm));
		}
		return !out.subs.empty();
	}

	void SectorPipeline::render(uint32_t sectorAddr)
	{
		RenderQueue& queue = device->queue;
		if (!queue.haveCam || !sectorAddr)
		{
			return;
		}
		Frame& frame = queue.buildFrame();
		frame.objectCalls.sectors++;
		struct CountNoDraw
		{
			Frame& f;
			size_t before;
			~CountNoDraw() { f.objectCalls.sectorsNoDraw += f.meshCmds.size() == before ? 1 : 0; }
		} countNoDraw{frame, frame.meshCmds.size()};
		static int sLogged = 0;
		static const bool sLogSector = sky2::env("LOG_SECTOR") != nullptr;
		if (sLogSector && sLogged++ < 40)
		{
			logSector(sectorAddr);
		}
		pg.reset();
		if (!parse(sectorAddr, pg))
		{
			return;
		}
		// Sectors test rpLIGHTLIGHTWORLD (distinct from the atomic class), gathered once per sector.
		queue.currentLights = queue.gatherLights(rpLIGHTLIGHTWORLD, sectorAddr, false, pg.hasNormals);
		for (const SubMesh& sm : pg.subs)
		{
			queue.enqueueSubMesh(pg, sm);
		}
	}
} // namespace sky2
