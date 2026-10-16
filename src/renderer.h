// sky2 renderer: the render-thread consumer. Owns the GL state, the render thread + window/present,
// and the render passes. The frame exchange itself lives in FrameRing. One instance, owned by the
// Device. RenderQueue::endFrame() calls submit().
#pragma once

#include "common/frame.h" // Frame, FrameRing, FramePass, MeshDraw, GL types
#include "pipelines/im2d/im2d.h" // Im2DRenderer: the Im2D render half GlState owns
#include "overlay.h" // ImguiOverlay: the render-thread debug UI the Renderer owns
#include "enhancements/interpolator.h" // Interpolator: draws between guest ticks

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Opaque X11/GLX handle types, forward-declared so this widely included header does not pull in
// Xlib.h and its global macros (None, Status, Success).
struct _XDisplay;
struct __GLXcontextRec;

namespace sky2
{
	class TextureManager;
	class ObjectManager;
	class ShaderManager;

	struct RenderTarget
	{
		GLuint fbo = 0, colorTex = 0, depthRbo = 0, flipFbo = 0, flipTex = 0;
		// Multisampled attachments, when the pass is drawn with more than one sample. fbo then renders
		// into these and resolves into colorTex, which is otherwise the render target itself.
		GLuint msFbo = 0, msColor = 0, msDepth = 0;
		int w = 0, h = 0, samples = 1;
	};

	// A copy of the display kept for framebuffer-feedback effects to sample.
	struct CaptureTarget
	{
		GLuint fbo = 0, tex = 0;
		int w = 0, h = 0;
	};

	// Where a capture's content sits: the texture is the GS buffer size (times feedback_scale) but only
	// the visible area holds pixels, the rest is black pad.
	struct CaptureGeometry
	{
		int capW = 0, capH = 0;
		float capVisW = 0.0f, capVisH = 0.0f;
	};

	// All render-thread GL state in one struct so it is created at init and freed/cleared in one place
	// (GlState::destroy). Usages are gl.<member>.
	// A geometry kept resident on the GPU: its VAO (Vertex3D attrib layout) + vertex buffer, uploaded
	// once and reused across frames/passes. Indexed by pool slot, with `id` recording which generation
	// of that slot was uploaded, so a recycled slot re-uploads instead of drawing the previous tenant.
	// `id` being stale means the contents are, not that the buffer is useless: the slot it names comes
	// back with a new generation next frame, wanting a buffer of much the same size. So a stale id only
	// forces a re-upload; the GL objects go on lastUsedFrame, which is the frame the entry was last
	// bound in.
	struct GpuGeom
	{
		GLuint vao = 0, vbo = 0;
		GeomId id = GEOM_ID_NONE;
		uint64_t lastUsedFrame = 0;
	};
	// A resident index buffer (one per submesh), indexed by pool slot the same way.
	struct GpuIndexBuf
	{
		GLuint ebo = 0;
		GeomId id = GEOM_ID_NONE;
		uint64_t lastUsedFrame = 0;
	};

	struct GlState
	{
		// The Im2D render half. Render thread only; the build half runs on the EE thread and shares
		// nothing with this.
		Im2DRenderer im2d;
		// 3D geometry kept GPU-resident, keyed by the shared vertex/index buffer identity: uploaded once,
		// reused every frame, evicted when no live frame references the buffer (Renderer::evictUnusedGeom).
		std::vector<GpuGeom> gpuGeom; // indexed by pool slot
		std::vector<GpuIndexBuf> gpuIdx;
		// The frame's whole bone arena (Frame::bones), uploaded once per frame. boneSSBOBones is the
		// matrix count the current allocation holds, so a steady scene stops reallocating.
		GLuint boneSSBO = 0;
		size_t boneSSBOBones = 0;
		uint64_t frameIndex = 0; // the frame being rendered, for ageing the two caches above
		// The frame's transient geometry (Im3D): one VAO over one vertex and one index buffer, uploaded
		// once per frame. A mesh draws its slice with a base vertex instead of owning GPU buffers.
		GLuint transientVao = 0, transientVbo = 0, transientEbo = 0;
		// Display FBO (window-sized, blitted to the default framebuffer at present) + off-screen targets.
		GLuint displayFbo = 0, displayColor = 0, displayDepth = 0;
		// Multisampled display storage, when the msaa enhancement is on. Passes render into this and it
		// resolves into displayFbo, so every reader of the display keeps reading one place.
		GLuint displayMsFbo = 0, displayMsColor = 0, displayMsDepth = 0;
		int displayW = 0, displayH = 0, displaySamples = 1;
		// Feedback copies of the display: `capture` is the frame so far (for a draw binding the buffer
		// being drawn into), `prevFrame` the frame already presented (for one binding the displayed buffer).
		CaptureTarget capture, prevFrame;
		std::unordered_map<uint32_t, RenderTarget> targets; // off-screen RTs keyed by raster addr

