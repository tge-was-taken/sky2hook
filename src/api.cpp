// The sky2 boundary: everything in sky2::api is callable from the app and game side, everything
// else in sky2 is internal. hooks::Handler is a plain C function pointer that cannot carry a Device,
// so the app registers the Device it owns here, the only place in sky2 that holds a pointer to it.
#include "sky2/api.h"

#include "common.h"
#include "sky2/hooks.h" // hooks::Context / hooks::Entry (the RW-lifecycle tracker hook table)
#include "device.h"
#include "overlay.h"
#include "sky2/addresses.h" // RW_*_CREATE / _DESTROY hook addresses

#include <cstdio>
#include <vector>

namespace sky2::api
{
	static Device* s_device = nullptr;

	void setDevice(Device& device)
	{
		s_device = &device;
		// The overlay is render-thread state that belongs in Renderer as a member; until it lives
		// there it is injected here instead.
	}

	namespace
	{
		// The one place the registration is checked: a null here means an entry point ran before
		// app::App registered the Device, which is a startup-order bug rather than a runtime state.
		Device& device()
		{
			SKY2_ASSERT(s_device != nullptr, "sky2 entry point used before app::App registered the Device");
			return *s_device;
		}

		// The RW lifecycle tracker's handlers. Create handlers are post-hooks (c.v0 = the new object,
		// c.ra/c.sp = the creating site and frame); destroy handlers are pre-hooks (c.a0 = the object
		// being destroyed). hooks::Handler is a plain function pointer, so each one is a trampoline
		// into the Device's ObjectManager.
		template <ObjectManager::RwType T>
		void trackCreate(hooks::Context& c)
		{
			device().objects.trackCreate(T, c.v0, c.ra, c.sp);
		}

		template <ObjectManager::RwType T>
		hooks::Result trackDestroy(hooks::Context& c)
		{
			device().objects.trackDestroy(T, c.a0);
			return hooks::Result::Continue;
		}

		void lightCreate(hooks::Context& c) { device().objects.trackLightCreate(c.v0, c.ra, c.sp); }

		hooks::Result geometryDestroy(hooks::Context& c)
		{
			ObjectManager& objects = device().objects;
			objects.trackDestroy(ObjectManager::Geometry, c.a0);
			objects.invalidateGeom(c.a0);
			return hooks::Result::Continue;
		}

		// An area load frees the world, so every sector we cached for it is gone. RpWorldUnlock is the
		// in-place-edit analogue, same treatment.
		hooks::Result worldDropSectors(hooks::Context& c)
		{
			device().objects.invalidateWorldSectors(c.a0);
			return hooks::Result::Continue;
		}

		// RpGeometryUnlock: the geometry's verts/topology may have changed. Only the next re-parse can
		// say whether they did, so mark the cached parse rather than dropping it.
		hooks::Result geometryUnlock(hooks::Context& c)
		{
			device().objects.markGeomDirty(c.a0);
			return hooks::Result::Continue;
		}
	} // namespace

	DeviceHandle::DeviceHandle()
		: device(new Device)
	{
		setDevice(*device);
	}

	DeviceHandle::~DeviceHandle() { delete device; }

	// ---- device: window/GL lifecycle, camera passes, render state ----
	bool open() { return device().open(); }
	void close() { device().close(); }
	bool closeRequested() { return device().closeRequested(); }
	uint64_t shotsWritten() { return device().shotsWritten(); }
	bool consumeRtCaptureRequest() { return device().consumeRtCaptureRequest(); }
	void setGameResolution(float w, float h) { device().setGameResolution(w, h); }

	namespace
	{
		std::span<const OverlaySlider> s_overlaySliders;
	}

