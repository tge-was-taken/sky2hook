#pragma once

#include "common.h" // MeshDraw, FramePass, CameraParts, Vertex3D

#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sky2
{
	struct Frame;

	// Draws display frames between two 30 Hz guest ticks instead of re-presenting the same one.
	// Render thread only, and it must copy out of a Frame while that frame is still Presenting: the
	// ring recycles a slot as soon as the next takePending() returns.
	class Interpolator
	{
	public:
		// A draw's identity across ticks. The address alone is not enough: the game re-poses one atomic
		// and draws it several times inside a pass, and RW hands a freed address straight back to the
		// next create.
		struct Key
		{
			uint32_t sourceAddr = 0;
			uint32_t materialKey = 0;
			uint32_t generation = 0;
			uint32_t pass = 0; // a pass identity, not its position: see passKeyOf
			uint16_t drawOrdinal = 0;
			bool operator==(const Key&) const = default;
		};
		struct KeyHash
		{
			size_t operator()(const Key& k) const;
		};

		void setEnabled(bool on);
		bool enabled() const { return on.load(std::memory_order_relaxed); }

		void captureFrameBeingReplaced(const Frame& f);
		// `f` is the tick just taken. Ticks the ring dropped leave a gap in the frame index, and the two
		// frames either side of one are two updates apart: blending them over a single period runs the
		// world at double speed for that tick, so the gap snaps instead.
		void beginTick(const Frame& f);
		// Latch the blend point for one displayed frame. Everything drawn in that frame reads the same
		// value: sampling the clock per draw gave the camera one instant and the meshes another, which
		// shears the world against the camera whenever either moves.
		void beginDisplayFrame(const Frame& f);
		float alpha() const { return displayAlpha; }
		uint64_t tickIndex() const { return ticks; }

		// Each returns the frame's own value when disabled, unmatched, or implausible. Bones and Im3D
		// vertices come back as whole arenas because both are uploaded once before any pass runs.
		glm::mat4 modelFor(const MeshDraw& m, uint32_t pass, const glm::mat4& curr) const;
		glm::mat4 vpFor(const FramePass& p, uint32_t pass) const;
		const std::vector<glm::mat4>& bonesFor(const Frame& f) const;
		const std::vector<Vertex3D>& transientVertsFor(const Frame& f) const;
		// 2D blends only the draws a game hook named (Im2DDraw::interpTag). Nothing in a 2D draw says
		// which element it is, and every host-side guess tried so far blended unrelated draws together.
		const std::vector<Im2DVertex>& im2dVertsFor(const Frame& f) const;

		void logFrame() const; // P3FES_LOG_INTERP; defined in interpolator_debug.cpp
		double tickPeriodMs() const { return periodMs; }

		// 30 Hz on a 59.94 Hz field rate, the rate every measured scene publishes at. Only the starting
		// estimate: the clock tracks whatever the emulator turns out to be doing.
		static constexpr double NOMINAL_PERIOD_MS = 1000.0 / 29.97;

		// Counted over every display frame of one tick, so with N display frames per tick each draw is
		// counted N times. "static" is not a failure: an object that did not move needs no blending.
		struct Stats
		{
			int meshInterpolated = 0, meshSnapped = 0, meshStatic = 0;
			int passInterpolated = 0, passSnapped = 0, passStatic = 0;
			int bonesInterpolated = 0, bonesSnapped = 0; // per object, not per bone
			int im3dInterpolated = 0, im3dSnapped = 0;
			int im2dBlended = 0, im2dStatic = 0, im2dSnapped = 0; // tagged draws only
			// Why a tagged draw snapped, since the three causes want different fixes: no match is an
			// identity problem, a cut is a threshold, at-rest is the motion rule refusing to smooth.
			int im2dNoMatch = 0, im2dCut = 0, im2dFromRest = 0;
			int droppedTicks = 0; // publishes the ring never showed, so the pair straddles two updates
			int nonRigid = 0; // matrices with no extractable rotation; blended element-wise
			float minBasisDet = 2.0f;
			float maxBoneStep = 0.0f;
			double tickMs = 0.0;
		};
		mutable Stats stats;

	private:
		struct PassPose
		{
			uint32_t camera = 0, target = 0;
			CameraParts cam;
		};
		struct Slice
		{
			uint32_t offset = 0, count = 0;
			TextureId texKey = 0; // Im3D only: a shadow quad names the render target it samples
			// Im2D only: the furthest any of its vertices travelled to get here, so the next tick can
			// tell an element that is already animating from one the game just placed somewhere else.
			float step = 0.0f;
		};
		template <class T>
		using ByKey = std::unordered_map<Key, T, KeyHash>;

		std::atomic<bool> on{false};

		// Two buffers, not one. `pending` holds the frame being rendered now and only becomes `prev`
		// when a newer frame replaces it. Writing straight into `prev` makes every display frame after
		// the first in a tick interpolate from itself, which reads as a two-position stutter.
		ByKey<glm::mat4> prevModels, pendingModels;
		ByKey<Slice> prevBones, pendingBones;
		ByKey<Slice> prevIm3D, pendingIm3D;
		std::unordered_map<uint64_t, Slice> prevIm2D, pendingIm2D; // (tag, repeat) -> vertex slice
		std::vector<glm::mat4> prevPoses, pendingPoses;
		std::vector<Vertex3D> prevTransient, pendingTransient;
		std::vector<Im2DVertex> prevIm2DVerts, pendingIm2DVerts;
		std::vector<PassPose> prevPasses, pendingPasses;
		// A pass's identity, by position in each frame. Which camera drew into which target, and which
		// repeat of that pair it is, so a pass added or dropped between ticks shifts nothing after it.
		// The mesh keys carry it too: keying a draw by the position of its pass made every draw behind a
		// vanished shadow pass miss, and a whole-frame miss is a whole-frame snap.
		std::vector<uint32_t> currPassKeys, prevPassKeys, pendingPassKeys;
		std::unordered_map<uint32_t, PassPose> prevPassByKey;
		mutable std::vector<glm::mat4> boneScratch;
		mutable std::vector<Vertex3D> transientScratch;
		mutable std::vector<Im2DVertex> im2dScratch;

		uint64_t ticks = 0;
		uint64_t capturedIndex = 0;
		bool havePending = false;
		bool havePrev = false;

		// Phase-locked: advance by the nominal period and correct slowly toward the observed arrival.
		// The publish timestamp jitters by up to a vsync inside the EE's emulation burst, half a tick,
		// which driven into alpha directly would be a velocity discontinuity every tick.
		using Clock = std::chrono::steady_clock;
		Clock::time_point tickStart;
		Clock::time_point lastTick;
		float displayAlpha = 1.0f; // latched per displayed frame by beginDisplayFrame
		// Measured, not assumed. A fixed period leaves a standing phase error against any rate the
		// emulator actually runs at, and the correction below only cancels a constant offset: with a
		// per-tick error e it settles at 20e, which drifts until the hard resync throws a jump.
		double periodMs = NOMINAL_PERIOD_MS;
		int offRate = 0; // consecutive intervals far from the estimate, before it re-seeds
	};
} // namespace sky2
