#include "object_manager.h"
#include "sky2/host.h"
#include "sky2/host.h"

#include "common.h"
#include "render_queue.h"
#include "renderer.h"
#include "rw_to_gl.h"
#include "texture_manager.h" // enqueueDestroy: free the destroyed raster's GL on the render thread

#include "sky2/addresses.h" // the frame-buffer raster globals (P3FES_DUMP_RASTER)

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sky2
{

	static const char* rasterTypeName(uint8_t type)
	{
		switch (type)
		{
			case rwRASTERTYPENORMAL: return "normal";
			case rwRASTERTYPEZBUFFER: return "zbuffer";
			case rwRASTERTYPECAMERA: return "camera"; // render target (frame buffer)
			case rwRASTERTYPETEXTURE: return "texture";
			case rwRASTERTYPECAMERATEXTURE: return "cameratexture"; // render-to-texture (shadow/FMV)
			default: return "?";
		}
	}

	// Record each raster the first time sky2 sees it, building the picture of render targets.
	void ObjectManager::registerRaster(uint32_t rasterAddr)
	{
		if (!rasterAddr)
			return;
		const auto r = guest::ptr<RwRaster>(rasterAddr);
		RasterInfo info;
		info.width = r->width;
		info.height = r->height;
		info.depth = r->depth;
		info.type = static_cast<uint8_t>(r->cType) & rwRASTERTYPEMASK;
		info.cFormat = static_cast<uint8_t>(r->cFormat);
		const auto [it, fresh] = rasterRegistry.emplace(rasterAddr, info);
		// A game frees and recreates rasters, so the same address comes back with different content;
		// say so rather than logging it as a new raster or, worse, keeping the stale size.
		const bool changed = !fresh && !(it->second == info);
		if (changed)
		{
			it->second = info;
		}
		if (fresh || changed)
		{
			// RwRaster::cFormat is RwRasterFormat >> 8, whose low nibble is the pixel format and tops
			// out at 555. A value past that is not a raster we are looking at, and the "?" type on the
			// same line is the other half of the same tell.
			const bool plausible = rasterTypeName(info.type)[0] != '?' && (info.cFormat & 0x0F) <= 0x0A;
			std::fprintf(stderr, "[sky2] raster 0x%08X %dx%d depth=%d type=%s cFormat=0x%02X%s%s\n",
				rasterAddr, info.width, info.height, info.depth, rasterTypeName(info.type),
				info.cFormat, changed ? " (recreated)" : "",
				plausible ? "" : " [not a raster: type/format out of range]");
		}
	}

	// rwSTANDARD RASTERCREATE (rarely called in P3FES: textures come via NativeTextureRead,
	// framebuffers via a direct path; kept for completeness / when sky2 is the live driver).
	void ObjectManager::rasterCreate(uint32_t rasterAddr) { registerRaster(rasterAddr); }

	// rwSTANDARD RASTERUNLOCK: pixels of this raster just changed; invalidate our cached decode.
	void ObjectManager::rasterUnlock(uint32_t rasterAddr)
	{
		cpuWrittenRasters.insert(rasterAddr); // the EE wrote these pixels, so decoding them is right
		textures->invalidate(rasterAddr);
	}

	// SkyRasterDestroy: the raster is about to be freed (still valid at hook entry, so cpPixels reads
	// fine). Drop every EE-thread cache keyed by it and queue the GL resources for the render thread.
	void ObjectManager::rasterDestroy(uint32_t rasterAddr)
	{
		if (!rasterAddr)
		{
			return;
		}
		const uint32_t pxAddr = guest::ptr<RwRaster>(rasterAddr)->cpPixels.addr;
		textures->invalidate(rasterAddr); // drop the decode signature
		// Every registry keyed by this address: guest rasters are freelist allocated, so the address
		// returns for an unrelated raster and a surviving entry misclassifies it.
		rasterRegistry.erase(rasterAddr);
		renderTargetRasters.erase(rasterAddr);
		cpuWrittenRasters.erase(rasterAddr);
		displayRasters.erase(rasterAddr);
		frameBufferRasters.erase(rasterAddr);
		cameraTargetsSeen.erase(rasterAddr);
		textures->enqueueDestroy(rasterAddr, pxAddr); // render thread frees the GL texture/FBO
	}

	void ObjectManager::registerFrameBufferRaster(uint32_t rasterAddr, BufferRole role, const char* source)
	{
		if (!rasterAddr)
			return;
		const auto r = guest::ptr<RwRaster>(rasterAddr);
		frameBufferW = int32_t(r->width);
		frameBufferH = int32_t(r->height);
		const auto [it, added] = frameBufferRasters.emplace(rasterAddr, role);
		if (!added && it->second == BufferRole::Unknown) // a later source named the role
		{
			it->second = role;
		}
		if (added)
			SKY2_TRACE_ONCE(rasterAddr, "frame-buffer raster 0x%X registered %dx%d via %s (feedback)",
				rasterAddr, frameBufferW, frameBufferH, source);
	}

	ObjectManager::BufferRole ObjectManager::frameBufferRole(uint32_t rasterAddr) const
	{
		const auto it = frameBufferRasters.find(rasterAddr);
		return it == frameBufferRasters.end() ? BufferRole::Unknown : it->second;
	}

	// P3FES_DUMP_RASTER=<hex>[,<hex>...]: dump those RwRasters (fields plus the plugin extension bytes
	// that follow the base struct, where the Sky driver keeps its GS registers) once, at frame
	// P3FES_DUMP_RASTER_FRAME (default 900). For identifying GS-resident buffers by what they alias.
	void ObjectManager::dumpWatchedRasters()
	{
		static const char* spec = sky2::env("DUMP_RASTER");
		if (!spec)
			return;
		static const int atFrame =
			sky2::env("DUMP_RASTER_FRAME") ? std::atoi(sky2::env("DUMP_RASTER_FRAME")) : 900;
		if (++dumpRasterFrames < atFrame || dumpRasterFrames > atFrame + 3)
			return;
		// The flip pair, once per frame: each raster plus the GS address (TEX0 TBP0) it currently points
		// at, which is what a flip rewrites.
		auto bufferTbp = [](uint32_t global) -> uint32_t {
			const uint32_t raster = global ? *guest::ptr<uint32_t>(global) : 0;
			// The Sky raster extension follows the base struct; its first TEX0 word holds TBP0 in bits 0-13.
			return raster ? (*guest::ptr<uint32_t>(raster + sizeof(RwRaster) + 8) & 0x3FFF) : 0;
		};
		std::fprintf(stderr, "[sky2][rasterdump] frame %d: display=0x%X (tbp %u) draw=0x%X (tbp %u)\n",
			dumpRasterFrames,
			sky2::addresses().skyDisplayRaster ? *guest::ptr<uint32_t>(sky2::addresses().skyDisplayRaster) : 0,
			bufferTbp(sky2::addresses().skyDisplayRaster),
			sky2::addresses().skyDrawRaster ? *guest::ptr<uint32_t>(sky2::addresses().skyDrawRaster) : 0,
			bufferTbp(sky2::addresses().skyDrawRaster));
		for (const char* p = spec; *p;)
		{
			const uint32_t addr = uint32_t(std::strtoul(p, const_cast<char**>(&p), 16));
			while (*p == ',' || *p == ' ')
				p++;
			if (!addr)
				continue;
			// Every dumped frame: the GS address this raster currently points at, so a flip is visible.
			std::fprintf(stderr, "[sky2][rasterdump]   0x%08X tbp %u\n", addr,
				*guest::ptr<uint32_t>(addr + sizeof(RwRaster) + 8) & 0x3FFF);
			if (dumpRasterFrames != atFrame)
				continue;
			const auto r = guest::ptr<RwRaster>(addr);
			std::fprintf(stderr,
				"[sky2][rasterdump] 0x%08X parent=0x%X px=0x%X pal=0x%X %dx%d depth=%d stride=%d "
				"off=(%d,%d) cType=0x%02X cFlags=0x%02X privFlags=0x%02X cFormat=0x%02X orig=0x%X %dx%d/%d\n",
				addr, uint32_t(r->parent.addr), uint32_t(r->cpPixels.addr), uint32_t(r->palette.addr),
				int32_t(r->width), int32_t(r->height), int32_t(r->depth), int32_t(r->stride),
				int16_t(r->nOffsetX), int16_t(r->nOffsetY), uint8_t(r->cType), uint8_t(r->cFlags),
				uint8_t(r->privateFlags), uint8_t(r->cFormat), uint32_t(r->originalPixels.addr),
				int32_t(r->originalWidth), int32_t(r->originalHeight), int32_t(r->originalStride));
			const auto ext = guest::ptr<uint32_t>(addr + sizeof(RwRaster));
			for (int i = 0; i < 32; i += 8)
			{
				std::fprintf(stderr, "[sky2][rasterdump]   +0x%02X:", unsigned(sizeof(RwRaster)) + i * 4);
				for (int k = 0; k < 8; k++)
					std::fprintf(stderr, " %08X", ext[i + k]);
				std::fprintf(stderr, "\n");
			}
		}
	}

	// A draw referenced a render-target raster whose pixels live in GS VRAM (cpPixels=0), so sky2 has
	// no EE pixels to decode. Trace the origin once per raster by cross-referencing what sky2 saw:
	// whether the game rendered into it (or its parent) through a camera pass we tracked, and how we
	// classified that pass. This turns an opaque "cannot sample" into an actionable lead.
	void ObjectManager::traceGsResidentRaster(uint32_t rasterAddr, int depth, uint8_t cType, uint8_t cFmt, int tw, int th)
	{
		rtSampleWanted.store(true); // let the host auto-capture this frame (P3FES_RT_AUTOCAP)
		const uint32_t parent = guest::ptr<RwRaster>(rasterAddr)->parent.addr;
		const auto self = cameraTargetsSeen.find(rasterAddr);
		const auto par = parent ? cameraTargetsSeen.find(parent) : cameraTargetsSeen.end();
		const bool selfSeen = self != cameraTargetsSeen.end();
		const bool parSeen = par != cameraTargetsSeen.end();
		const char* hint =
			(isRenderTargetRaster(rasterAddr) || (parent && isRenderTargetRaster(parent))) ? "registered off-screen RT but its FBO is missing (should have sampled the FBO)" : (selfSeen || parSeen) ? "seen as a camera target but not registered off-screen (pass classification missed it)" :
																																																	   "never a camera target: rendered via a non-camera GS path (blit/IPU/effect we do not intercept)";
		SKY2_TRACE_ONCE(rasterAddr,
			"GS-resident RT unsampleable: raster=0x%X parent=0x%X %dx%d depth=%d cType=0x%02X(%s) "
			"cFormat=0x%02X | cameraTargetSeen=%s parentSeen=%s -> %s",
			rasterAddr, parent, tw, th, depth, cType, rasterTypeName(cType & rwRASTERTYPEMASK), cFmt,
			selfSeen ? self->second.kind : "never", parSeen ? par->second.kind : "never", hint);
	}

	void ObjectManager::setMovieFrame(uint32_t bufAddr)
	{
		movieBuf = bufAddr; // the just-decoded macroblock-tiled frame (detiled at draw time)
		movieFrameId++;
	}

	// ---- Sector cache (see object_manager.h) ----
	void ObjectManager::releaseSector(const CachedSector& s)
	{
		geometry->verts.free(s.verts);
		for (const CachedSector::Sub& sub : s.subs)
			geometry->indices.free(sub.indices);
	}

	const ObjectManager::CachedSector* ObjectManager::findSector(uint32_t sectorAddr) const
	{
		const auto it = sectorCache.find(sectorAddr);
		return it == sectorCache.end() ? nullptr : &it->second;
	}

	ObjectManager::CachedSector& ObjectManager::putSector(uint32_t sectorAddr, CachedSector&& s)
	{
		if (const auto it = sectorCache.find(sectorAddr); it != sectorCache.end())
			releaseSector(it->second);
		return sectorCache.insert_or_assign(sectorAddr, std::move(s)).first->second;
	}

	void ObjectManager::invalidateSector(uint32_t sectorAddr)
	{
		if (const auto it = sectorCache.find(sectorAddr); it != sectorCache.end())
		{
			releaseSector(it->second);
			sectorCache.erase(it);
		}
	}

	// worldAddr 0 drops everything, which is what a world we never recorded should do.
	void ObjectManager::invalidateWorldSectors(uint32_t worldAddr)
	{
		size_t dropped = 0;
		for (auto it = sectorCache.begin(); it != sectorCache.end();)
		{
			if (!worldAddr || it->second.world == worldAddr)
			{
				releaseSector(it->second);
				it = sectorCache.erase(it);
				dropped++;
			}
			else
			{
				++it;
			}
		}
		// Observable on purpose: an area load is the case the signature check alone cannot cover, and
		// this is the only way to tell the hook fired without playing through one.
		if (dropped)
			std::fprintf(stderr, "[sky2][sector] world 0x%X destroyed, dropped %zu cached sectors\n",
				worldAddr, dropped);
	}

	// ---- Parsed-geometry cache (see object_manager.h) ----
	const ObjectManager::CachedGeom* ObjectManager::findGeom(uint32_t geomAddr) const
	{
		const auto it = geomCache.find(geomAddr);
		return it == geomCache.end() ? nullptr : &it->second;
	}

	// Cached geometry is not per-frame, so the pool holds its slots until told otherwise. Dropping an
	// entry without this leaks them for the run.
	void ObjectManager::releaseGeom(const CachedGeom& g)
	{
		geometry->verts.free(g.verts);
		for (const CachedGeom::Sub& s : g.subs)
			geometry->indices.free(s.indices);
	}

	ObjectManager::CachedGeom& ObjectManager::putGeom(uint32_t geomAddr, CachedGeom&& g)
	{
		if (const auto it = geomCache.find(geomAddr); it != geomCache.end())
			releaseGeom(it->second);
		return geomCache.insert_or_assign(geomAddr, std::move(g)).first->second;
	}

	void ObjectManager::invalidateGeom(uint32_t geomAddr)
	{
		if (const auto it = geomCache.find(geomAddr); it != geomCache.end())
		{
			releaseGeom(it->second);
			geomCache.erase(it);
		}
	}

	void ObjectManager::markGeomDirty(uint32_t geomAddr)
	{
		if (const auto it = geomCache.find(geomAddr); it != geomCache.end())
		{
			it->second.dirty = true;
		}
	}

	// Vertex3D and the index buffers are trivially copyable and packed (Vertex3D is 22 floats, no
	// padding), so the bytes are the content.
	bool ObjectManager::sameGeom(const CachedGeom& a, const CachedGeom& b) const
	{
		if (a.vcount != b.vcount || a.lit != b.lit || a.hasNormals != b.hasNormals
			|| a.hasVertexColor != b.hasVertexColor || a.hasSkinData != b.hasSkinData
			|| a.geomFlags != b.geomFlags || a.numTexCoordSets != b.numTexCoordSets
			|| a.texCoordsAddr != b.texCoordsAddr || a.subs.size() != b.subs.size())
		{
			return false;
		}
		const std::span<const Vertex3D> va = geometry->verts.view(a.verts);
		const std::span<const Vertex3D> vb = geometry->verts.view(b.verts);
		if (va.size() != vb.size() || std::memcmp(va.data(), vb.data(), va.size_bytes()) != 0)
		{
			return false;
		}
		for (size_t i = 0; i < a.subs.size(); i++)
		{
			if (a.subs[i].matIndex != b.subs[i].matIndex)
			{
				return false;
			}
			const std::span<const uint16_t> ia = geometry->indices.view(a.subs[i].indices);
			const std::span<const uint16_t> ib = geometry->indices.view(b.subs[i].indices);
			if (ia.size() != ib.size() || std::memcmp(ia.data(), ib.data(), ia.size_bytes()) != 0)
			{
				return false;
			}
		}
		return true;
	}

	ObjectManager::CachedGeom& ObjectManager::refreshGeom(uint32_t geomAddr, CachedGeom&& reparsed)
	{
		const auto it = geomCache.find(geomAddr);
		if (it != geomCache.end() && sameGeom(it->second, reparsed))
		{
			// The edit did not touch anything we read. Drop the duplicate parse and keep the entry, so
			// its GeomIds stay valid and the GPU copy is not re-uploaded.
			releaseGeom(reparsed);
			it->second.dirty = false;
			bumpStat(geometry->stats.atomics.unchanged);
			return it->second;
		}
		return putGeom(geomAddr, std::move(reparsed));
	}

	// ---- RW object lifecycle tracker (folded from rw_tracker) ------------------------------------
	static const char* const RW_TYPE_NAME[ObjectManager::RwTypeCount] = {
		"Frame", "Atomic", "Geometry", "Texture", "Raster", "Material", "Clump", "Light", "Camera"};

	constexpr int MAX_CHAIN_FRAMES = 16; // deepest chain we keep
	constexpr uint32_t STACK_SCAN_BYTES = 1024; // stack window scanned above $sp for return addresses
	constexpr uint32_t EE_RAM = 0x02000000; // 32 MB EE main RAM

	static uint32_t readWord(uint32_t addr)
	{
		return addr + 4 <= EE_RAM ? *guest::ptr<uint32_t>(addr) : 0;
	}

	// Is `pc` a real return address? A jal/jalr stores (call_pc + 8), so the word two instructions
	// back must be the call. This rejects the stray data/pointers that litter a raw stack scan.
	static bool isReturnAddr(uint32_t pc)
	{
		if (pc & 3 || pc < 8 || !sky2::host().inCodeRange(pc))
			return false;
		const uint32_t op = readWord(pc - 8);
		const bool jal = (op >> 26) == 3; // JAL
		const bool jalr = (op >> 26) == 0 && (op & 0x3F) == 9; // SPECIAL / JALR
		return jal || jalr;
	}

	// Conservative backtrace: the create site followed by return addresses found on the caller's stack.
	// MIPS has no frame pointer, so scan a window above $sp and keep the words that pass isReturnAddr().
	static std::vector<uint32_t> captureChain(uint32_t createRa, uint32_t sp)
	{
		std::vector<uint32_t> chain;
		chain.push_back(createRa);
		if (sp == 0)
			return chain;
		uint32_t last = createRa;
		for (uint32_t off = 0; off < STACK_SCAN_BYTES && int(chain.size()) < MAX_CHAIN_FRAMES; off += 4)
		{
			const uint32_t w = readWord(sp + off);
			if (w != last && isReturnAddr(w))
			{
				chain.push_back(w);
				last = w;
				if (sky2::host().resolveSymbol(w).rfind("_start", 0) == 0)
					break; // reached the entry point; anything above is stale stack
			}
		}
		return chain;
	}

	// P3FES_TRACE_DRAWBUF: print the EE call chain reaching an access to a frame buffer, once per
	// distinct stack, with a running count reported at exit. `id` is the raster involved.
	void ObjectManager::traceEeChain(const char* tag, uint32_t id, uint32_t ra, uint32_t sp)
	{
		static const bool sOn = sky2::env("TRACE_DRAWBUF") != nullptr;
		if (!sOn)
		{
			return;
		}
		// Key on the caller, the one frame that comes from the call itself. The frames above it are a
		// scan of the stack window (MIPS has no frame pointer), so they include stale words and would
		// otherwise split one call site into several "distinct" stacks.
		static std::unordered_set<uint64_t> seen;
		if (!seen.insert((static_cast<uint64_t>(ra) << 32) ^ id).second)
		{
			return;
		}
		const std::vector<uint32_t> chain = captureChain(ra, sp);
		std::fprintf(stderr, "[drawbuf] %s raster=0x%08X from %s\n", tag, id,
			sky2::host().resolveSymbol(ra).c_str());
		for (size_t i = 1; i < chain.size(); i++) // above the caller: best-effort, may hold stale frames
			std::fprintf(stderr, "    ^ %s\n", sky2::host().resolveSymbol(chain[i]).c_str());
	}

	static uint64_t hashChain(uint8_t type, const std::vector<uint32_t>& chain)
	{
		uint64_t h = 1469598103934665603ull ^ type;
		for (uint32_t a : chain)
			h = (h ^ a) * 1099511628211ull;
		return h;
	}

	static void logProvenance(const char* why, uint8_t t, uint32_t obj, const std::vector<uint32_t>& chain)
	{
		std::fprintf(stderr, "[rwtrack] %s %s 0x%08X created via:\n", why, RW_TYPE_NAME[t], obj);
		for (size_t i = 0; i < chain.size(); i++)
			std::fprintf(stderr, "    #%zu %s\n", i, sky2::host().resolveSymbol(chain[i]).c_str());
	}

	static bool watched(const std::unordered_set<uint32_t>& watch, uint32_t obj, const std::vector<uint32_t>& chain)
	{
		if (watch.empty())
			return false;
		if (watch.count(obj))
			return true;
		for (uint32_t a : chain)
			if (watch.count(a))
				return true;
		return false;
	}

	void ObjectManager::trackerInit()
	{
		trackEnabled = sky2::env("NO_RWTRACK") == nullptr; // on by default; opt out for parallel/CI
		if (!trackEnabled)
			return;
		trackStackMode = sky2::env("RWTRACK_STACK") != nullptr;
		const char* s = sky2::env("RWTRACK_WATCH");
		while (s && *s)
		{
			char* end = nullptr;
			const uint32_t a = static_cast<uint32_t>(std::strtoul(s, &end, 0));
			if (end == s)
				break;
			if (a)
				trackWatch.insert(a);
			s = (*end == ',') ? end + 1 : end;
		}
		sky2::host().loadSymbols();
	}

	// EE-thread only (runs inside Dispatch), so no locking.
	void ObjectManager::trackCreate(RwType t, uint32_t obj, uint32_t createRa, uint32_t sp)
	{
		if (obj == 0)
			return;
		Stat& st = trackStat[t];
		st.created++;
		st.live++;
		if (st.live > st.peakLive)
			st.peakLive = st.live;
		trackSites[(static_cast<uint64_t>(createRa) << 4) | t]++;
		if (t == Atomic)
		{
			// RpAtomicCreate allocates from an RW freelist, which is LIFO, so a destroy followed by a
			// create hands back the same address. Bumping per create is what lets a consumer tell the
			// new tenant from the old one; the entry deliberately outlives the free, since resetting it
			// would let a new object collide with a stamp taken before the free.
			++generations[obj];
		}

		uint64_t chainKey = 0;
		const bool needChain = trackStackMode || !trackWatch.empty();
		if (needChain)
		{
			std::vector<uint32_t> chain = captureChain(createRa, sp);
			chainKey = hashChain(static_cast<uint8_t>(t), chain);
			if (trackStackMode)
			{
				ChainStat& cs = trackChains[chainKey];
				if (cs.count == 0)
				{
					cs.type = static_cast<uint8_t>(t);
					cs.chain = chain;
				}
				cs.count++;
			}
			if (watched(trackWatch, obj, chain))
				logProvenance("WATCH", static_cast<uint8_t>(t), obj, chain);
		}
		trackLive[obj] = {static_cast<uint8_t>(t), createRa, chainKey};
	}

	void ObjectManager::trackDestroy(RwType t, uint32_t obj)
	{
		trackStat[t].destroyed++;
		auto it = trackLive.find(obj);
		if (it != trackLive.end())
		{
			if (trackStat[t].live > 0)
				trackStat[t].live--;
			trackLive.erase(it);
		}
	}

	void ObjectManager::describe(uint32_t addr, std::FILE* out) const
	{
		if (!trackEnabled)
			return;
		const auto it = trackLive.find(addr);
		if (it == trackLive.end())
		{
			std::fprintf(out, "[rwtrack] 0x%08X: not a live tracked object\n", addr);
			return;
		}
		const Obj& o = it->second;
		std::fprintf(out, "[rwtrack] 0x%08X: %s created at %s\n", addr, RW_TYPE_NAME[o.type],
			sky2::host().resolveSymbol(o.createRa).c_str());
		if (o.chainKey)
		{
			const auto cit = trackChains.find(o.chainKey);
			if (cit != trackChains.end())
				logProvenance("chain for", o.type, addr, cit->second.chain);
		}
	}

	void ObjectManager::report(const char* reason, std::FILE* out) const
	{
		if (!trackEnabled)
			return;
		std::fprintf(out, "[rwtrack] RW objects (%s):\n", reason ? reason : "");
		for (int t = 0; t < RwTypeCount; t++)
		{
			const Stat& st = trackStat[t];
			if (st.created == 0 && st.destroyed == 0)
				continue;
			std::fprintf(out, "  %-9s created=%-6llu destroyed=%-6llu live=%-5u peak=%u\n",
				RW_TYPE_NAME[t], static_cast<unsigned long long>(st.created),
				static_cast<unsigned long long>(st.destroyed), st.live, st.peakLive);
		}

		std::vector<std::pair<uint64_t, uint64_t>> sites(trackSites.begin(), trackSites.end());
		std::sort(sites.begin(), sites.end(),
			[](const auto& a, const auto& b) { return a.second > b.second; });
		std::fprintf(out, "  top create sites:\n");
		for (size_t i = 0; i < sites.size() && i < 12; i++)
		{
			const int t = static_cast<int>(sites[i].first & 0xF);
			const uint32_t ra = static_cast<uint32_t>(sites[i].first >> 4);
			std::fprintf(out, "    %-9s x%-6llu <- %s\n", RW_TYPE_NAME[t],
				static_cast<unsigned long long>(sites[i].second), sky2::host().resolveSymbol(ra).c_str());
		}

		// Granular attribution: the full loader chains, most-used first. Only populated in stack mode.
		if (trackStackMode && !trackChains.empty())
		{
			std::vector<const ChainStat*> chains;
			chains.reserve(trackChains.size());
			for (const auto& kv : trackChains)
				chains.push_back(&kv.second);
			std::sort(chains.begin(), chains.end(),
				[](const ChainStat* a, const ChainStat* b) { return a->count > b->count; });
			std::fprintf(out, "  top create chains:\n");
			for (size_t i = 0; i < chains.size() && i < 20; i++)
			{
				const ChainStat* cs = chains[i];
				std::fprintf(out, "    %-9s x%-6llu\n", RW_TYPE_NAME[cs->type],
					static_cast<unsigned long long>(cs->count));
				for (size_t f = 0; f < cs->chain.size(); f++)
					std::fprintf(out, "        %s\n", sky2::host().resolveSymbol(cs->chain[f]).c_str());
			}
		}
	}

	// RpLightCreate: the tracked create plus the P3FES_LIGHTLOG census of what kind of light it is.
	void ObjectManager::trackLightCreate(uint32_t obj, uint32_t createRa, uint32_t sp)
	{
		trackCreate(Light, obj, createRa, sp);
		if (obj && sky2::env("LIGHTLOG"))
		{
			// Census: does the game create positioned (point/spot) lights? If only 1/2 appear, the
			// world lighting is ambient+directional only and the sector light-tie path (Stage 2) is moot.
			const uint8_t type = guest::ptr<RpLight>(obj)->object.object.subType;
			const char* name = type == 1 ? "directional" : type == 2 ? "ambient" : type == 0x80 ? "point"
				: type == 0x81 ? "spot" : type == 0x82 ? "spotsoft" : "?";
			std::fprintf(stderr, "[lightlog] RpLightCreate 0x%08X type=%u (%s)\n", obj, type, name);
		}
	}
} // namespace sky2
