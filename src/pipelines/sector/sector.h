// World-sector pipeline: the BSP world geometry (RpWorldSector) the game renders via
// _rpSectorDefaultRenderCallBack. Owned by Device.
#pragma once

#include "geometry.h"
#include "object_manager.h" // ObjectManager::CachedSector

#include <cstdint>
#include <vector>

namespace sky2
{
	class Device;

	struct SectorPipeline
	{
		Device* device = nullptr; // injected by Device (queue)

		void render(uint32_t sectorAddr);

	private:
		ParsedGeometry pg; // reused per draw; EE thread only, and render() is not reentrant
		bool parse(uint32_t sectorAddr, ParsedGeometry& out);
		// The geometry-static half: vertices and per-mesh index lists, with the material left as an
		// address for the caller to re-read. This is what the sector cache holds.
		bool parseGeometry(uint32_t sectorAddr, const SectorFields& f, ObjectManager::CachedSector& out,
			bool perFrame);
		static ObjectManager::SectorSignature signatureFor(uint32_t sectorAddr, const SectorFields& f);
		// Sectors whose geometry ships pre-instanced as PS2 VIF packets, with the platform-independent
		// arrays left null. Each mesh decodes to its own tristrip, so they concatenate into one vertex
		// buffer and each submesh indexes its own range.
		bool parseNative(uint32_t sectorAddr, uint32_t nativeTable, ObjectManager::CachedSector& out, bool perFrame);
		// Expand one mesh-header entry's indices into a triangle list. Indices are tristrips (flag 0x1) or
		// a plain list; degenerate (repeated-vertex) tristrip triangles are skipped, as RW uses them to
		// stitch strips. Winding alternates each step so all triangles share the source orientation.
		static void appendMeshIndices(std::vector<uint16_t>& outIdx, const RpMesh& mesh, bool triStrip, int numVertices);
		static void logSector(uint32_t sectorAddr);
	};
} // namespace sky2
