// sky2 window + render thread: GLX/X11 window creation and the present loop that pulls
// each queued frame and drives the render passes.
#include "common.h"
#include "object_manager.h"
#include "profile.h"
#include "render_queue.h"
#include "overlay.h" // debug overlay
#include "renderer.h"
#include "shader_manager.h" // reloadShaders() on the F5 hot-reload request
#include "rw_to_gl.h"
#include "texture_manager.h"

#include "enhancements/msaa.h"
#include "enhancements/shadow_detail.h"

#include <SDL3/SDL.h>

#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

typedef GLXContext (*CtxProc)(Display*, GLXFBConfig, GLXContext, Bool, const int*);

static const char* glDebugSourceStr(GLenum s)
{
	switch (s)
	{
		case GL_DEBUG_SOURCE_API: return "API";
		case GL_DEBUG_SOURCE_WINDOW_SYSTEM: return "WindowSystem";
		case GL_DEBUG_SOURCE_SHADER_COMPILER: return "ShaderCompiler";
		case GL_DEBUG_SOURCE_THIRD_PARTY: return "ThirdParty";
		case GL_DEBUG_SOURCE_APPLICATION: return "Application";
		default: return "Other";
	}
}
static const char* glDebugTypeStr(GLenum t)
{
	switch (t)
	{
		case GL_DEBUG_TYPE_ERROR: return "Error";
		case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: return "Deprecated";
		case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR: return "UndefinedBehavior";
		case GL_DEBUG_TYPE_PORTABILITY: return "Portability";
		case GL_DEBUG_TYPE_PERFORMANCE: return "Performance";
		case GL_DEBUG_TYPE_MARKER: return "Marker";
		default: return "Other";
	}
}
static const char* glDebugSeverityStr(GLenum s)
{
	switch (s)
	{
		case GL_DEBUG_SEVERITY_HIGH: return "High";
		case GL_DEBUG_SEVERITY_MEDIUM: return "Medium";
		case GL_DEBUG_SEVERITY_LOW: return "Low";
		default: return "Notification";
	}
}

// GL_KHR_debug callback (installed with GL_DEBUG_OUTPUT_SYNCHRONOUS, so this fires on the render
// thread at the offending call site, not queued to some later point). High severity and actual
// GL_DEBUG_TYPE_ERROR are real bugs - route them through the same reportError/P3FES_STRICT
// path as every other gap in this codebase. Lower severities are informational (deprecated usage,
// performance hints): print but don't report, so they don't count against the exceptions log.
static void GLAPIENTRY glDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity,
	GLsizei /*length*/, const GLchar* message, const void* /*userParam*/)
{
	// Shader-compiler messages are handled explicitly by gl::compileShader/checkProgramLinked (a bad
	// hot-reload edit is recoverable), so keep them non-fatal here; other high-severity errors are bugs.
	if ((severity == GL_DEBUG_SEVERITY_HIGH || type == GL_DEBUG_TYPE_ERROR) &&
		source != GL_DEBUG_SOURCE_SHADER_COMPILER)
	{
		SKY2_ERROR(false, "GL debug [%s/%s/%s] id=%u: %s", glDebugSourceStr(source),
			glDebugTypeStr(type), glDebugSeverityStr(severity), id, message);
	}
	else
	{
		std::fprintf(stderr, "[gl][%s/%s/%s] id=%u: %s\n", glDebugSourceStr(source),
			glDebugTypeStr(type), glDebugSeverityStr(severity), id, message);
	}
}

namespace sky2
{
	// Window dimensions below this are almost certainly a bad P3FES_NATIVE_RENDERER_W/H env value, not an
	// intentional tiny window; clamping avoids feeding 0/negative into XCreateWindow, the viewport,
	// the projection divide (drawFrame/Im2DRenderer::draw), and writePPM's buffer size (negative -> huge
	// size_t allocation).
	constexpr int MIN_WIN_DIM = 64;

