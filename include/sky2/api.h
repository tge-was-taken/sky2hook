// The sky2 boundary: everything in sky2::api is callable from the app and game side, everything
// else in sky2 is internal. Kept free of the sky2 and GL headers so a PCSX2 translation unit can
// include it (those pull in GS headers that clash with glad and GIFReg*).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <span>

namespace sky2::hooks
{
	struct Entry;
}

namespace sky2
{
	class Device;

	// The guest display space that 2D coordinates are expressed in (PS2 native for P3FES).
	constexpr float GAME_WIDTH = 640.0f;
	constexpr float GAME_HEIGHT = 448.0f;
} // namespace sky2

namespace sky2::api
{
	// The app owns the Device and registers it here before anything touches sky2. hooks::Handler is a
	// plain C function pointer that cannot carry a Device, so this unit holds it for the entry points.
	void setDevice(Device& device);

	// ---- device lifecycle ----------------------------------------------------------------------
	// Open / close the device (window + GL backend thread). open() returns false when the render
	// thread could not create its window or GL context.
	bool open();
	void close();
	bool closeRequested();
	// Number of captures fully written to disk. captureFrame() only posts a request to the render
	// thread, so a caller that needs the file present waits for this to advance.
	uint64_t shotsWritten();
	// True (once) if a draw sampled an unsampleable GS-resident render target since the last call.
	// The host polls this to auto-capture the frame for offline study (P3FES_RT_AUTOCAP).
	bool consumeRtCaptureRequest();

	// App-owned window: the SDL3 app calls this before open() to hand the renderer its window and GL
	// context (opaque SDL_Window* / SDL_GLContext). If unset, open() self-creates a GLX/X11 window
	// (the headless/replay fallback). onResize/requestClose let the app feed window lifecycle, since
	// in the app-owned path the render thread no longer reads window-system events itself.
	void setHostWindow(void* sdlWindow, void* glContext);
	void onResize(int width, int height);
	void requestClose();

	void setGameResolution(float w, float h);

	// ---- camera passes and render state --------------------------------------------------------
	// rwSTANDARD CAMERABEGINUPDATE: read the RwCamera and set view/projection.
	void cameraBeginUpdate(uint32_t cameraAddr);

	// rwSTANDARD CAMERAENDUPDATE: end the current camera pass.
	void cameraEndUpdate();

	// RwCameraClear(camera, RwRGBA* color, flags): capture the game's on-screen clear color so the
	// backbuffer border matches the PS2 (which clears to black behind smaller quads). `colorPtr` is
	// the guest RwRGBA; `flags` has rwCAMERACLEARIMAGE set when the color plane is cleared.
	void cameraClearColor(uint32_t cameraAddr, uint32_t colorPtr, int flags);

	// rwSTANDARD RASTERSHOWRASTER: `rasterAddr` is the on-screen raster being presented;
	// it marks that raster as a display target and presents the accumulated frame.
	void showRaster(uint32_t rasterAddr);

	// RwDevice fpRenderStateSet: state follows the RwRenderState enum.
	void setRenderState(unsigned state, unsigned value);

	// RpSkyRenderStateSet(substate, value): the game sets the GS ALPHA (blend) and TEST (alpha
	// test) registers directly through this, bypassing SRCBLEND/DESTBLEND.
	void setSkyRenderState(unsigned substate, unsigned value);

	// mdlEffect compositing: the game pokes the GS FRAME and CLAMP_1 registers directly to run a
	// two-pass masked screen-distortion effect, bypassing RpSkyRenderStateSet. The FRAME write mask
	// becomes a GL color-write mask; CLAMP_1 becomes a UV clamp in the 2D shader.
	void setFrameMask(uint32_t fbmsk);
	void setGsClamp(uint64_t clamp1);
	void restoreGsClamp();

	// ---- rasters and the movie frame -----------------------------------------------------------
	// rwSTANDARD RASTERCREATE: register a newly created RwRaster (width/height/depth/type/format),
	// so sky2 has the full picture of render targets (display, camera-texture, FMV, textures).
	void rasterCreate(uint32_t rasterAddr);

	// rwSTANDARD RASTERUNLOCK: the raster's pixels just changed; drop its cached decode so the
	// next use re-decodes (fixes stale textures across scenes + refreshes dynamic/FMV rasters).
	void rasterUnlock(uint32_t rasterAddr);

	// SkyRasterDestroy: the raster is being freed; evict every cache keyed by it (decode signature,
	// GL texture, render-target FBO, registry) so memory does not grow across scene transitions and
	// a later raster reusing the address does not read a stale decode.
	void rasterUnlockPalette(uint32_t rasterAddr); // CLUT rewritten; the pixels may be untouched
	void rasterDestroy(uint32_t rasterAddr);

	// sceMpegGetPicture output buffer (EE RAM) for the current movie frame. Recorded so the
	// GS-resident FMV texture (cpPixels=0) can be sourced from the decoded frame.
	void setMovieFrame(uint32_t bufAddr);

