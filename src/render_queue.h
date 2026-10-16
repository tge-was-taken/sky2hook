// sky2 render queue: the EE build-thread producer. Accumulates the frame's camera passes + 2D/glyph
// draws and mirrors the game's render state, then hands a finished frame to the Renderer at endFrame.
// One instance, owned by the Device.
#pragma once

#include "frame.h" // Frame, FrameRing, FramePass, FrameLights, StencilState, ATST_/AFAIL_ enums
#include "geometry_pool.h" // GeometryPool: the buffers a draw's GeomIds name

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace sky2
{
	struct ParsedGeometry;
	struct SubMesh;
	class Renderer;
	class ObjectManager;
	class TextureManager;

	class RenderQueue
	{
	public:
		Renderer* renderer = nullptr; // injected by Device (frame submit at endFrame)
		ObjectManager* objects = nullptr; // injected by Device (display-raster classification)
		FrameRing* frames = nullptr; // injected by Device (the pooled frame storage)
		GeometryPool* geometry = nullptr; // injected by Device (per-frame reclaim + reference stamping)
		TextureManager* textures = nullptr; // injected by Device (the blend predicate's texture term)

		// Im3D stashes its vertex block at RwIm3DTransform and consumes it at the following
		// RenderPrimitive. Build-thread state like everything else here, so it lives with the rest
		// rather than in a file static inside the pipeline.
		struct Im3DBuild
		{
			uint32_t vertsAddr = 0;
			int count = 0;
			bool haveUV = false;
			glm::mat4 model{1.0f};
			uint32_t ltmAddr = 0; // what the matrix was loaded from; 0 = RwIm3DTransform passed none
			unsigned flags = 0; // RwIm3DTransform flags (rwIM3D_VERTEXUV etc.)
		};
		Im3DBuild im3d;

		// ---- build-thread producer methods ----
		FramePass& currentBuildPass(); // the last camera pass, or a fresh default one

		// A clear the game issued before opening any camera pass this frame. RW's own order is
		// BeginUpdate then Clear, but the game also clears straight after a present, which lands
		// between frames on our side. Dropping it lost the only thing that painted the sky in the
		// Abyss of Time, since nothing else writes those pixels and the colour buffer is never
		// otherwise cleared. Applied to the next pass that opens.
		PassClear pendingClear;
		bool havePendingClear = false;
		// Append a draw to the frame's command list and extend the current pass's range. Passes are
		// built in order, so the range is just "everything appended since the pass opened".
		void submitMesh(MeshDraw&& m);
		void submitIm2D(Im2DDraw&& d);
		// Name the next `draws` Im2D draws, for interpolation. A game hook enables this immediately before
		// the guest function that draws the element, and each draw it submits takes the name. Arming
		// again replaces an unclaimed name, and the frame end drops it, so a caller that draws fewer
		// than it claimed leaks its name no further than the next enable.
		void armIm2DInterpTag(uint32_t tag, uint32_t draws = 1)
		{
			im2dTag = tag;
			im2dTagDraws = draws;
		}
		bool onScreenTarget(); // is the active target one that reaches the screen?
		void beginFrame(); // reset the build state for a new frame
		void endFrame(); // hand the built frame to the Renderer
		// Gather the world's lights for one object at its draw callback, filtered by target class
		// (rpLIGHTLIGHTATOMICS for atomics, rpLIGHTLIGHTWORLD for sectors). Ambient accumulates; colours
		// are read live so a mid-frame re-scope lands on the right draw. The render callbacks set
		// currentLights from this; enqueueSubMesh copies it into each submesh.
		FrameLights gatherLights(uint32_t targetFlag, uint32_t objAddr, bool isAtomic, bool hasNormals);
		void readRenderStateInto(ParsedGeometry& out); // blend/cull/depth/fog latches for a draw
		void enqueueSubMesh(const ParsedGeometry& pg, const SubMesh& sm); // push a mesh into the current pass
		// Where this draw's state lives in the frame's table, appending it unless the last entry already
		// matches. Comparing only the last entry is enough: draws arrive grouped by object, so this
		// collapses an object's submeshes onto one entry, and a miss costs a duplicate rather than a
		// wrong index. state_dedupe / light_dedupe turn the comparison off as a reference path.
		uint32_t stateIndexFor(const DrawState& s);
		uint32_t lightsIndexFor(const FrameLights& l);

		// The frame under construction, on loan from the ring for this frame only. Acquired lazily
		// because draws can arrive before the first showRaster opens a frame.
		Frame& buildFrame()
		{
			if (!build)
			{
				build = frames->acquireBuild();
				// Latched once per frame: the overlay flips the toggle from the render thread, and a
				// per-object read would leave the arenas half filled on the frame it changed.
				build->wantPoses = wantPoses;
				// The pool reclaims against the fence here, once per frame, with the index it stamps
				// references with. retiredIndex only advances on the render thread, so reading it once
				// is conservative: a slot freed a moment later just waits for the next frame.
				geometry->beginFrame(build->index, frames->retiredIndex());
			}
			return *build;
		}
		Frame* build = nullptr;
		// Mirrors Interpolator::enabled(), refreshed by the device once per frame so the build thread
		// never reads the render thread's flag mid-frame.
		bool wantPoses = false;
		uint32_t im2dTag = 0; // enabled by armIm2DInterpTag, consumed by the next submitIm2D
		uint32_t im2dTagDraws = 0; // draws the name still covers
		FrameLights buildLights; // holds the current camera's fog (gatherLights reads it)
		FrameLights currentLights; // lights of the object being rendered; set by the render callbacks
		uint32_t curWorld = 0; // RW's curWorld: the active camera's world, captured at begin-update

		// Debug-overlay light-colour holds, applied by slot in gatherLights. The game rewrites the light
		// colours every frame (DrawField), so a guest write wouldn't hold; gatherLights substitutes the
		// held colour at gather time. Slot 0 ambient, 1 directional, 2 directional2, scoped to one pass
		// or to every pass (pass -1).
		//
		// A fixed array of atomics, not a map: the overlay writes on the render thread while the EE
		// thread reads, and the map version returned a pointer into a node that a concurrent rehash or
		// clear() could destroy under the reader. Colour channels are independent atomics, so a
		// simultaneous edit can be read half-applied for one frame; that is a debug colour picker, not
		// a dangling pointer.
		static constexpr int LIGHT_LOCK_SLOTS = 4; // per pass: 3 used, 4 keeps the index arithmetic trivial
		static constexpr int LIGHT_LOCK_PASSES = 65; // pass -1 (all) plus 64 real ones
		struct LightLock
		{
			std::atomic<bool> held{false};
			std::atomic<float> r{0.0f}, g{0.0f}, b{0.0f};
		};
		static int lightLockKey(int pass, int slot) { return (pass + 1) * LIGHT_LOCK_SLOTS + slot; }
		std::optional<glm::vec3> lightLock(int pass, int slot) const; // by value: no node to dangle
		void setLightLock(int pass, int slot, const glm::vec3& c);
		void clearLightLock(int pass, int slot);
		void clearLightLocks();
		bool anyLightLockHeld() const;
		std::array<LightLock, LIGHT_LOCK_PASSES * LIGHT_LOCK_SLOTS> lightLocks;
		uint32_t buildDisplayRaster = 0; // the raster presented this frame (RwCameraShowRaster arg)

		// Render-state mirror: the device writes it as the game sets RwRenderState/RpSky state, the
		// parser reads it per draw.
		std::array<unsigned, 64> renderState{};
		std::array<uint64_t, 64> rsHits{};
		// RW SRCBLEND/DESTBLEND and the GS ALPHA register are kept separate; blendState() uses rwBlend as
		// the base and lets an explicit GS poke override the blend-mode fields on top (subset override).
		RwBlend rwBlend; // from RW SRCBLEND/DESTBLEND (factors only; equation is always ADD)
		GsBlend gsBlend; // from the raw GS ALPHA register (equation/factors/constAlpha/premul + reg)
		Blend blendState() const
		{
			Blend b;
			b.src = rwBlend.src;
			b.dst = rwBlend.dst;
			if (gsBlend.active) // explicit GS poke wins over the RW factors it overrides
			{
				b.equation = gsBlend.equation;
				b.src = gsBlend.src;
				b.dst = gsBlend.dst;
				b.constAlpha = gsBlend.constAlpha;
				b.alphaPremul = gsBlend.alphaPremul;
			}
			return b;
		}
		StencilState stencil; // current RW stencil state (updateStencilFromRwState)
		// GS alpha-test state (setGsAtest). Starts at the RW PS2 driver's standing TEST value, not at
		// "off": a game that never pokes ATEST_1 still draws through it. Sonic Heroes pokes ALPHA_1
		// and never ATEST_1, and 237 of its 382 GS batches read exactly this.
		GsAtest gsAtest = skyDefaultAtest();
		glm::bvec4 colorMask{true}; // GS FRAME write mask (mdlEffect mask passes); default RGBA
		GsClamp gsClamp; // GS CLAMP_1 region clamp (setGsClamp / restoreGsClamp)
		// rpSKYRENDERSTATEFARFOGPLANE: guest address of the RwReal where fog reaches full density. The
		// address is kept rather than the value, so a game that updates the global in place without
		// re-setting the state is still tracked. Overrides the camera far plane; P4 sets it, P3FES does not.
		uint32_t farFogPlaneAddr = 0;

		// Pass-build cursor: the camera pass currently being built.
		bool haveCam = false;
		uint32_t activeCamera = 0; // the camera of the pass being built (for the EndUpdate capture)
		uint32_t activeTarget = 0;
	};

	// Pure function of guest memory: it holds no state, so it stays free rather than becoming a member.
	// RwCamera addr -> view-projection. `parts`, when given, also receives the camera decomposed, with
	// valid=false on every path where the vp is a degenerate fallback rather than a real matrix.
	glm::mat4 buildCameraVP(uint32_t camAddr, CameraParts* parts = nullptr);
} // namespace sky2
