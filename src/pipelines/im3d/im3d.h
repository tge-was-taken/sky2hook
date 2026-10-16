// Im3D pipeline (A2): projected 3D primitives (the field character-shadow quad; shuffle-time cards).
// Owned by Device; the per-draw stash lives in RenderQueue::im3d with the rest of the build state.
#pragma once

#include "common.h"

#include <cstdint>

namespace sky2
{
	class Device;

	struct Im3DPipeline
	{
		Device* device = nullptr; // injected by Device (queue)

		void transform(uint32_t vertsAddr, int numVerts, uint32_t ltmAddr, unsigned flags);
		void renderPrimitive(uint32_t primType);
		void renderIndexedPrimitive(uint32_t primType, uint32_t indicesAddr, int numIndices);
		void renderLine(int vert1, int vert2);
		void end();

	private:
		// Build a MeshDraw from the stashed verts + current render state (no indices yet). False if
		// there's nothing to draw (no camera / no stashed verts).
		bool beginMesh(MeshDraw& m);
		// Give the mesh its indices and decide where they live. False when there is nothing to draw,
		// in which case the vertices beginMesh appended have already been given back.
		bool finishMesh(MeshDraw& m, std::vector<uint16_t>&& idx);
		void discardMesh(const MeshDraw& m); // abandon a batch beginMesh already appended
		static glm::mat4 loadLtm(uint32_t ltmAddr);

		// Measured: every batch carries an identity transform, so the game writes world-space positions
		// into the vertex array and the bytes change whenever the object moves. That rules out keying
		// this geometry by content and leaves a per-frame arena as the way to make it cheap.
		void noteBatch(int count, const glm::mat4& model);
	};
} // namespace sky2
