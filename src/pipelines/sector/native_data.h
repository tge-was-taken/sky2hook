// PS2 native (pre-instanced) geometry: some RenderWare builds ship world sectors as VIF DMA packets
// instead of the platform-independent vertex/triangle arrays, leaving RpWorldSector's arrays NULL.
// Decoding walks the packet and replays its UNPACKs to recover per-vertex attributes.
#pragma once

#include "common.h"

namespace sky2
{
	// One mesh's geometry: a single tristrip. The packet splits it into VU1-sized batches that each
	// repeat the previous batch's last two vertices; decoding rejoins them, so the result is as long
	// as the mesh header's numIndices. An attribute vector is empty if the packet omitted it.
	struct NativeMesh
	{
		std::vector<glm::vec3> pos;
		std::vector<glm::vec2> uv;
		std::vector<glm::vec4> color;
		std::vector<glm::vec3> normal;
		std::vector<glm::vec4> boneIndex;
		std::vector<glm::vec4> boneWeight;
	};

	// Address of the per-mesh RwResEntry table the native-data plugin hangs off a world sector or an
	// atomic's geometry, or 0 if this build/disc has no such plugin. Both are {count, entry[count]}.
	uint32_t sectorNativeTable(uint32_t sectorAddr);
	uint32_t geometryNativeTable(uint32_t geomAddr);

	// Decode mesh m's packet. False if the entry is missing or not a chain we handle.
	bool decodeNativeMesh(uint32_t nativeTableAddr, int mesh, NativeMesh& out);

	// How to read a decoded native mesh's vertex run as triangles.
	enum class NativePrim
	{
		Strip, // one tristrip; winding alternates each step
		QuadList // independent 4-vertex quads in cycle order, two triangles each
	};

	// The primitive an RpMeshHeader's flags name. A pointlist mesh has no RenderWare-defined topology
	// (`ps2allatomic.h`: "We can't guess what primitive type you'll submit"), so the game's own packet
	// decides; P4's 3D sprite generator emits independent quads there, which the GS batch confirms by
	// submitting exactly six vertices per four decoded.
	NativePrim nativePrimFor(uint32_t meshHeaderFlags);

	// Expand a decoded mesh's vertex run over verts[base, base+count) into triangles. A repeated
	// position is one of RW's stitching steps and drops out.
	void appendNativePrims(std::vector<uint16_t>& outIdx, std::span<const Vertex3D> verts,
		uint32_t base, uint32_t count, NativePrim prim);

	// Summarise one mesh's packet: unpack formats and the decoded strip (P3FES_LOG_SECTOR).
	void logNativeMesh(uint32_t nativeTableAddr, int mesh);
} // namespace sky2