	// A pass draws in submission order, so mesh and Im2D commands interleave. Call `run` once per
	// maximal run of one kind: the two renderers each set up a program and a VAO, and a run is the
	// largest span that can share one setup.
	template <class Fn>
	static void forEachDrawRun(const Frame& frame, const FramePass& p, Fn&& run)
	{
		for (size_t at = p.cmdBegin; at < p.cmdEnd;)
		{
			const CmdType kind = frame.commandList[frame.drawOrder[at]].type;
			size_t end = at;
			while (end < p.cmdEnd && frame.commandList[frame.drawOrder[end]].type == kind)
				end++;
			run(kind, std::span<const uint32_t>{frame.drawOrder.data() + at, end - at});
			at = end;
		}
	}
	static size_t countOfType(const Frame& frame, const FramePass& p, CmdType kind)
	{
		size_t n = 0;
		for (size_t at = p.cmdBegin; at < p.cmdEnd; at++)
			if (frame.commandList[frame.drawOrder[at]].type == kind)
				n++;
		return n;
	}
	static size_t meshCount(const Frame& frame, const FramePass& p)
	{
		return countOfType(frame, p, CmdType::Mesh);
	}
	static size_t im2dCount(const Frame& frame, const FramePass& p)
	{
		return countOfType(frame, p, CmdType::Im2d);
	}
	// Lights are gathered per object, so a pass has none of its own beyond fog; show its first mesh's.
	static const FrameLights& firstMeshLights(const Frame& frame, const FramePass& p)
	{
		for (uint32_t ci = p.cmdBegin; ci < p.cmdEnd; ci++)
			if (frame.commandList[ci].type == CmdType::Mesh)
				return frame.lights[frame.meshCmds[frame.commandList[ci].data].lightsIndex];
		return p.lights;
	}

	// The shaders are #version 450 core and the buffer paths use DSA, so a context below 4.5 would
	// fail later as a shader compile error or a null entry point. Fail here, where the cause is named.
	// SDL silently hands back whatever the driver offers, so the app-window path needs this as much as
	// the GLX one.
	static bool checkGlVersion()
	{
		if (!GLAD_GL_VERSION_4_5)
		{
			SKY2_ERROR(false, "GL 4.5 core required, got %s", glGetString(GL_VERSION));
			return false;
		}
		return true;
	}

	// Brackets a render pass with a GL_KHR_debug group (GL_DEBUG_TYPE_PUSH/POP_GROUP), so a GL
	// capture (RenderDoc/apitrace) groups that pass's calls under one label instead of a flat list.
	// No-ops if the extension isn't available.
	struct DebugGroup
	{
		DebugGroup(const Renderer& renderer, const char* name)
			: renderer(renderer)
		{
			if (renderer.window.hasDebug)
			{
				glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, name);
			}
		}
		~DebugGroup()
		{
			if (renderer.window.hasDebug)
			{
				glPopDebugGroup();
			}
		}
		DebugGroup(const DebugGroup&) = delete;
		DebugGroup& operator=(const DebugGroup&) = delete;

