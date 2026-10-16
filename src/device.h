// sky2 Device: the composition root. Owns the four sky2 subsystems as members so they form one
// object graph with a single lifetime, instead of four independent singletons. The RwDevice entry
// points (render state, camera passes, the Im2D funnel) are Device methods. The app owns the one
// instance; sky2/api.h holds the pointer the C hook ABI needs, and everything else is injected.
#pragma once

#include "object_manager.h"
#include "render_queue.h"
#include "renderer.h"
#include "shader_manager.h"
#include "texture_manager.h"

#include "pipelines/atomic/atomic.h"
#include "pipelines/atomic/ptank.h"
#include "pipelines/im2d/im2d.h"
#include "pipelines/im3d/im3d.h"
#include "pipelines/rt2d/rt2d.h"
#include "pipelines/sector/sector.h"

namespace sky2
{
	class Device
	{
	public:
		Device(); // wires the inter-subsystem references (the ObjectManager<->TextureManager cycle)

		// ---- RwDevice entry points (sky2/api.h calls these on the registered instance) ----
		bool open(); // false = the render thread failed to bring up its window/GL context
		void close();
		bool closeRequested();
		uint64_t shotsWritten(); // captures completed; capture is async, so callers poll this
		bool consumeRtCaptureRequest();
		void setGameResolution(float w, float h);
		void setHostWindow(void* sdlWindow, void* glContext);
		void onResize(int width, int height);
		void requestClose();
		void cameraBeginUpdate(uint32_t cameraAddr);
		void cameraEndUpdate();
		void cameraClearColor(uint32_t cameraAddr, uint32_t colorPtr, int flags);
		void showRaster(uint32_t rasterAddr);
		void setRenderState(unsigned state, unsigned value);
		void setSkyRenderState(unsigned substate, unsigned value);
		void setFrameMask(uint32_t fbmsk); // GS FRAME write mask -> glColorMask
		void setGsClamp(uint64_t clamp1); // GS CLAMP_1 poke
		void setFarFogPlane(uint32_t valueAddr); // rpSKYRENDERSTATEFARFOGPLANE (pointer to RwReal)
		void restoreGsClamp();
		void captureFrame(const char* path, void (*onStart)() = nullptr);
		unsigned im2dFilterAddressing(); // packed filter|U<<8|V<<12 from the current render state

		// Declaration order is construction order; keep dependencies before their dependents once the
		// inter-subsystem references are injected (objects -> textures -> renderer -> queue).
		ObjectManager objects;
		TextureManager textures;
		FrameRing frames; // pooled frame storage, shared by the build and render threads
		GeometryPool geometry; // vertex/index buffers behind GeomId, shared the same way
		Renderer renderer;
		RenderQueue queue;
		ShaderManager shaders;

		// The RenderWare geometry pipelines. Each holds a Device* wired in the constructor body, so
		// their position here does not matter.
		AtomicPipeline atomic;
		PTankPipeline ptank;
		SectorPipeline sector;
		Im3DPipeline im3d;
		Im2DPipeline im2d;
		Rt2DPipeline rt2d;

	private:
		void setInitialRenderStateDefaults();
		void captureCameraState(FramePass& pass, uint32_t cameraAddr);
		void updateBlendFromRwState();
		void updateStencilFromRwState();
		void setGsBlend(unsigned v);
		void setGsAtest(unsigned v);
		void logRenderStateUsage();
	};
} // namespace sky2