		// Free every GL object above and clear the caches. GlState has no pointer back to its Renderer,
		// so the two caches it also clears come in as parameters.
		void destroy(ShaderManager& shaders, TextureManager& textures);
	};

	// The self-created GLX/X11 window (the headless/replay fallback; unused in the app-owned path).
	// win/wmDelete are X11 Window/Atom, both XID (unsigned long).
	struct WindowState
	{
		_XDisplay* dpy = nullptr;
		unsigned long win = 0;
		__GLXcontextRec* ctx = nullptr;
		unsigned long wmDelete = 0;
		bool hasDebug = false; // GL_KHR_debug available: debug output installed, push/pop groups safe
	};

	class Renderer
	{
	public:
		TextureManager* textures = nullptr; // injected by Device (decode/upload cache)
		ObjectManager* objects = nullptr; // injected by Device (RT classification, framebuffer size)
		ShaderManager* shaders = nullptr; // injected by Device (program cache; prog3d variants)

		FrameRing* frames = nullptr; // injected by Device (the pooled frame storage)
		GeometryPool* geometry = nullptr; // injected by Device (vertex/index buffers behind GeomId)

		// Called on the EE build thread at end-of-frame: hand the finished frame to the render thread.
		void submit(Frame* frame);

		// ---- render-thread passes that touch the injected caches (accessor-free) ----
		void evictDestroyedRasters();
		void uploadPendingTextures();
		void snapshotDisplayForFeedback(float u0 = 0.0f, float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f);
		void capturePreviousFrame(); // at present: keep this frame for next frame's display-buffer taps
		void finishOffscreenTarget(uint32_t rasterAddr);
		void logRtFboMiss(uint32_t texKey);
		// P3FES_DRAW_TRACE=x0,y0,x1,y1: after every draw, the mean colour of that rect. One run in
		// place of a P3FES_DRAW_UPTO bisection, which costs an emulator boot per step.
		void traceDrawRegion(int drawIndex);
		bool traceRegionArmed() const { return traceRect[2] > traceRect[0]; }
		int traceRect[4] = {0, 0, 0, 0};
		std::FILE* traceRegionOut = nullptr;
		void drawOneMesh(const Frame& frame, const MeshDraw& m, const glm::mat4& vp, uint32_t passIndex);
		// `order` is this pass's slice of Frame::drawOrder, already partitioned opaque-then-blended.
		void drawMeshes(const Frame& frame, std::span<const uint32_t> order, const FrameLights& lights,
			const glm::mat4& vp, uint32_t passIndex);
		void bindMeshBuffers(const MeshDraw& m); // bind the mesh's resident VAO+VBO+EBO, uploading on first use
		void uploadTransientGeometry(const Frame& frame); // the frame's Im3D arena, one upload each
		void evictUnusedGeom(); // free cached GPU geometry no live frame still references (end of frame)

		// ---- render-thread window + present (window.cpp) ----
		// The render thread's entry point. The Device outlives the thread (App::run joins it), so
		// `this` cannot dangle.
		void renderLoop();
		bool createWindow(); // adopts the app's window when one was handed over, else creates a GLX one
		bool adoptHostWindow();
		void renderFrame(const Frame& frame);
		// Centered viewport preserving the game's aspect ratio inside the window.
		struct Viewport
		{
			int x, y, w, h;
		};
		Viewport letterboxViewport(int winW, int winH) const;

		// ---- render-thread init + FBO/pass management (operate on gl) ----
		void initGL();
		void init3D();
		void displayFboSize(int& w, int& h);
		// Bring displayFbo up to date with the multisampled passes, for anything that reads the display
		// (the feedback taps, the present blit). A no-op when multisampling is off.
		void resolveDisplay();
		// True when the render is larger than the resolution the art was authored for, which is the only
		// case any upscale compensation applies to.
		bool magnifying() const { return gl.displayW > int(gameResW.load()) || gl.displayH > int(gameResH.load()); }
		void ensureDisplayFbo(int w, int h);
		void followGameResolution(); // adopt the disc's presented resolution when no size was asked for
		void bindDisplayFbo();
		void bindOffscreenTarget(uint32_t rasterAddr, int w, int h);
		void blitDisplayToWindow(int winW, int winH);
		void uploadBones(const std::vector<glm::mat4>& bones);

		GlState gl;
		ImguiOverlay overlay; // debug UI; render thread only

		// Frame exchange lives in the FrameRing; the render thread pulls with takePending().

		// Render thread control.
		std::thread thread;
		std::atomic<bool> running{false};
		std::atomic<bool> closeRequested{false};

		// App-owned window adoption (void* to keep SDL types out of the header); read-only on the render
		// thread once set before open().
		void* hostSdlWindow = nullptr;
		void* hostGlContext = nullptr;
		bool hasHostWindow = false;

		// The window sky2 creates itself when no app window was handed over (headless/replay path).
		// Render thread only.
		WindowState window;

		// Window + guest-resolution dimensions, atomic (written by the render thread, read cross-thread).
		std::atomic<int> winW{640};
		std::atomic<int> winH{448};
		std::atomic<float> gameResW{640};
		std::atomic<float> gameResH{448};