	void setOverlaySliders(std::span<const OverlaySlider> rows) { s_overlaySliders = rows; }
	std::span<const OverlaySlider> overlaySliders() { return s_overlaySliders; }
	void setHostWindow(void* sdlWindow, void* glContext) { device().setHostWindow(sdlWindow, glContext); }
	void onResize(int width, int height) { device().onResize(width, height); }
	void requestClose() { device().requestClose(); }
	void cameraBeginUpdate(uint32_t cameraAddr) { device().cameraBeginUpdate(cameraAddr); }
	void cameraEndUpdate() { device().cameraEndUpdate(); }
	void cameraClearColor(uint32_t cameraAddr, uint32_t colorPtr, int flags)
	{
		device().cameraClearColor(cameraAddr, colorPtr, flags);
	}
	void showRaster(uint32_t rasterAddr) { device().showRaster(rasterAddr); }
	void setRenderState(unsigned state, unsigned value) { device().setRenderState(state, value); }
	void setSkyRenderState(unsigned substate, unsigned value) { device().setSkyRenderState(substate, value); }
	void setFrameMask(uint32_t fbmsk) { device().setFrameMask(fbmsk); }
	void setGsClamp(uint64_t clamp1) { device().setGsClamp(clamp1); }
	void restoreGsClamp() { device().restoreGsClamp(); }
	void captureFrame(const char* path, void (*onStart)()) { device().captureFrame(path, onStart); }

	// ---- objects: rasters, the movie frame, the RW lifecycle tracker ----
	void rasterCreate(uint32_t rasterAddr) { device().objects.rasterCreate(rasterAddr); }
	void rasterUnlock(uint32_t rasterAddr) { device().objects.rasterUnlock(rasterAddr); }
	void rasterUnlockPalette(uint32_t rasterAddr) { device().objects.rasterUnlock(rasterAddr); }
	void rasterDestroy(uint32_t rasterAddr) { device().objects.rasterDestroy(rasterAddr); }
	void setMovieFrame(uint32_t bufAddr) { device().objects.setMovieFrame(bufAddr); }
	bool isFrameBufferRaster(uint32_t rasterAddr)
	{
		return device().objects.frameBufferRasters.count(rasterAddr) != 0;
	}

	void registerFrameBufferRaster(uint32_t rasterAddr, int role, const char* source)
	{
		device().objects.registerFrameBufferRaster(
			rasterAddr, static_cast<ObjectManager::BufferRole>(role), source);
	}
	void traceEeChain(const char* tag, uint32_t id, uint32_t ra, uint32_t sp)
	{
		device().objects.traceEeChain(tag, id, ra, sp);
	}
	void objectTrackerInit() { device().objects.trackerInit(); }
	bool objectTrackerEnabled() { return device().objects.trackerEnabled(); }

	std::span<const hooks::Entry> objectTrackerTable()
	{
		using OM = ObjectManager;
		// Built on first call, after game::loadAddresses (addresses are zero before that).
		static const std::vector<hooks::Entry> t = {
			{sky2::addresses().RwFrameCreate, nullptr, false, &trackCreate<OM::Frame>},
			{sky2::addresses().RwFrameDestroy, &trackDestroy<OM::Frame>, false},
			{sky2::addresses().RpAtomicCreate, nullptr, false, &trackCreate<OM::Atomic>},
			{sky2::addresses().RpAtomicDestroy, &trackDestroy<OM::Atomic>, false},
			{sky2::addresses().RpGeometryDestroy, &geometryDestroy, false},
			{sky2::addresses().RpGeometryUnlock, &geometryUnlock, false},
			{sky2::addresses().RpWorldDestroy, &worldDropSectors, false},
			{sky2::addresses().RpWorldUnlock, &worldDropSectors, false},
			{sky2::addresses().RwTextureCreate, nullptr, false, &trackCreate<OM::Texture>},
			{sky2::addresses().RwTextureDestroy, &trackDestroy<OM::Texture>, false},
			{sky2::addresses().RwRasterCreate, nullptr, false, &trackCreate<OM::Raster>},
			{sky2::addresses().RwRasterDestroy, &trackDestroy<OM::Raster>, false},
			{sky2::addresses().RpMaterialCreate, nullptr, false, &trackCreate<OM::Material>},
			{sky2::addresses().RpMaterialDestroy, &trackDestroy<OM::Material>, false},
			{sky2::addresses().RpClumpCreate, nullptr, false, &trackCreate<OM::Clump>},
			{sky2::addresses().RpClumpDestroy, &trackDestroy<OM::Clump>, false},
			{sky2::addresses().RpLightCreate, nullptr, false, &lightCreate},
			{sky2::addresses().RpLightDestroy, &trackDestroy<OM::Light>, false},
			{sky2::addresses().RwCameraCreate, nullptr, false, &trackCreate<OM::Camera>},
			{sky2::addresses().RwCameraDestroy, &trackDestroy<OM::Camera>, false},
		};
		return t;
	}
	void objectTrackerReport(const char* reason, std::FILE* out) { device().objects.report(reason, out); }
	void objectTrackerDescribe(uint32_t addr, std::FILE* out) { device().objects.describe(addr, out); }