	private:
		const Renderer& renderer;
	};

	// Adopt an app-provided SDL3 window + GL context (set via sky2::api::setHostWindow) instead of
	// creating our own. The context is created on the app's main thread and released there, so the
	// render thread claims it here. Runs on the render thread.
	bool Renderer::adoptHostWindow()
	{
		SDL_Window* win = static_cast<SDL_Window*>(hostSdlWindow);
		SDL_GLContext ctx = static_cast<SDL_GLContext>(hostGlContext);
		if (!SDL_GL_MakeCurrent(win, ctx))
		{
			SKY2_ERROR(false, "SDL_GL_MakeCurrent failed: %s", SDL_GetError());
			return false;
		}
		if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress)))
		{
			SKY2_ERROR(false, "gladLoadGL failed (host window)");
			return false;
		}
		SDL_GL_SetSwapInterval(sky2::config().vsync ? 1 : 0);
		std::fprintf(stderr, "[sky2] GL %s | %s (app window)\n",
			glGetString(GL_VERSION), glGetString(GL_RENDERER));
		if (!checkGlVersion())
		{
			return false;
		}
		window.hasDebug = GLAD_GL_KHR_debug != 0;
		if (window.hasDebug)
		{
			glEnable(GL_DEBUG_OUTPUT);
			glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
			glDebugMessageCallback(glDebugCallback, nullptr);
			glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0,
				nullptr, GL_FALSE);
		}
		int w = 0, h = 0;
		SDL_GetWindowSizeInPixels(win, &w, &h);
		if (w > 0 && h > 0)
		{
			winW = w;
			winH = h;
		}
		return true;
	}

	bool Renderer::createWindow()
	{
		if (hasHostWindow)
		{
			return adoptHostWindow();
		}
		const Config& appCfg = sky2::config();
		if (appCfg.windowWidth < MIN_WIN_DIM || appCfg.windowHeight < MIN_WIN_DIM)
			SKY2_ERROR(false, "window %dx%d below min %d, clamping", appCfg.windowWidth,
				appCfg.windowHeight, MIN_WIN_DIM);
		winW = appCfg.windowWidth < MIN_WIN_DIM ? MIN_WIN_DIM : appCfg.windowWidth;
		winH = appCfg.windowHeight < MIN_WIN_DIM ? MIN_WIN_DIM : appCfg.windowHeight;
		window.dpy = XOpenDisplay(nullptr);
		if (!window.dpy)
		{
			SKY2_ERROR(false, "no X display");
			return false;
		}
		static int att[] = {GLX_X_RENDERABLE, True, GLX_RENDER_TYPE, GLX_RGBA_BIT,
			GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_DOUBLEBUFFER, True,
			GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8,
			GLX_DEPTH_SIZE, 24, GLX_STENCIL_SIZE, 8, None}; // depth: without it GL_DEPTH_TEST is a no-op
		int n = 0;
		GLXFBConfig* fbc = glXChooseFBConfig(window.dpy, DefaultScreen(window.dpy), att, &n);
		if (!fbc || n == 0)
		{
			SKY2_ERROR(false, "no FB config");
			return false;
		}
		GLXFBConfig cfg = fbc[0];
		XFree(fbc);
		XVisualInfo* vi = glXGetVisualFromFBConfig(window.dpy, cfg);
		if (!vi)
		{
			SKY2_ERROR(false, "FB config has no matching X visual");
			return false;
		}
		XSetWindowAttributes swa{};
		swa.colormap = XCreateColormap(window.dpy, RootWindow(window.dpy, vi->screen),
			vi->visual, AllocNone);
		swa.event_mask = ExposureMask | StructureNotifyMask; // resize + close; this window takes no input
		window.win = XCreateWindow(window.dpy, RootWindow(window.dpy, vi->screen),
			0, 0, winW, winH, 0,
			vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask, &swa);
		XStoreName(window.dpy, window.win, "P3FES native");
		window.wmDelete = XInternAtom(window.dpy, "WM_DELETE_WINDOW", False);
		// intercept the close button
		XSetWMProtocols(window.dpy, window.win, &window.wmDelete, 1);
		if (!appCfg.takeFocus)
		{
			// Unattended playback: tell the WM this window wants no keyboard input, so mapping it
			// doesn't pull focus off whatever the user is doing. It reads no key events anyway.
			XWMHints hints{};
			hints.flags = InputHint;
			hints.input = False;
			XSetWMHints(window.dpy, window.win, &hints);
			// The hint above only says the window takes no keyboard input, and a WM is free to focus
			// it regardless; WSLg's does. _NET_WM_USER_TIME = 0 is the EWMH way to say "this window
			// was not opened in response to user action", which is the part that asks not to be
			// raised or focused on map. Both are set: the hint is what an older WM reads.
			const Atom userTime = XInternAtom(window.dpy, "_NET_WM_USER_TIME", False);
			const unsigned long noUserAction = 0;
			XChangeProperty(window.dpy, window.win, userTime, XA_CARDINAL, 32, PropModeReplace,
				reinterpret_cast<const unsigned char*>(&noUserAction), 1);
		}
		// An unmapped window is still a valid GLX drawable, so the context and every FBO pass work
		// exactly as before; only the present has nothing to present to.
		if (appCfg.showWindow)
		{
			XMapWindow(window.dpy, window.win);
		}
		if (appCfg.takeFocus)
		{
			XRaiseWindow(window.dpy, window.win);
			XSync(window.dpy, False); // wait for the map to land before focusing, or X may reject it (BadMatch)
			// Don't rely on the window manager to focus a newly mapped window - under WSLg's minimal WM
			// this silently doesn't happen, leaving the window visible but unfocused.
			XSetInputFocus(window.dpy, window.win, RevertToParent, CurrentTime);
		}
		auto make = reinterpret_cast<CtxProc>(glXGetProcAddressARB(
			reinterpret_cast<const GLubyte*>("glXCreateContextAttribsARB")));
		if (!make)
		{
			SKY2_ERROR(false, "glXCreateContextAttribsARB extension not available");
			XFree(vi);
			return false;
		}
		// The debug bit must stay: some drivers stop generating messages without it, silencing
		// glDebugMessageCallback below.
		int ca[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 4, GLX_CONTEXT_MINOR_VERSION_ARB, 5,
			GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
			GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_DEBUG_BIT_ARB, None};
		window.ctx = make(window.dpy, cfg, nullptr, True, ca);
		XFree(vi);
		if (!window.ctx)
		{
			SKY2_ERROR(false, "no GL 4.5 core context");
			return false;
		}
		if (!glXMakeCurrent(window.dpy, window.win, window.ctx))
		{
			SKY2_ERROR(false, "glXMakeCurrent failed");
			return false;
		}
		if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glXGetProcAddressARB)))
		{
			SKY2_ERROR(false, "gladLoadGL failed");
			return false;
		}
		using SwapIntervalProc = void (*)(Display*, GLXDrawable, int);
		if (auto setSwapInterval = reinterpret_cast<SwapIntervalProc>(glXGetProcAddressARB(
				reinterpret_cast<const GLubyte*>("glXSwapIntervalEXT"))))
		{
			setSwapInterval(window.dpy, window.win, sky2::config().vsync ? 1 : 0);
		}
		std::fprintf(stderr, "[sky2] GL %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
		if (!checkGlVersion())
		{
			return false;
		}
		window.hasDebug = GLAD_GL_KHR_debug != 0;
		if (window.hasDebug)
		{
			glEnable(GL_DEBUG_OUTPUT);
			glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS); // callback fires at the offending call site
			glDebugMessageCallback(glDebugCallback, nullptr);
			// Notification severity is mostly driver placement/recompile hints, not actionable;
			// drop it so real errors and warnings aren't buried under it.
			glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0,
				nullptr, GL_FALSE);
		}
		else
		{
			SKY2_ERROR(false, "GL_KHR_debug not available, no GL debug output");
		}
		return true;
	}

	// Centered viewport preserving the game's aspect ratio (gameResW/H, 640x448) inside the actual
	// window: pillarboxed (bars left/right) if the window is relatively wider, letterboxed (bars
	// top/bottom) if relatively taller. Every pass (2D and 3D) renders in game-resolution NDC space
	// and relies on this single viewport transform to fit it into the window without distortion.
	Renderer::Viewport Renderer::letterboxViewport(int winW, int winH) const
	{
		const float resW = gameResW.load(), resH = gameResH.load();
		if (winW <= 0 || winH <= 0 || resW <= 0.0f || resH <= 0.0f)
		{
			return {0, 0, winW, winH};
		}
		const float gameAspect = resW / resH;
		const float winAspect = static_cast<float>(winW) / static_cast<float>(winH);
		if (winAspect > gameAspect)
		{
			const int w = static_cast<int>(std::lround(winH * gameAspect));
			return {(winW - w) / 2, 0, w, winH};
		}
		const int h = static_cast<int>(std::lround(winW / gameAspect));
		return {0, (winH - h) / 2, winW, h};
	}

	// Render at the resolution the game presents, unless a size was asked for. Only for a window
	// sky2 owns: with a host window the size is the user's and upscaling to it is the point. Without
	// this a PAL disc renders 640x448 against a 640x512 emulated frame, and a capture cannot be
	// compared at all.
	void Renderer::followGameResolution()
	{
		const Config& appCfg = sky2::config();
		if (hasHostWindow || appCfg.windowSizeExplicit)
		{
			return;
		}
		const int w = int(gameResW.load()), h = int(gameResH.load());
		if (w < MIN_WIN_DIM || h < MIN_WIN_DIM || (w == winW && h == winH))
		{
			return;
		}
		winW = w;
		winH = h;
		if (window.dpy && window.win)
		{
			XResizeWindow(window.dpy, window.win, unsigned(w), unsigned(h));
		}
	}

	// A capture's file base: the requested shot path with its "-native.ppm" / ".ppm" tail removed.
	// The manifest and the per-pass dumps hang off it, so they all share one name.
	static std::string captureBaseOf(const std::string& shotPath)
	{
		std::string base = shotPath;
		for (const char* suffix : {"-native.ppm", ".ppm"})
		{
			const size_t n = std::strlen(suffix);
			if (base.size() >= n && base.compare(base.size() - n, n, suffix) == 0)
			{
				base.resize(base.size() - n);
				break;
			}
		}
		return base;
	}

	// Present one frame: walk its passes in submission order, compositing to the window. A mesh/2D
	// draw's unified draw id is a running index across passes, so drawEnabled()
	// (P3FES_DRAW_ONLY/UPTO) and the manifest agree with the render order.
	void Renderer::renderFrame(const Frame& frame)
	{
		const std::vector<FramePass>& passes = frame.passes;
		gl.frameIndex = frame.index;
		{
			prof::Zone z("upload");
			uploadPendingTextures();
		}
		uploadBones(interp.bonesFor(frame));
		uploadTransientGeometry(frame);
		gl.im2d.uploadVerts(*this, frame);

		// P3FES_DRAW_TRACE=x0,y0,x1,y1 [+ _OUT]: a region's mean after every draw of the captured
		// frame. One run in place of a P3FES_DRAW_UPTO bisection, which is an emulator boot per step.
		if (!passDumpBase.empty() && traceRegionArmed() && !traceRegionOut)
		{
			traceRegionOut = std::fopen((passDumpBase + "-drawtrace.jsonl").c_str(), "w");
		}
		followGameResolution();
		const int winW = this->winW, winH = this->winH;
		ensureDisplayFbo(winW, winH);
		const Viewport vp = letterboxViewport(winW, winH);

		// Apply a pass's RwCameraClear to the currently-bound FBO. glClear ignores the viewport (no
		// scissor), so this clears the whole target, matching a PS2 CameraClear on the backbuffer/RT.
		auto applyClear = [](const PassClear& c) {
			GLbitfield mask = 0;
			if (c.color)
			{
				glClearColor(c.colorValue.r, c.colorValue.g, c.colorValue.b, c.colorValue.a);
				mask |= GL_COLOR_BUFFER_BIT;
			}
			if (c.depth)
			{
				mask |= GL_DEPTH_BUFFER_BIT;
			}
			if (c.stencil)
			{
				glStencilMask(0xFF); // glClear honors the stencil write mask
				mask |= GL_STENCIL_BUFFER_BIT;
			}
			if (mask)
			{
				glClear(mask);
			}
		};

		// Fresh display canvas each frame (black, the PS2 backbuffer border). Per-pass CameraClears
		// then clear whatever the game actually clears, when it clears it.
		bindDisplayFbo();
		glViewport(0, 0, winW, winH);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glStencilMask(0xFF); // glClear honors the stencil write mask
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

		// Passes in submission order. Display passes composite into the display FBO (letterbox
		// viewport); off-screen passes render into their own FBO and are published for sampling. A
		// draw's unified id is a running index across passes.
		// Snapshot what this frame's passes are, for the overlay's pass viewer.
		{
			std::vector<PassInfo>& info = this->passes;
			info.clear();
			info.reserve(passes.size());
			for (const FramePass& p : passes)
				// Lights are gathered per object, not per pass: FramePass::lights only carries the
				// camera's fog. Show the first mesh's gather, which is the lighting the pass's geometry
				// actually used; a 2D-only pass has none.
				info.push_back({p.target, p.camera, p.targetW, p.targetH, p.isDisplay,
					static_cast<int>(meshCount(frame, p)), static_cast<int>(im2dCount(frame, p)), p.clear.color,
					p.clear.depth, firstMeshLights(frame, p)});
			passDisabled.resize(info.size(), 0); // keeps existing toggles by position
		}
		static int sLoggedPasses = 0;
		static const bool sLogPasses = sky2::env("LOG_PASSES") != nullptr;
		const bool logPasses = sLogPasses && sLoggedPasses < 4;
		for (const FramePass& p : passes)
		{
			if (logPasses)
				std::fprintf(stderr, "[sky2][pass] %s target=0x%X %dx%d meshes=%zu im2d=%zu\n",
					p.isDisplay ? "DISPLAY" : "offscreen",
					p.target, p.targetW, p.targetH, meshCount(frame, p), im2dCount(frame, p));
			const uint32_t passIndex = static_cast<uint32_t>(&p - passes.data());
			// Switched off in the overlay: skip it. Draw ids are command-list positions, so nothing
			// needs advancing to keep manifest ids and P3FES_DRAW_ONLY meaning the same thing.
			if (passIndex < passDisabled.size() && passDisabled[passIndex])
			{
				continue;
			}
			passIsDisplay = p.isDisplay; // display keeps straight-alpha; RTs use the game's blend
			if (p.isDisplay)
			{
				bindDisplayFbo();
				applyClear(p.clear); // a mid-frame CameraClear on the backbuffer (fades, wipes)
				glViewport(vp.x, vp.y, vp.w, vp.h);
				forEachDrawRun(frame, p, [&](CmdType kind, std::span<const uint32_t> run) {
					if (kind == CmdType::Mesh)
					{
						DebugGroup dbg(*this, "MeshPass");
						prof::Zone z("mesh");
						drawMeshes(frame, run, p.lights, interp.vpFor(p, passIndex), passIndex);
					}
					else
					{
						DebugGroup dbg(*this, "Im2DPass");
						prof::Zone z("im2d"); // includes the -feedback subset (snapshot blits)
						gl.im2d.draw(*this, frame, run);
					}
				});
			}
			else
			{
				DebugGroup dbg(*this, "OffscreenTarget");
				prof::Zone z("offscreen");
				bindOffscreenTarget(p.target, p.targetW, p.targetH);
				// A fresh RT each pass: honor the game's clear color, else transparent black so
				// undrawn areas sample as clear. Depth is always cleared for a fresh target.
				PassClear rtClear = p.clear;
				rtClear.depth = true;
				if (!rtClear.color)
				{
					rtClear.color = true;
					rtClear.colorValue = glm::vec4(0.0f);
				}
				applyClear(rtClear);
				forEachDrawRun(frame, p, [&](CmdType kind, std::span<const uint32_t> run) {
					if (kind == CmdType::Mesh)
						drawMeshes(frame, run, p.lights, interp.vpFor(p, passIndex), passIndex);
					else
						// Im2D coords in an off-screen RT are in the RT's pixel space, not 640x448.
						gl.im2d.draw(*this, frame, run, static_cast<float>(p.targetW),
							static_cast<float>(p.targetH));
				});
				finishOffscreenTarget(p.target); // publish its color for later passes to sample
			}
			// Every pass of a captured frame that did anything, colour and alpha, while its target is
			// still bound. A screenshot only ever shows the last display pass, and never the alpha
			// channel at all. A pass with no draws and no clear left its target exactly as the
			// previous one did, so dumping it writes a megabyte to say nothing.
			const bool passDidWork = p.cmdEnd > p.cmdBegin || p.clear.color || p.clear.depth ||
									 p.clear.stencil || !p.isDisplay;
			if (!passDumpBase.empty() && passDidWork)
			{
				if (p.isDisplay)
				{
					bindDisplayFbo();
					resolveDisplay(); // the multisampled buffer is not readable
					glBindFramebuffer(GL_FRAMEBUFFER, gl.displayFbo);
					dumpPassImage(passDumpBase, static_cast<int>(passIndex), gl.displayW, gl.displayH);
					bindDisplayFbo();
				}
				else
				{
					bindOffscreenTarget(p.target, p.targetW, p.targetH);
					dumpPassImage(passDumpBase, static_cast<int>(passIndex), p.targetW, p.targetH);
				}
			}
		}
		if (logPasses)
		{
			sLoggedPasses++;
			std::fprintf(stderr, "[sky2][pass] --- end frame ---\n");
		}
		passIsDisplay = true; // remaining draws (text overlay, present) target the display
		bindDisplayFbo();
		glViewport(vp.x, vp.y, vp.w, vp.h);
		{
			prof::Zone z("present");
			// Before the present blit, while the display FBO still holds this frame: keep a copy for a
			// draw that binds the displayed buffer next frame.
			capturePreviousFrame();
			blitDisplayToWindow(winW, winH); // present: display FBO -> default framebuffer
		}
		evictUnusedGeom(); // reclaim GPU geometry no longer referenced by a live frame
		prof::frame();
	}

	void Renderer::renderLoop()
	{
		if (const char* r = sky2::env("DRAW_TRACE"))
		{
			std::sscanf(r, "%d,%d,%d,%d", &traceRect[0], &traceRect[1], &traceRect[2], &traceRect[3]);
			std::fprintf(stderr, "[sky2] draw trace over (%d,%d)-(%d,%d) -> <capture>-drawtrace.jsonl\n",
				traceRect[0], traceRect[1], traceRect[2], traceRect[3]);
		}
		if (!createWindow())
		{
			initFailed.store(true); // createWindow already reported the specific failure
			running.store(false);
			return;
		}
		initGL();
		gl.im2d.init(*this);
		init3D();
		if (hasHostWindow)
			overlay.init(); // debug overlay only in the interactive (app-owned) window
		ready.store(true); // GL is up: open() can stop waiting
		// The frame being presented, owned by the ring. Null until the build thread publishes the first.
		Frame* current = nullptr;
		static const Frame emptyFrame; // stands in before the first publish, so the loop needs no branch
		// Screenshot of the NATIVE sky2 render, counted in render frames. P3FES_EMU_SHOT is the
		// emulated-GS counterpart; the two are different images and only COMPARE aligns them.
		const char* shot = sky2::env("NATIVE_SHOT");
		int shotRF = 240;
		if (const char* s = sky2::env("NATIVE_SHOT_FRAME"))
		{
			shotRF = std::atoi(s);
		}
		bool shotDone = false;
		// Present-rate cap from config (fps_cap / P3FES_FPS_CAP); 0 = start uncapped. Tab toggles the
		// limiter at runtime, so the rate is kept even while it is off.
		interp.setEnabled(sky2::config().interpolate);
		vsync.store(sky2::config().vsync);
		bool vsyncApplied = sky2::config().vsync;
		enhancements::initShadowDetail();
		enhancements::initMsaa();
		const int fpsCap = sky2::config().fpsCap;
		frameLimitRate = fpsCap > 0 ? fpsCap : 60;
		frameLimiter.store(fpsCap > 0);
		auto nextPresent = std::chrono::steady_clock::now();
		auto lastFrameStart = nextPresent;
		int statFrames = 0;
		auto statWindowStart = nextPresent;
		for (int rf = 0; running.load(); rf++)
		{
			// Self-owned window only: pump its own X11 events (resize + close). In the app-owned
			// path the app pumps SDL events and feeds resize/close through sky2::api::onResize/requestClose,
			// and gameplay input goes app -> core directly, so there is no key handler here anymore.
			if (!hasHostWindow)
			{
				while (XPending(window.dpy))
				{
					XEvent ev;
					XNextEvent(window.dpy, &ev);
					if (ev.type == ConfigureNotify)
					{
						winW = ev.xconfigure.width;
						winH = ev.xconfigure.height;
					}
					else if (ev.type == ClientMessage &&
							 static_cast<Atom>(ev.xclient.data.l[0]) == window.wmDelete)
					{
						closeRequested.store(true); // close button: shut down gracefully
						running.store(false);
					}
				}
			}
			if (!running.load())
			{
				break;
			}
			if (current)
				interp.captureFrameBeingReplaced(*current);
			Frame* next = frames->takePending();
			if (next)
			{
				current = next;
				interp.beginTick(*next);
			}
			// Re-presenting the frame already on screen runs its passes again, and the display-buffer
			// feedback tap compounds one more time for a frame the game never produced. How often that
			// happens is pure render-thread timing, so a capture run draws only what the build thread
			// published. An interactive run keeps re-presenting: that is what makes it smooth.
			if (!next && renderOnlyNewFrames)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}
			if (reqShaderReload.exchange(false))
			{
				shaders->reloadShaders(); // GL context is current on this thread
			}
			if (const bool want = vsync.load(); want != vsyncApplied)
			{
				vsyncApplied = want;
				if (hasHostWindow)
				{
					SDL_GL_SetSwapInterval(want ? 1 : 0);
				}
				else if (auto set = reinterpret_cast<void (*)(Display*, GLXDrawable, int)>(
							 glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXSwapIntervalEXT"))))
				{
					set(window.dpy, window.win, want ? 1 : 0);
				}
				std::fprintf(stderr, "[sky2] vsync %s\n", want ? "on" : "off");
			}
			const Frame& frame = current ? *current : emptyFrame;
			// Peek at the pending capture before rendering, so the pass loop can dump each pass as it
			// finishes. Taking it here rather than after would consume the request the block below owns.
			{
				std::lock_guard<std::mutex> lk(shotMutex);
				passDumpBase = (!reqShotPath.empty() && current && current->index >= reqShotIndex)
					? captureBaseOf(reqShotPath)
					: std::string();
			}
			interp.beginDisplayFrame(frame); // one blend point for everything this frame draws
			renderFrame(frame);
			passDumpBase.clear();
		if (traceRegionOut)
		{
			std::fclose(traceRegionOut);
			traceRegionOut = nullptr;
		}
			if (shot && !shotDone && rf >= shotRF)
			{
				writePPM(shot);
				shotDone = true;
			}
			// On-demand capture requested from the EE thread (compare mode).
			std::string reqShot;
			{
				std::lock_guard<std::mutex> lk(shotMutex);
				// Wait for the pinned frame. The ring holds it Pending until this thread takes it, so
				// this only skips the frames built before the request was posted.
				if (!reqShotPath.empty() && current && current->index >= reqShotIndex)
					reqShot.swap(reqShotPath);
			}
			if (!reqShot.empty())
			{
				// Write the manifest next to the shot (same unique base) instead of a fixed name, so
				// each capture keeps its own manifest for renaming later.
				const std::string manifest = captureBaseOf(reqShot) + "-manifest.jsonl";
				debugWriteManifest(frame, manifest.c_str()); // unique per capture, for renaming later
				debugWriteManifest(frame, "experiments/dumps/skygl_manifest.jsonl"); // fixed "latest" (tooling convention)
				writePPM(reqShot.c_str());
				shotsWritten.fetch_add(1); // publish after the files are on disk
			}
			if (hasHostWindow)
			{
				overlay.frame(winW, winH); // draw the overlay onto the presented frame
				SDL_GL_SwapWindow(static_cast<SDL_Window*>(hostSdlWindow));
			}
			else if (sky2::config().showWindow)
				glXSwapBuffers(window.dpy, window.win); // present after the optional back-buffer read
			// Pace to the cap by the remaining time to the next tick, not a flat sleep on top of render
			// time (which capped the effective rate at 1000/(render+16) ms). A frame that overran the cap
			// resets the target to now, so a slow frame does not build up a catch-up burst. fps_cap 0 =
			// uncapped: no sleep, present as fast as the render allows.
			// Render+present cost without the pacing sleep, so the overlay can report the ceiling the
			// cap is hiding. Smoothed, since a single frame's number is too jittery to read.
			{
				const auto now = std::chrono::steady_clock::now();
				Renderer::Stats& st = stats;
				const float renderMs = std::chrono::duration<float, std::milli>(now - lastFrameStart).count();
				st.renderMs = st.renderMs > 0.0f ? st.renderMs * 0.9f + renderMs * 0.1f : renderMs;
				st.passes = static_cast<int>(frame.passes.size());
				st.meshes = static_cast<int>(frame.meshCmds.size());
				st.im2dBatches = static_cast<int>(frame.im2dCmds.size());
				statFrames++;
				const float windowMs = std::chrono::duration<float, std::milli>(now - statWindowStart).count();
				if (windowMs >= 500.0f)
				{
					st.fps = statFrames * 1000.0f / windowMs;
					st.frameMs = windowMs / statFrames;
					statFrames = 0;
					statWindowStart = now;
					// P3FES_LOG_FPS: the same numbers the overlay shows, for headless runs. Unlike
					// P3FES_PROFILE this adds no glFinish, so it measures the renderer as it really runs.
					static const bool sLogFps = sky2::env("LOG_FPS") != nullptr;
					if (sLogFps)
						std::fprintf(stderr, "[sky2][fps] %.1f fps  frame %.2f ms  render %.2f ms\n",
							st.fps, st.frameMs, st.renderMs);

				}
			}
			interp.logFrame();
			// The swap blocks until the display is ready, so pacing it again with a sleep would only
			// add a second, unrelated cadence on top.
			if (frameLimiter.load() && !vsync.load())
			{
				// Read the rate each frame: the overlay can change it while the loop is running.
				const int rate = frameLimitRate > 0 ? frameLimitRate : 60;
				nextPresent += std::chrono::microseconds(1000000 / rate);
				const auto now = std::chrono::steady_clock::now();
				if (now < nextPresent)
					std::this_thread::sleep_until(nextPresent);
				else
					nextPresent = now;
			}
			else
			{
				nextPresent = std::chrono::steady_clock::now(); // no catch-up burst when re-enabled
			}
			lastFrameStart = std::chrono::steady_clock::now();
		}
		// Clean teardown so closing the window never breaks the connection.
		if (hasHostWindow)
			overlay.shutdown();
		gl.destroy(*shaders, *textures); // free all GL resources + caches while the context is still current
		if (hasHostWindow)
		{
			// The app owns the window + context; just release the context from this thread.
			SDL_GL_MakeCurrent(static_cast<SDL_Window*>(hostSdlWindow), nullptr);
		}
		else
		{
			glXMakeCurrent(window.dpy, None, nullptr);
			if (window.ctx)
			{
				glXDestroyContext(window.dpy, window.ctx);
			}
			if (window.win)
			{
				XDestroyWindow(window.dpy, window.win);
			}
			XCloseDisplay(window.dpy);
			window.dpy = nullptr;
		}
	}

} // namespace sky2