		// Screenshot request (C key / capture) handed to the render thread. reqShotIndex is the pinned
		// frame the capture must land on, so the captured game frame does not move with render speed.
		std::mutex shotMutex;
		std::string reqShotPath;
		uint64_t reqShotIndex = 0;
		// Bumped by the render thread after a requested capture is fully written. Capture is
		// asynchronous, so a caller that needs the file on disk waits for this to advance.
		std::atomic<uint64_t> shotsWritten{0};

		// Render-thread startup result, so a caller can tell "still initializing" from "failed".
		std::atomic<bool> ready{false};
		std::atomic<bool> initFailed{false};

		// Shader hot-reload request (F5): the render thread rebuilds programs from disk before drawing.
		std::atomic<bool> reqShaderReload{false};

		// Draw only frames the build thread published, instead of re-presenting the last one. A capture
		// run needs it: a re-present runs the feedback tap again, so the result would follow render
		// speed rather than the game.
		bool renderOnlyNewFrames = false;
		// Render thread: is the pass being drawn the on-screen display FBO?
		bool passIsDisplay = true;
		// Current pass's color attachment + sample count, for the GS DATE test in drawOneMesh.
		GLuint passColorTex = 0;
		int passSamples = 1;

		// Present-rate cap. The rate comes from config (fps_cap, 0 meaning "start uncapped"); Tab
		// toggles the flag from the app thread while the render loop reads it each frame.
		std::atomic<bool> frameLimiter{true};
		// Present on the display's cadence. Applied on the render thread when it changes, since only
		// that thread holds the GL context.
		std::atomic<bool> vsync{false};
		int frameLimitRate = 60;
		Interpolator interp; // render thread: draws between guest ticks when enabled

		// Live render-thread stats for the debug overlay. Written by the render loop and read by the
		// overlay, both on the render thread.
		struct Stats
		{
			float fps = 0.0f; // presented frames per second
			float frameMs = 0.0f; // wall time between presents, limiter sleep included
			float renderMs = 0.0f; // render + present work alone: 1000/renderMs is the uncapped ceiling
			int passes = 0, meshes = 0, im2dBatches = 0; // what the last frame contained
		};
		Stats stats;

		// What each pass of the last frame contained, so the overlay can inspect a specific pass
		// instead of whatever state the newest draw happened to leave behind. Render thread only.
		struct PassInfo
		{
			uint32_t target = 0, camera = 0;
			int w = 0, h = 0;
			bool isDisplay = false;
			int meshes = 0, im2dBatches = 0;
			bool clearColor = false, clearDepth = false;
			FrameLights lights;
		};
		std::vector<PassInfo> passes;
		// Per-pass on/off, by position in the frame (the overlay's solo button just clears the others).
		// Sized with `passes`; a pass switched off is skipped entirely, so you can see what it drew.
		std::vector<uint8_t> passDisabled;

		// A pass's rendered colour, for the overlay to display. Off-screen targets expose the published
		// copy (already flipped into the orientation the game samples); the display FBO is GL y-up, so
		// the caller flips V when told to. tex 0 = nothing to show yet.
		struct PassPreview
		{
			GLuint tex = 0;
			int w = 0, h = 0;
			bool flipV = false;
		};
		PassPreview passPreview(const PassInfo& p) const;
		std::string glRenderer; // GL_RENDERER, so the overlay shows software vs hardware at a glance

		// Bytes held by each kind of render-thread GL resource, for the overlay.
		struct MemoryUse
		{
			size_t textureBytes = 0, geometryBytes = 0, indexBytes = 0, targetBytes = 0, screenBytes = 0;
			size_t textureCount = 0, geometryCount = 0, indexCount = 0, targetCount = 0;
			size_t total() const { return textureBytes + geometryBytes + indexBytes + targetBytes + screenBytes; }
		};
		MemoryUse memoryUse() const;

		// ---- capture/trace, off the render path (trace.cpp) ----
		void debugWriteManifest(const Frame& frame, const char* path) const;
		void writePPM(const char* path) const;
		// Every pass of a captured frame, as RGBA. Empty base = not capturing.
		void dumpPassImage(const std::string& base, int pass, int w, int h) const;
		std::string passDumpBase; // render thread only; set for the frame a capture is taken on

	private:
		// get-or-create an off-screen RT's FBO, rebuilt when its size or sample count changes
		RenderTarget& acquireTarget(uint32_t raster, int w, int h, int samples = 1);
		bool captureGeometry(CaptureGeometry& g) const; // false before the display size is known
		void ensureCaptureTarget(CaptureTarget& t, const CaptureGeometry& g, TextureId texId, const char* label);
		// Copy a UV window of the display into a capture target. Leaves the display FBO bound.
		void blitDisplayRegion(const CaptureTarget& dst, const CaptureGeometry& g, float u0, float v0,
			float u1, float v1);
	};
} // namespace sky2