	// ---- renderer + shaders ----
	void setFrameLimiter(bool on)
	{
		Renderer& r = device().renderer;
		if (r.frameLimiter.exchange(on) != on)
			std::fprintf(stderr, "[sky2] frame limiter %s (%d fps)\n", on ? "on" : "off", r.frameLimitRate);
	}
	void toggleFrameLimiter() { setFrameLimiter(!device().renderer.frameLimiter.load()); }
	bool frameLimiterEnabled() { return device().renderer.frameLimiter.load(); }
	void requestShaderReload() { device().renderer.reqShaderReload.store(true); }
	void setDebugChannel(int ch) { device().shaders.setDebugChannel(ch); }

	// ---- pipelines ----
	void renderAtomic(uint32_t atomicAddr) { device().atomic.render(atomicAddr); }
	void renderWorldSector(uint32_t sectorAddr) { device().sector.render(sectorAddr); }

	void im3dTransform(uint32_t vertsAddr, int numVerts, uint32_t ltmAddr, unsigned flags)
	{
		device().im3d.transform(vertsAddr, numVerts, ltmAddr, flags);
	}
	void im3dRenderPrimitive(uint32_t primType) { device().im3d.renderPrimitive(primType); }
	void im3dRenderLine(int vert1, int vert2)
	{
		device().im3d.renderLine(vert1, vert2);
	}
	void im3dRenderIndexedPrimitive(uint32_t primType, uint32_t indicesAddr, int numIndices)
	{
		device().im3d.renderIndexedPrimitive(primType, indicesAddr, numIndices);
	}
	void im3dEnd() { device().im3d.end(); }

	void im2dRenderPrimitive(uint32_t primType, uint32_t vertsAddr, int numVerts)
	{
		device().im2d.renderPrimitive(primType, vertsAddr, numVerts);
	}
	void im2dRenderTriangle(uint32_t vertsAddr, int numVerts, int vert1, int vert2, int vert3)
	{
		device().im2d.renderTriangle(vertsAddr, numVerts, vert1, vert2, vert3);
	}
	void im2dRenderIndexedPrimitive(uint32_t primType, uint32_t vertsAddr, int numVerts,
		uint32_t indicesAddr, int numIndices)
	{
		device().im2d.renderIndexedPrimitive(primType, vertsAddr, numVerts, indicesAddr, numIndices);
	}
	void im2dRenderLine(uint32_t vertsAddr, int numVerts, int vert1, int vert2)
	{
		device().im2d.renderLine(vertsAddr, numVerts, vert1, vert2);
	}

	void armIm2DInterpTag(uint32_t tag, uint32_t draws)
	{
		device().queue.armIm2DInterpTag(tag, draws);
	}

	void rt2dPathFill(uint32_t pathAddr, uint32_t brushAddr)
	{
		device().rt2d.pathFill(pathAddr, brushAddr);
	}

	void rt2dPathStroke(uint32_t pathAddr, uint32_t brushAddr)
	{
		device().rt2d.pathStroke(pathAddr, brushAddr);
	}

	void rt2dFontShow(uint32_t fontAddr, uint32_t stringAddr, float height, uint32_t anchorAddr,
		uint32_t brushAddr)
	{
		device().rt2d.fontShow(fontAddr, stringAddr, height, anchorAddr, brushAddr);
	}

	void rt2dDumpGuestCtm()
	{
		device().rt2d.dumpGuestCtm();
	}

	// ---- imgui overlay ----
	void imguiPushEvent(const void* sdlEvent) { device().renderer.overlay.pushEvent(sdlEvent); }
	void imguiToggle() { device().renderer.overlay.toggle(); }
	bool imguiWantsInput() { return device().renderer.overlay.wantsInput(); }
} // namespace sky2::api
