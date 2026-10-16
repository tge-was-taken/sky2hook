#pragma once

#include "common.h" // FramePass

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace sky2
{
	// What the frame cost to build, for the overlay. `growths` counts pooled vectors that had to
	// reallocate this frame; it reaching zero on a repeating scene is the point of pooling.
	struct FrameStats
	{
		uint32_t passes = 0;
		uint32_t meshes = 0;
		uint32_t im2d = 0;
		uint32_t growths = 0;
		size_t frameBytes = 0; // capacity held by this frame's vectors
	};

	// One frame of render data: the ordered camera passes plus what the build thread learned while
	// producing them. Owned by a FrameRing slot and reused, so reset() keeps capacity rather than
	// freeing it.
	struct Frame
	{
		uint64_t index = 0; // monotonic, starts at 1; 0 means "never built"
		uint32_t displayRaster = 0; // the raster presented this frame (RwCameraShowRaster arg)
		std::vector<FramePass> passes;
		// Every skinned object's bone matrices for this frame, appended in draw order. A MeshDraw names
		// its slice with boneOffset/boneCount, so the render thread uploads this once per frame instead
		// of re-uploading one object's palette for each of its submeshes.
		std::vector<glm::mat4> bones;
		// Interpolation inputs, parallel to `bones` and filled only while it is enabled. `bones` holds
		// the composed pose * invBind that the shader wants; blending that product is wrong, because the
		// constant bind offset rides inside the interpolated translation and shortens limbs. So the two
		// factors are kept separately and recomposed after the pose is blended.
		std::vector<glm::mat4> poses, invBinds;
		bool wantPoses = false; // latched once per frame: the toggle must not change mid-build
		// Deduped per-draw state and lighting. A draw carries an index, so an object's submeshes share
		// one entry instead of each copying a 552-byte FrameLights.
		std::vector<DrawState> states;
		std::vector<FrameLights> lights;
		// Every draw of the frame, in submission order, plus the order to execute them in. A pass owns
		// a contiguous [cmdBegin, cmdEnd) slice of both; drawOrder permutes within that slice only
		// (opaque meshes, blended meshes, then Im2D), so a command's position in commandList is a
		// stable id the manifest and P3FES_DRAW_ONLY can both name.
		std::vector<FrameCmd> commandList;
		std::vector<uint32_t> drawOrder;
		std::vector<MeshDraw> meshCmds;
		std::vector<Im2DDraw> im2dCmds;
		// Every Im2D vertex of the frame. One upload per frame instead of a vector per batch and a
		// concatenation per pass.
		std::vector<Im2DVertex> im2dVerts;
		// The same treatment for geometry that exists for one frame (Im3D). A pool slot per batch meant
		// a GPU buffer per batch per frame; here a mesh names a slice and the whole arena uploads once.
		// Indices stay mesh-local and are drawn with a base vertex, so appending shifts nothing.
		std::vector<Vertex3D> transientVerts;
		std::vector<uint16_t> transientIdx;
		void buildDrawOrder(); // fill drawOrder from commandList + states; call once per frame at endBuild
		// Im3D primitives the build thread refused, by reason. A silent drop here is invisible in a
		// manifest that only lists what was drawn, and P4's sky is one primitive arriving and none
		// surviving.
		struct Im3DDrops { uint32_t noCamera = 0, noVerts = 0, badCount = 0, degenerate = 0; };
		Im3DDrops im3dDrops;
		// How many times the guest asked for an atomic or a sector, and how many of those produced no
		// draw at all. "We never saw it" and "we saw it and dropped it" are different bugs and a mesh
		// count cannot tell them apart.
		struct ObjectCalls { uint32_t atomics = 0, sectors = 0, atomicsNoDraw = 0, sectorsNoDraw = 0; };
		ObjectCalls objectCalls;
		FrameStats stats;

		// Clear for reuse while keeping every allocation. std::vector<FramePass>::clear() would destroy
		// the passes and free their mesh/im2d vectors, which is exactly the capacity worth keeping, so
		// the used prefix is cleared in place and the logical size tracked separately.
		void reset(uint64_t frameIndex);
		// Capacity of every pooled vector, sampled at reset and compared at endBuild. Without this
		// `growths` only saw the pass vector, which stopped holding anything once passes became ranges.
		struct Capacities
		{
			size_t passes = 0, bones = 0, poses = 0, invBinds = 0, states = 0, lights = 0;
			size_t commandList = 0, drawOrder = 0, meshCmds = 0, im2dCmds = 0, im2dVerts = 0;
			size_t transientVerts = 0, transientIdx = 0;
			bool operator==(const Capacities&) const = default;
		};
		Capacities capacities() const;
		void endBuild(); // trim the pass vector to what was actually used and total up the stats

		FramePass& openPass(); // reuse the next retained pass, or grow by one
		size_t passCount() const { return used; }
		// The passes built so far. During the build `passes` still holds retained spare entries past
		// `used`, so mid-frame readers must go through this rather than iterating the vector.
		std::span<const FramePass> livePasses() const { return {passes.data(), used}; }

	private:
		Capacities startCapacities;
		size_t used = 0; // passes[0, used) are live this frame; the rest are retained capacity
	};

	// Triple buffer between the EE build thread and the render thread. At most one slot is in each of
	// Building/Pending/Presenting, so acquireBuild always finds a Free one and never blocks (blocking
	// would stall the only thread dispatching guest calls). publish() drops an un-taken Pending back
	// to Free, matching the old behaviour where the render thread simply skips a frame it never took.
	class FrameRing
	{
	public:
		Frame* acquireBuild(); // build thread; always succeeds
		void publish(Frame* frame); // build thread; Building -> Pending, dropping any older Pending
		Frame* takePending(); // render thread; null when nothing new arrived
		// Build thread: pin the next frame started, so publish() waits for the render thread to take it
		// rather than dropping it, and return its index. Without a pin a capture lands on whichever
		// frame the render thread holds when it services the request, which moves with render speed.
		// onStart runs when that frame opens, the only point that precedes exactly its guest calls.
		uint64_t pinNextFrame(void (*onStart)() = nullptr);
		// Highest frame index the render thread has finished with. A later stage gates geometry-slot
		// recycling on this; kept here because the ring is what knows when a frame stops being read.
		uint64_t retiredIndex() const { return retired.load(std::memory_order_acquire); }
		// Render every published frame instead of skipping ones the render thread never took. A
		// measurement run needs it: the display-buffer feedback tap samples the previous rendered
		// frame, so skipping one changes the result and two runs of the same capture disagree.
		void renderEveryFrame(bool on) { dropFrames = !on; }

	private:
		static constexpr int FRAME_SLOTS = 3;
		enum class SlotState : uint8_t
		{
			Free,
			Building,
			Pending,
			Presenting
		};
		Frame frames[FRAME_SLOTS];
		SlotState state[FRAME_SLOTS] = {SlotState::Free, SlotState::Free, SlotState::Free};
		std::mutex mutex;
		std::condition_variable pinReleased; // signalled by takePending
		uint64_t pinned = 0; // frame index publish() must not drop; 0 = none
		// Drop a frame the render thread never took, which is what an interactive run wants. A
		// measurement run cannot: the display-buffer feedback tap samples the previous rendered
		// frame, so a dropped frame changes the result and the capture stops being reproducible.
		bool dropFrames = true;
		void (*pinStart)() = nullptr; // runs on the build thread when the pinned frame opens
		std::atomic<uint64_t> retired{0}; // frame indices start at 1, so 0 = nothing retired yet
		uint64_t nextIndex = 1;
	};
} // namespace sky2