	// Record a raster the game reported as the draw buffer (RpSkyGetDrawBufferRaster). A later draw that
	// binds it as a texture is sampling the framebuffer; sky2 feeds it the display FBO snapshot instead.
	// role: 0 unknown, 1 the displayed buffer, 2 the buffer being drawn into (ObjectManager::BufferRole,
	// as an int so the hook layer stays free of renderer types).
	void registerFrameBufferRaster(uint32_t rasterAddr, int role, const char* source);
	bool isFrameBufferRaster(uint32_t rasterAddr); // is this one of the GS frame buffers?

	// P3FES_TRACE_DRAWBUF: log the EE call chain at an access to a frame buffer, once per distinct stack.
	void traceEeChain(const char* tag, uint32_t id, uint32_t ra, uint32_t sp);

	// ---- RW object lifecycle tracker (folded rw_tracker) ---------------------------------------
	void objectTrackerInit(); // enable unless P3FES_NO_RWTRACK; load symbols
	bool objectTrackerEnabled();
	std::span<const hooks::Entry> objectTrackerTable(); // RW create/destroy hook entries
	void objectTrackerReport(const char* reason, std::FILE* out = stderr);
	void objectTrackerDescribe(uint32_t addr, std::FILE* out = stderr);

	// ---- render pipelines ----------------------------------------------------------------------
	// Object-pipeline render node: read the RpAtomic and render its geometry
	// (per-material submeshes, decoded textures, lighting).
	void renderAtomic(uint32_t atomicAddr);
	void renderWorldSector(uint32_t sectorAddr);

	// Im3D pipeline (A2): projected 3D primitives (e.g. the field character-shadow quad).
	// Transform stashes the object-space vertex array + the object->world matrix; RenderPrimitive
	// expands it to triangles at gCamVP * ltm (unlit, depth-tested, current blend); End clears it.
	void im3dTransform(uint32_t vertsAddr, int numVerts, uint32_t ltmAddr, unsigned flags);
	void im3dRenderPrimitive(uint32_t primType);
	void im3dRenderIndexedPrimitive(uint32_t primType, uint32_t indicesAddr, int numIndices);
	// RwIm3DRenderLine(v1, v2): two indices into the stashed vertices, no primitive type.
	void im3dRenderLine(int vert1, int vert2);
	void im3dEnd();

	// Im2D pipeline (2D immediate primitives: UI text, sprites, fades).
	void im2dRenderPrimitive(uint32_t primType, uint32_t vertsAddr, int numVerts);
	void im2dRenderTriangle(uint32_t vertsAddr, int numVerts, int vert1, int vert2, int vert3);
	void im2dRenderIndexedPrimitive(uint32_t primType, uint32_t vertsAddr, int numVerts,
		uint32_t indicesAddr, int numIndices);
	void im2dRenderLine(uint32_t vertsAddr, int numVerts, int vert1, int vert2);

	// Name the next `draws` 2D draws so the interpolator can recognise them next tick. Armed from a game
	// hook on the function that draws the element; anything left unclaimed dies at the end of the frame.
	void armIm2DInterpTag(uint32_t tag, uint32_t draws);

	// Rt2d pipeline (2D vector graphics)
	void rt2dPathFill(uint32_t pathAddr, uint32_t brushAddr);
	void rt2dPathStroke(uint32_t pathAddr, uint32_t brushAddr);
	void rt2dFontShow(uint32_t fontAddr, uint32_t stringAddr, float height, uint32_t anchorAddr,
		uint32_t brushAddr);
	void rt2dDumpGuestCtm();

	// ---- frame pacing and debug ----------------------------------------------------------------
	void toggleFrameLimiter(); // Tab: pace presents to fps_cap, or run as fast as the render allows
	void setFrameLimiter(bool on);
	bool frameLimiterEnabled(); // the app mirrors this onto the emulator's own speed limiter
	// onStart runs on the build thread when the captured frame opens, so a caller can mark the guest
	// call stream at exactly that point.
	void captureFrame(const char* path, void (*onStart)() = nullptr);
	void requestShaderReload(); // hot-reload the 3D shaders from disk on the render thread (F5)
	void setDebugChannel(int ch); // 3D debug view: 0 = normal, 1..9 isolate a shading component (number keys)

	// ---- imgui debug overlay -------------------------------------------------------------------
	// Host-owned rows in the overlay's enhancement section. The library draws them but owns none of
	// the state: `value` is read and written on the render thread, so it is an atomic the host polls
	// from its own thread. A row that is not armed shows `note` instead of its slider, which is how a
	// tweak whose hooks were never registered says so rather than moving a value nothing reads.
	struct OverlaySlider
	{
		const char* label;
		std::atomic<float>* value;
		float min;
		float max;
		bool armed = true;
		const char* note = nullptr;
	};
	// The span must outlive the device (point it at a static array).
	void setOverlaySliders(std::span<const OverlaySlider> rows);
	std::span<const OverlaySlider> overlaySliders();


	void imguiPushEvent(const void* sdlEvent); // app thread: forward an SDL_Event*
	void imguiToggle(); // show/hide the debug window
	bool imguiWantsInput(); // app thread: true when the UI is capturing mouse/keyboard
} // namespace sky2::api
