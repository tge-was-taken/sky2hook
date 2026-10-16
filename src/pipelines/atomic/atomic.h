// Atomic pipeline: the RpAtomic model geometry (characters, props, the skybox) the game renders via
// AtomicDefaultRenderCallBack. Owned by Device.
#pragma once

#include "geometry.h" // ParsedGeometry: the reused per-draw scratch
#include "object_manager.h"

#include <cstdint>

namespace sky2
{
	class Device;

	struct AtomicPipeline
	{
		Device* device = nullptr; // injected by Device (queue, objects)

		void render(uint32_t atomicAddr);

	private:
		ParsedGeometry pg; // reused per draw; EE thread only, and render() is not reentrant

	private:
		// Read an RpGeometry out of EE RAM into the geometry-static cache entry: host vertices (pos/normal/
		// uv/color + skin weights) + per-material index lists + geometry flags. No atomic or per-frame state
		// (transform/bones/material config are applied in render). Returns false if the geometry is not
		// renderable. The ObjectManager caches the result by geometry address.
		// `perFrame` geometry is reclaimed by the pool once the retire fence passes the frame that
		// built it; cached geometry lives until ObjectManager drops the entry.
		bool parseGeomStatic(uint32_t geomAddr, ObjectManager::CachedGeom& out, bool perFrame);
		// Geometry shipped pre-instanced as PS2 VIF packets, with the vertex/triangle arrays left null.
		// One tristrip per mesh, so the meshes concatenate into one vertex buffer and each submesh
		// indexes its own range.
		bool parseGeomNative(uint32_t geomAddr, uint32_t nativeTable, ObjectManager::CachedGeom& out, bool perFrame);
	};
} // namespace sky2
