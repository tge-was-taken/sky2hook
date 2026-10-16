// PTank pipeline: an RpAtomic that holds a batch of particles instead of a mesh (RpPTank). Its
// RpGeometry is an empty shell and the attributes live in the plugin's own buffers, so the atomic
// pipeline has nothing to read and the particles reach the GS without passing through any geometry
// sky2 can see. Owned by Device.
#pragma once

#include "glm/glm.hpp"

#include <cstdint>

namespace sky2
{
	class Device;

	// One attribute buffer: base plus stride, which is the same shape for both data organisations
	// (parallel arrays give the attribute size, an interleaved block gives the structure size).
	struct PTankCluster
	{
		uint32_t data = 0;
		int32_t stride = 0;
		explicit operator bool() const { return data != 0; }
		uint32_t at(int particle) const
		{
			return data + static_cast<uint32_t>(particle) * static_cast<uint32_t>(stride);
		}
	};

	// A tank as the guest holds it, read once per render. Whether an attribute is per particle or one
	// value for the whole tank is decided by the cluster pointer: the per-particle flag and its
	// rpPTANKDFLAGCNS* twin are mutually exclusive, so a null cluster says the same thing without
	// depending on the CNS bit values.
	// The GS state the Sky render node writes into the tank's DMA packet, read out of the platform's
	// RpPTankSkyData. This is where a tank's blend and alpha test live: the RW srcBlend/dstBlend words
	// beside them are left unset on this platform.
	struct PTankSkyState
	{
		bool valid = false; // the pointer resolved and the registers decode; false means ignore it all
		uint64_t alpha = 0, test = 0, tex1 = 0; // GS ALPHA_1 / TEST_1 / TEX1_1
		uint64_t prim = 0; // GIFtag the node sends, before it ORs in TME/ABE
		uint32_t primType = 0; // GS primitive: 6 is SPRITE, whose two vertices are screen-space corners
		uint32_t batchSize = 0, numClusters = 0;
		uint32_t transformInfo = 0; // diagnostics: which half of the layout resolved
	};

	struct ParsedPTank
	{
		int32_t maxCount = 0, actCount = 0; // capacity, and the live particles this frame
		uint32_t dataFlags = 0; // RpPTankDataFlags
		uint32_t platFlags = 0; // which sprite generator built this tank
		uint32_t numClusters = 0, structStride = 0;
		PTankCluster position, matrix, size, color, vtxColor, rotate, uv2, uv4;
		glm::vec2 cSize{0.0f};
		float cRotate = 0.0f;
		glm::vec4 cColor{255.0f};
		bool cVtxColor = false; // the four corner colours below stand in for a vtxColor cluster
		glm::vec4 cVtxColorValue[4] = {};
		glm::vec2 cUV[4] = {};
		bool vertexAlphaBlend = false; // becomes PRIM.ABE: this tank blends at all
		PTankSkyState sky;
		uint32_t material = 0; // material 0 of the atomic's geometry: one texture for the whole tank
	};

	struct PTankPipeline
	{
		Device* device = nullptr; // injected by Device (queue, textures)

		// True when the PTank plugin is attached to this atomic. Cheap: one guest read.
		static bool isPTank(uint32_t atomicAddr);
		void render(uint32_t atomicAddr);

	private:
		// Read the plugin's buffers into `pt`. Returns false when there is nothing to draw, which is a
		// normal state for a tank between bursts.
		bool parse(uint32_t atomicAddr, ParsedPTank& out) const;
		// Expand the live particles into camera-facing quads in the frame's arena, and enqueue them.
		void submitQuads(uint32_t atomicAddr, const ParsedPTank& pt);

		ParsedPTank pt; // reused per draw; EE thread only, and render() is not reentrant
	};
} // namespace sky2
