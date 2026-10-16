#include "frame.h"

#include "sky2/config.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace sky2
{
	namespace
	{
		// Recycled storage returns last frame's bytes, so a use-after-reset reads plausible data
		// instead of obvious garbage. Poisoning makes that class of bug visible in a capture.
		void poison(std::vector<MeshDraw>& meshes, std::vector<Im2DDraw>& im2d, std::vector<Im2DVertex>& verts,
			std::vector<Vertex3D>& transientVerts)
		{
			for (MeshDraw& m : meshes)
			{
				m.verts = GEOM_ID_NONE;
				m.idx = GEOM_ID_NONE;
				m.model = glm::mat4(0.0f);
				m.idxCount = 0;
				m.diag = MeshDiag{}; // identity too: a stale match is as wrong as a stale transform
			}
			for (Im2DDraw& b : im2d)
				b.texKey.reset();
			Im2DVertex bad{};
			bad.x = bad.y = std::numeric_limits<float>::quiet_NaN();
			std::fill(verts.begin(), verts.end(), bad);
			Vertex3D badVert{};
			badVert.pos = glm::vec3(std::numeric_limits<float>::quiet_NaN());
			std::fill(transientVerts.begin(), transientVerts.end(), badVert);
		}
	} // namespace

	void Frame::reset(uint64_t frameIndex)
	{
		objectCalls = {};
		index = frameIndex;
		displayRaster = 0;
		im3dDrops = {};
		const bool pooled = sky2::config().framePool;
		const bool wantPoison = sky2::config().poolPoison;
		if (!pooled)
		{
			passes.clear();
			passes.shrink_to_fit(); // the reference path: allocate fresh every frame
			bones.clear();
			bones.shrink_to_fit();
			poses.clear();
			poses.shrink_to_fit();
			invBinds.clear();
			invBinds.shrink_to_fit();
			states.clear();
			states.shrink_to_fit();
			lights.clear();
			lights.shrink_to_fit();
			commandList.clear();
			commandList.shrink_to_fit();
			drawOrder.clear();
			drawOrder.shrink_to_fit();
			meshCmds.clear();
			meshCmds.shrink_to_fit();
			im2dCmds.clear();
			im2dCmds.shrink_to_fit();
			im2dVerts.clear();
			im2dVerts.shrink_to_fit();
			transientVerts.clear();
			transientVerts.shrink_to_fit();
			transientIdx.clear();
			transientIdx.shrink_to_fit();
			used = 0;
			stats = FrameStats{};
			startCapacities = capacities();
			return;
		}
		if (wantPoison)
		{
			const glm::mat4 bad(std::numeric_limits<float>::quiet_NaN());
			std::fill(bones.begin(), bones.end(), bad);
			std::fill(poses.begin(), poses.end(), bad);
			std::fill(invBinds.begin(), invBinds.end(), bad);
		}
		bones.clear(); // keeps capacity: the arena is the same size every frame in a steady scene
		poses.clear();
		invBinds.clear();
		states.clear();
		lights.clear();
		if (wantPoison)
			poison(meshCmds, im2dCmds, im2dVerts, transientVerts);
		// FramePass holds no vectors any more, so clearing the used prefix costs nothing to keep.
		for (size_t i = 0; i < used; i++)
			passes[i] = FramePass{};
		used = 0;
		commandList.clear();
		drawOrder.clear();
		meshCmds.clear();
		im2dCmds.clear();
		im2dVerts.clear();
		transientVerts.clear();
		transientIdx.clear();
		stats = FrameStats{};
		startCapacities = capacities();
	}

	// Submission order, which is the only order the GS has. Sorting Im2D after a pass's meshes broke
	// the full-screen distortion effects: the tap samples the frame buffer at its own draw time, so
	// moving meshes ahead of it changes what it picks up, and UI the game draws before the tap has to
	// be in it while UI drawn after has to sit on top.
	void Frame::buildDrawOrder()
	{
		drawOrder.clear();
		drawOrder.reserve(commandList.size());
		for (const FramePass& p : passes)
			for (uint32_t i = p.cmdBegin; i < p.cmdEnd; i++)
				drawOrder.push_back(i);
	}

	Frame::Capacities Frame::capacities() const
	{
		return {passes.capacity(), bones.capacity(), poses.capacity(), invBinds.capacity(),
			states.capacity(), lights.capacity(),
			commandList.capacity(), drawOrder.capacity(), meshCmds.capacity(), im2dCmds.capacity(),
			im2dVerts.capacity(), transientVerts.capacity(), transientIdx.capacity()};
	}

	FramePass& Frame::openPass()
	{
		if (used < passes.size())
		{
			FramePass& p = passes[used++];
			p.cmdBegin = p.cmdEnd = static_cast<uint32_t>(commandList.size());
			return p;
		}
		passes.emplace_back();
		used++;
		passes.back().cmdBegin = passes.back().cmdEnd = static_cast<uint32_t>(commandList.size());
		return passes.back();
	}

	void Frame::endBuild()
	{
		passes.resize(used); // hand the render thread only the passes actually built this frame
		buildDrawOrder();
		if (!(capacities() == startCapacities))
			stats.growths++; // some pooled vector reallocated: the pool has not converged yet
		stats.passes = static_cast<uint32_t>(used);
		stats.meshes = static_cast<uint32_t>(meshCmds.size());
		stats.im2d = static_cast<uint32_t>(im2dCmds.size());
		stats.frameBytes = passes.capacity() * sizeof(FramePass) +
						   (bones.capacity() + poses.capacity() + invBinds.capacity()) * sizeof(glm::mat4) +
						   states.capacity() * sizeof(DrawState) + lights.capacity() * sizeof(FrameLights) +
						   commandList.capacity() * sizeof(FrameCmd) + drawOrder.capacity() * sizeof(uint32_t) +
						   meshCmds.capacity() * sizeof(MeshDraw) + im2dCmds.capacity() * sizeof(Im2DDraw) +
						   im2dVerts.capacity() * sizeof(Im2DVertex) +
						   transientVerts.capacity() * sizeof(Vertex3D) +
						   transientIdx.capacity() * sizeof(uint16_t);
		// growths falling to zero on a repeating scene is the whole point of the ring; log it so that
		// is observable rather than assumed.
		static const bool sLogAlloc = sky2::env("LOG_FRAME_ALLOC") != nullptr;
		if (sLogAlloc && index % 60 == 0)
		{
			std::fprintf(stderr,
				"[sky2][alloc] frame %llu passes=%u meshes=%u im2d=%u growths=%u pool=%.1f KB "
				"states=%zu lights=%zu\n",
				static_cast<unsigned long long>(index), stats.passes, stats.meshes, stats.im2d,
				stats.growths, stats.frameBytes / 1024.0, states.size(), lights.size());
		}
	}

	Frame* FrameRing::acquireBuild()
	{
		Frame* opened = nullptr;
		void (*notify)() = nullptr;
		{
			std::lock_guard<std::mutex> lk(mutex);
			for (int i = 0; i < FRAME_SLOTS; i++)
			{
				if (state[i] != SlotState::Free)
				{
					continue;
				}
				state[i] = SlotState::Building;
				frames[i].reset(nextIndex++);
				opened = &frames[i];
				if (pinStart && opened->index == pinned)
				{
					notify = pinStart;
					pinStart = nullptr;
				}
				break;
			}
		}
		SKY2_ASSERT(opened, "FrameRing has no free slot; the state invariant was violated");
		if (notify)
		{
			notify(); // outside the lock: it reaches another thread
		}
		return opened ? opened : &frames[0];
	}

	uint64_t FrameRing::pinNextFrame(void (*onStart)())
	{
		std::lock_guard<std::mutex> lk(mutex);
		pinned = nextIndex; // the index acquireBuild hands out next
		pinStart = onStart;
		return pinned;
	}

	void FrameRing::publish(Frame* frame)
	{
		std::unique_lock<std::mutex> lk(mutex);
		if (!dropFrames)
		{
			// Pace the build thread to the render thread rather than skipping a frame. Bounded for the
			// same reason the pin below is: a render thread that is not running must not wedge the only
			// thread dispatching guest calls, so a timeout falls back to dropping.
			pinReleased.wait_for(lk, std::chrono::seconds(5), [&] {
				for (const SlotState& s : state)
					if (s == SlotState::Pending)
						return false;
				return true;
			});
		}
		int slot = -1;
		for (int i = 0; i < FRAME_SLOTS; i++)
		{
			if (state[i] == SlotState::Pending)
				state[i] = SlotState::Free; // the render thread never took it; skip it as before
			if (&frames[i] == frame)
				slot = i;
		}
		if (slot < 0)
		{
			SKY2_ASSERT(false, "publishing a frame that does not belong to this ring");
			return;
		}
		state[slot] = SlotState::Pending;
		if (pinned && frame->index == pinned)
		{
			// Bounded: a render thread that is not running must not wedge the only thread dispatching
			// guest calls. A timeout leaves the capture to land on a later frame, as it did before.
			if (!pinReleased.wait_for(lk, std::chrono::seconds(5),
					[&] { return state[slot] != SlotState::Pending; }))
			{
				SKY2_ERROR(false, "pinned frame %llu was never taken by the render thread",
					static_cast<unsigned long long>(frame->index));
			}
			pinned = 0;
		}
	}

	Frame* FrameRing::takePending()
	{
		std::lock_guard<std::mutex> lk(mutex);
		int pending = -1;
		for (int i = 0; i < FRAME_SLOTS; i++)
			if (state[i] == SlotState::Pending)
				pending = i;
		if (pending < 0)
			return nullptr; // nothing new: the caller keeps re-rendering what it holds
		for (int i = 0; i < FRAME_SLOTS; i++)
		{
			if (state[i] == SlotState::Presenting)
			{
				state[i] = SlotState::Free;
				retired.store(frames[i].index, std::memory_order_release);
			}
		}
		state[pending] = SlotState::Presenting;
		pinReleased.notify_all(); // release a build thread waiting on a pinned frame
		return &frames[pending];
	}
} // namespace sky2
