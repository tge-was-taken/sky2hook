// sky2 object manager: the RwRaster lifecycle tracker. Owns the registry of every raster the game
// creates, the display / render-target classification, GS-resident-RT tracing, the current movie
// frame, and the RW create/destroy lifecycle tracker. The texture manager consumes it. One instance,
// owned by the Device.
#pragma once

#include "common.h" // guest, RwRaster, TextureId, macros
#include "geometry_pool.h" // GeometryPool: cached geometry owns pool slots

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sky2
{
	class TextureManager;

	class ObjectManager
	{
	public:
		TextureManager* textures = nullptr; // injected by Device (invalidate + enqueueDestroy on raster events)
		GeometryPool* geometry = nullptr; // injected by Device (cached geometry's buffers)

		// Record a raster in the registry the first time sky2 sees it.
		void registerRaster(uint32_t rasterAddr);
		// rwSTANDARD hooks (also exposed as the free-function sky2::api:: API for the hook layer).
		void rasterCreate(uint32_t rasterAddr);
		void rasterUnlock(uint32_t rasterAddr);
		void rasterDestroy(uint32_t rasterAddr);
		// Which half of the GS double buffer a raster is. The pointers keep their role for the whole run
		// (a flip rewrites the GS address inside the raster, not the globals), so this is stable once
		// known. Unknown means we learned the raster from a source that doesn't say which it is.
		enum class BufferRole
		{
			Unknown,
			Display, // presented; a draw sampling it wants the previous frame
			Draw // being rendered into; a draw sampling it wants the frame so far
		};
		// `source` names how the raster was identified, for the registration log.
		void registerFrameBufferRaster(uint32_t rasterAddr, BufferRole role = BufferRole::Unknown,
			const char* source = "buffer getter");
		BufferRole frameBufferRole(uint32_t rasterAddr) const;
		void setMovieFrame(uint32_t bufAddr);
		// P3FES_DUMP_RASTER: dump the listed rasters' fields + extension bytes once (raster archaeology).
		void dumpWatchedRasters();
		int dumpRasterFrames = 0;
		// A draw referenced a GS-resident render target with no EE pixels to decode: trace its origin once.
		void traceGsResidentRaster(uint32_t rasterAddr, int depth, uint8_t cType, uint8_t cFmt, int tw, int th);
		bool isRenderTargetRaster(uint32_t rasterAddr) const
		{
			return rasterAddr != 0 && renderTargetRasters.count(rasterAddr) != 0;
		}

		// Registry of every RwRaster the game creates (RasterCreate), keyed by guest address. EE-thread.
		struct RasterInfo
		{
			int width, height, depth;
			uint8_t type; // RwRasterType (cType & rwRASTERTYPEMASK)
			uint8_t cFormat; // PS2 pixel format byte
			bool operator==(const RasterInfo&) const = default;
		};
		std::unordered_map<uint32_t, RasterInfo> rasterRegistry;

		// On-screen (ShowRaster) targets, and off-screen color RTs the game later samples as a texture.
		std::unordered_set<uint32_t> displayRasters;
		std::unordered_set<uint32_t> renderTargetRasters;
		// Rasters the game filled through the CPU (RasterUnlock), so their EE pixels are real data.
		std::unordered_set<uint32_t> cpuWrittenRasters;

		// Every raster seen as a camera render target + how the pass was classified (GS-resident tracing).
		struct CameraTargetInfo
		{
			int w = 0, h = 0;
			const char* kind = "?";
		};
		std::unordered_map<uint32_t, CameraTargetInfo> cameraTargetsSeen;

		// Rasters the game treats as a frame buffer (framebuffer feedback), + the GS buffer's size.
		std::unordered_map<uint32_t, BufferRole> frameBufferRasters;
		int frameBufferW = 0;
		int frameBufferH = 0;
		// Set when a draw samples the displayed buffer, so the render thread only pays for the
		// previous-frame copy in games that actually read it.
		std::atomic<bool> prevFrameWanted{false};

		// Latest Sofdec/MPEG decoded-frame buffer in EE RAM; frameId bumps each decode for cache refresh.
		uint32_t movieBuf = 0;
		uint64_t movieFrameId = 0;

		// Set when a draw samples an unsampleable GS-resident RT; the host polls it (P3FES_RT_AUTOCAP).
		std::atomic<bool> rtSampleWanted{false};

		// ---- Parsed-geometry cache (the ResEntry-style instance cache) ----
		// Reading an RpGeometry out of guest RAM into host verts + per-material index lists is the per-frame
		// hotspot and is geometry-static, so cache it by geometry address; the transform/bones/material
		// state is applied fresh per frame. Dropped on geometry modify/destroy. EE-thread only, so no lock.
		struct CachedGeom
		{
			GeomId verts = GEOM_ID_NONE;
			struct Sub
			{
				int matIndex; // index into the geometry's material list, or -1 for the no-material case
				GeomId indices = GEOM_ID_NONE;
				bool prelitAlpha = false; // RW's vertex-alpha blend term, computed at parse
			};
			std::vector<Sub> subs;
			int vcount = 0;
			bool lit = false, hasNormals = false, hasVertexColor = false, hasSkinData = false;
			bool native = false; // parsed from the pre-instanced VIF table, not the RW vertex arrays
			uint32_t geomFlags = 0;
			int numTexCoordSets = 0;
			uint32_t texCoordsAddr = 0;
			// The hook announcing a geometry edit fires whether or not anything changed. Mark rather
			// than drop, so the re-parse has something to compare against.
			bool dirty = false;
		};
		// ---- Sector cache ----
		// World sectors are re-read out of guest RAM every frame, and on pre-instanced discs that means
		// walking the VIF DMA chain per sector per frame. They are static between area loads, so cache
		// the parse by sector address.
		//
		// Staleness has four defences, in order of authority: RpWorldDestroy (an area load), a
		// curWorld change, RpWorldUnlock (in-place edit), and a signature revalidated on every render
		// call. The signature is what makes this safe on the three serials with no symbol map, where
		// the two hook addresses are unknown.
		struct SectorSignature
		{
			uint32_t triangles = 0, vertices = 0, normals = 0, texCoords0 = 0, preLitLum = 0, mesh = 0;
			int numVertices = 0, numTriangles = 0;
			uint32_t meshFlags = 0, totalIndices = 0;
			uint32_t nativeTable = 0; // pre-instanced sectors keep their geometry here, not in `vertices`
			int numMeshes = 0;
			bool operator==(const SectorSignature&) const = default;
		};
		struct CachedSector
		{
			GeomId verts = GEOM_ID_NONE;
			struct Sub
			{
				uint32_t materialAddr = 0; // re-read per frame: material state is not geometry
				GeomId indices = GEOM_ID_NONE;
				bool prelitAlpha = false; // RW's vertex-alpha blend term, computed at parse
			};
			std::vector<Sub> subs;
			int vcount = 0;
			bool lit = false, hasNormals = false, hasVertexColor = false;
			bool native = false; // parsed from the pre-instanced VIF table, not the RW vertex arrays
			int numTexCoordSets = 0;
			uint32_t pipeline = 0; // a cache hit still feeds recordObjectPipeline
			uint32_t world = 0;
			SectorSignature sig;
		};
		const CachedSector* findSector(uint32_t sectorAddr) const;
		CachedSector& putSector(uint32_t sectorAddr, CachedSector&& s);
		void invalidateSector(uint32_t sectorAddr);
		void invalidateWorldSectors(uint32_t worldAddr); // RpWorldDestroy / RpWorldUnlock / world change
		void releaseSector(const CachedSector& s);
		std::unordered_map<uint32_t, CachedSector> sectorCache;

		const CachedGeom* findGeom(uint32_t geomAddr) const; // nullptr on miss
		CachedGeom& putGeom(uint32_t geomAddr, CachedGeom&& g); // store and return the stored entry
		void invalidateGeom(uint32_t geomAddr); // the geometry is gone: drop the entry
		void markGeomDirty(uint32_t geomAddr); // it may have changed: re-parse and compare before believing it
		// The entry to draw with after a re-parse: the cached one when the two match, so its buffers keep
		// the ids the GPU already holds, else the new one. Takes ownership either way.
		CachedGeom& refreshGeom(uint32_t geomAddr, CachedGeom&& reparsed);
		bool sameGeom(const CachedGeom& a, const CachedGeom& b) const;
		void releaseGeom(const CachedGeom& g); // return the entry's pool slots
		std::unordered_map<uint32_t, CachedGeom> geomCache;

		// ---- RW object lifecycle tracker (folded from rw_tracker) ----
		// Hooks the RW create/destroy entry points and keeps per-type counts + the live set with each
		// object's create site. A diagnostic for now; the foundation for caching converted atomics and
		// invalidating them on destroy. On by default; P3FES_NO_RWTRACK disables it. EE-thread only.
		enum RwType
		{
			Frame,
			Atomic,
			Geometry,
			Texture,
			Raster,
			Material,
			Clump,
			Light,
			Camera,
			RwTypeCount
		};
		void trackerInit(); // enable unless P3FES_NO_RWTRACK; load symbols
		bool trackerEnabled() const { return trackEnabled; }
		void trackCreate(RwType t, uint32_t obj, uint32_t createRa, uint32_t sp); // post-hook: obj = return
		// How many times an RpAtomic has been created at this address. Frames stamp it into each draw so
		// that a recycled address fails a cross-frame match instead of being read as the same object.
		// 0 when the tracker is off, which is also the "do not trust this identity" value.
		uint32_t generationOf(uint32_t obj) const
		{
			const auto it = generations.find(obj);
			return it == generations.end() ? 0 : it->second;
		}
		void trackDestroy(RwType t, uint32_t obj); // pre-hook: obj being destroyed
		void trackLightCreate(uint32_t obj, uint32_t createRa, uint32_t sp); // trackCreate + P3FES_LIGHTLOG
		// P3FES_TRACE_DRAWBUF: print the EE call chain reaching a frame-buffer access, once per stack.
		void traceEeChain(const char* tag, uint32_t id, uint32_t ra, uint32_t sp);
		void report(const char* reason, std::FILE* out = stderr) const; // per-type counts + top sites/chains
		void describe(uint32_t addr, std::FILE* out = stderr) const; // one live object's origin

		struct Stat
		{
			uint64_t created = 0, destroyed = 0;
			uint32_t live = 0, peakLive = 0;
		};
		struct Obj
		{
			uint8_t type;
			uint32_t createRa;
			uint64_t chainKey; // 0 unless a call-chain was captured (stack mode)
		};
		struct ChainStat
		{
			uint64_t count = 0;
			uint8_t type = 0;
			std::vector<uint32_t> chain;
		};
		bool trackEnabled = false;
		bool trackStackMode = false; // P3FES_RWTRACK_STACK: capture a call-chain per create
		std::unordered_set<uint32_t> trackWatch; // P3FES_RWTRACK_WATCH: addrs to log on sight
		Stat trackStat[RwTypeCount];
		std::unordered_map<uint32_t, Obj> trackLive; // object address -> info
		std::unordered_map<uint32_t, uint32_t> generations; // atomic address -> creates seen there
		std::unordered_map<uint64_t, uint64_t> trackSites; // (createRa << 4 | type) -> count
		std::unordered_map<uint64_t, ChainStat> trackChains;
	};

} // namespace sky2
