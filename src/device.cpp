// sky2 device: render-state tracking, the camera/present entry points, and the
// SkyIm2DRender* family (the game's 2D UI funnel).
#include "common.h"
#include "sky2/addresses.h" // sky2::pluginOffset: capture curWorld from the camera's world plugin ext
#include "object_manager.h"
#include "render_queue.h"
#include "renderer.h"
#include "rw_to_gl.h"
#include "texture_manager.h"

#include "device.h" // Device: owns the four subsystems (composition root)

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>

namespace sky2
{
	// Wire the inter-subsystem references after all members are constructed (they are default-constructed
	// in declaration order first). ObjectManager<->TextureManager is a cycle, so it must be 2-phase.
	Device::Device()
	{
		textures.objects = &objects;
		objects.textures = &textures;
		queue.renderer = &renderer;
		queue.objects = &objects;
		queue.frames = &frames;
		queue.geometry = &geometry;
		queue.textures = &textures;
		objects.geometry = &geometry;
		renderer.frames = &frames;
		// A compare capture is a measurement, so it must render every frame the game builds: the
		// display-buffer feedback tap reads the previous rendered frame, and skipping one makes two
		// runs of the same capture disagree. Interactive runs keep skipping.
		const bool capturing = sky2::env("COMPARE") != nullptr;
		frames.renderEveryFrame(capturing);
		// Arm the display copy up front rather than on the first tap that asks for it: that request
		// crosses to the render thread mid-frame, so which frame starts keeping the copy depends on
		// where the two threads happen to be, and the tap converges over a different number of frames
		// each run. Costs one blit per frame in a capture run, which is not a run that is timed.
		objects.prevFrameWanted.store(capturing);
		renderer.renderOnlyNewFrames = capturing;
		renderer.geometry = &geometry;
		renderer.textures = &textures;
		renderer.objects = &objects;
		renderer.shaders = &shaders;
		renderer.overlay.device = this;
		atomic.device = this;
		ptank.device = this;
		sector.device = this;
		im3d.device = this;
		im2d.device = this;
		rt2d.device = this;
	}

	//Logs each unique (call site, message) pair once to a dedicated file so a playthrough collects every gap; aborts only under P3FES_STRICT.
	void reportError(const char* srcFile, int line, const char* func, const char* fmt, ...)
	{
		char msg[512];
		va_list ap;
		va_start(ap, fmt);
		std::vsnprintf(msg, sizeof(msg), fmt, ap);
		va_end(ap);
		const char* base = std::strrchr(srcFile, '/');
		srcFile = base ? base + 1 : srcFile;
		char site[300];
		std::snprintf(site, sizeof(site), "%s:%d", srcFile, line);
		static std::mutex mutex;
		static std::unordered_set<std::string> seen;
		static std::ofstream logFile("experiments/logs/sky2.log", std::ios::trunc);
		static bool warnedFileOpenFailed = false;
		std::lock_guard<std::mutex> lock(mutex);

		if (seen.insert(std::string(site) + ' ' + msg).second)
		{
			std::fprintf(stderr, "[sky2][critical] %s %s(): %s\n", site, func, msg);
			if (logFile.is_open())
			{
				logFile << site << ' ' << func << "(): " << msg << '\n';
				logFile.flush();
			}
			else if (!warnedFileOpenFailed)
			{
				warnedFileOpenFailed = true;
				std::fprintf(stderr, "[sky2] failed to open experiments/logs/sky2.log for writing\n");
			}
		}
		if (sky2::config().strict)
		{
			std::abort();
		}
	}

	// B8: mirror RW's default render-state vector so draws issued before the game's first
	// kwlnPushCommonRenderStates read sane values. queue.rsHits stays 0 so the "was it set by the game"
	// fallbacks (ztest/zwrite/cull in parseAtomic) still apply; this fixes the unguarded direct reads
	// (SRCBLEND/DESTBLEND -> rwBlendToGL) that otherwise hit RwBlendFunction 0 before the first set.
	void Device::setInitialRenderStateDefaults()
	{
		queue.renderState[rwRENDERSTATETEXTUREADDRESS] = rwTEXTUREADDRESSWRAP;
		queue.renderState[rwRENDERSTATETEXTUREADDRESSU] = rwTEXTUREADDRESSWRAP;
		queue.renderState[rwRENDERSTATETEXTUREADDRESSV] = rwTEXTUREADDRESSWRAP;
		queue.renderState[rwRENDERSTATEZTESTENABLE] = 1;
		queue.renderState[rwRENDERSTATESHADEMODE] = rwSHADEMODEGOURAUD;
		queue.renderState[rwRENDERSTATEZWRITEENABLE] = 1;
		queue.renderState[rwRENDERSTATETEXTUREFILTER] = rwFILTERLINEAR;
		queue.renderState[rwRENDERSTATESRCBLEND] = rwBLENDSRCALPHA;
		queue.renderState[rwRENDERSTATEDESTBLEND] = rwBLENDINVSRCALPHA;
		queue.renderState[rwRENDERSTATECULLMODE] = rwCULLMODECULLBACK;
	}

	// Returns false when the render thread could not bring up its window/GL context. Callers that have
	// nothing to fall back on (offline replay) must not keep dispatching into a dead renderer.
	bool Device::open()
	{
		if (renderer.running.exchange(true))
		{
			return !renderer.initFailed.load();
		}
		setInitialRenderStateDefaults();
		renderer.thread = std::thread(&Renderer::renderLoop, &renderer);
		while (!renderer.ready.load() && !renderer.initFailed.load())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return !renderer.initFailed.load();
	}

	uint64_t Device::shotsWritten()
	{
		return renderer.shotsWritten.load();
	}

	void Device::close()
	{
		// Signal the render thread to exit, then join it whenever it is still joinable. The join must
		// NOT be gated on renderer.running: the render thread's own close handler can clear renderer.running first, and
		// gating the join behind it left the global thread joinable -> std::terminate in its destructor
		// at process exit. Callers are serialized (CPU thread, then the app after cpu.join), so the
		// joinable() check is enough to make a second Close a no-op.
		renderer.running.store(false);
		if (renderer.thread.joinable())
		{
			renderer.thread.join();
			logRenderStateUsage();
		}
	}

	bool Device::closeRequested()
	{
		return renderer.closeRequested.load();
	}

	bool Device::consumeRtCaptureRequest()
	{
		return objects.rtSampleWanted.exchange(false);
	}

	void Device::setGameResolution(float w, float h)
	{
		// Every display pass offers its size, so most calls are the same value. Only store a change:
		// the render thread reads these, and a write per pass is a write the reader has to see.
		if (renderer.gameResW.load() == w && renderer.gameResH.load() == h)
		{
			return;
		}
		renderer.gameResW = w;
		renderer.gameResH = h;
	}

	void Device::setHostWindow(void* sdlWindow, void* glContext)
	{
		renderer.hostSdlWindow = sdlWindow;
		renderer.hostGlContext = glContext;
		renderer.hasHostWindow = (sdlWindow != nullptr && glContext != nullptr);
	}

	void Device::onResize(int width, int height)
	{
		if (width > 0 && height > 0)
		{
			renderer.winW = width;
			renderer.winH = height;
		}
	}

	void Device::requestClose()
	{
		renderer.closeRequested.store(true);
		renderer.running.store(false);
	}

	// Capture the pass's camera-scoped state: view-projection and fog (into queue.buildLights). Scene
	// lights are gathered per object at the render callbacks (gatherLights), not here. Called as a
	// fallback at BeginUpdate and authoritatively at EndUpdate.
	void Device::captureCameraState(FramePass& pass, uint32_t cameraAddr)
	{
		if (!cameraAddr)
		{
			return;
		}
		const auto camera = guest::ptr<RwCamera>(cameraAddr);
		pass.vp = buildCameraVP(cameraAddr, &pass.cam);
		queue.buildLights.fogStart = camera->fogPlane;
		// The Sky far-fog-plane state, where the game sets one, is what fog reaches full density at;
		// the camera far plane is only the fallback. Read per pass: the game owns that float and may
		// change it without re-setting the render state.
		queue.buildLights.fogEnd = camera->farPlane;
		if (queue.farFogPlaneAddr)
		{
			const float far = *guest::ptr<float>(queue.farFogPlaneAddr);
			if (std::isfinite(far) && far > queue.buildLights.fogStart)
			{
				queue.buildLights.fogEnd = far;
			}
			else
			{
				SKY2_ERROR(false, "FARFOGPLANE *0x%08X = %f is not past the fog plane %f; using the "
								   "camera far plane",
					queue.farFogPlaneAddr, far, queue.buildLights.fogStart);
			}
		}
		queue.buildLights.fogColor = toVec3(RwRGBA::fromRwLong(queue.renderState[rwRENDERSTATEFOGCOLOR]));
		pass.lights = queue.buildLights;
	}

	void Device::cameraBeginUpdate(uint32_t cameraAddr)
	{
		if (!cameraAddr || !guest::ready())
		{
			SKY2_ERROR(false, "cameraAddr=0x%X guest::ready()=%d - refusing to poison "
							   "the frame",
				cameraAddr, guest::ready());
			return;
		}
		const auto camera = guest::ptr<RwCamera>(cameraAddr);
		queue.activeCamera = cameraAddr;
		// curWorld <- the camera's world (RW's begin-update contract). The world lives in the camera's
		// RpWorld plugin ext (discovered offset); world sits at ext+0x0c (RpWorldCameraExt.world).
		if (const uint32_t extOff =
				sky2::pluginOffset(rwCAMERABASE, rwID_WORLDCAMERAPLUGIN, sky2::addresses().cameraExtOffset))
			queue.curWorld = *guest::ptr<uint32_t>(cameraAddr + extOff + 0x0c);
		queue.activeTarget = camera->frameBuffer.addr; // this pass's render target
		objects.registerRaster(queue.activeTarget); // record every render target we draw to
		queue.haveCam = true;

		// Open a new pass for this camera. Draws until the next Begin/present land in it, so the
		// frame is an ordered list of per-target passes (see FramePass). The camera state (vp/lights)
		// is captured now as a fallback and finalized at CameraEndUpdate.
		FramePass pass;
		pass.target = queue.activeTarget;
		pass.camera = cameraAddr;
		pass.isDisplay = queue.onScreenTarget(); // reaches the screen; before the first present, all do
		if (queue.activeTarget)
		{
			const auto r = guest::ptr<RwRaster>(queue.activeTarget);
			pass.targetW = r->width;
			pass.targetH = r->height;
			// The game's resolution is whatever it presents, not a constant: a PAL disc renders
			// 640x512. Take it from a raster ShowRaster has named, never from the pre-first-present
			// guess in onScreenTarget, which would adopt an off-screen effect target's size.
			if (objects.displayRasters.count(queue.activeTarget) && pass.targetW > 0 && pass.targetH > 0)
			{
				setGameResolution(float(pass.targetW), float(pass.targetH));
			}
		}
		captureCameraState(pass, cameraAddr);
		// An off-screen color pass is a render target the game may sample later (portrait/effect RTs).
		// Record it so a draw binding this raster samples the target's FBO instead of trying to decode
		// PS2 pixels (see convertRaster).
		if (!pass.isDisplay && queue.activeTarget)
		{
			objects.renderTargetRasters.insert(queue.activeTarget);
		}
		if (static const bool sLogRt = sky2::env("LOG_RT") != nullptr; sLogRt)
		{
			const auto c = guest::ptr<RwCamera>(cameraAddr);
			std::fprintf(stderr, "[sky2][rt] pass target=0x%X %dx%d display=%d cam=0x%X "
								 "vw=(%.3f,%.3f) voff=(%.3f,%.3f) near=%.3f far=%.3f proj=%d frame=0x%X\n",
				queue.activeTarget, pass.targetW, pass.targetH, pass.isDisplay ? 1 : 0,
				cameraAddr, float(c->viewWindow.x), float(c->viewWindow.y),
				float(c->viewOffset.x), float(c->viewOffset.y), float(c->nearPlane), float(c->farPlane),
				int32_t(c->projectionType), uint32_t(c->object.object.parent.addr));
		}
		if (queue.activeTarget) // record for GS-resident-texture origin tracing (objects.traceGsResidentRaster)
		{
			objects.cameraTargetsSeen[queue.activeTarget] = {pass.targetW, pass.targetH,
				pass.isDisplay ? "display" : "offscreen-RT"};
		}
		// openPass anchored the command range at the current end of the list; assigning the built pass
		// over it would reset that to zero and hand this pass every earlier draw.
		FramePass& slot = queue.buildFrame().openPass();
		const uint32_t begin = slot.cmdBegin, end = slot.cmdEnd;
		slot = std::move(pass);
		slot.cmdBegin = begin;
		slot.cmdEnd = end;
		// A held clear belongs to this pass: either it arrived before any pass existed, or it named a
		// raster the open pass was not drawing into. Assigning the built pass above would have
		// overwritten it, so it lands after the move.
		if (queue.havePendingClear)
		{
			slot.clear = queue.pendingClear;
			queue.havePendingClear = false;
		}
	}

	void Device::cameraEndUpdate()
	{
		// The pass commit point (like glEnd): re-capture the camera/lights now that the game has
		// finished configuring them for this pass, over the BeginUpdate fallback.
		if (queue.buildFrame().passCount() == 0 || !queue.activeCamera)
		{
			return;
		}
		captureCameraState(queue.currentBuildPass(), queue.activeCamera);
	}

	void Device::cameraClearColor(uint32_t cameraAddr, uint32_t colorPtr, int flags)
	{
		// RwCameraClear(camera, RwRGBA*, flags): attach the clear to the pass currently being built,
		// so each target (backbuffer, effect RT) is cleared with its own color/flags on
		// the render thread. Fires after CameraBeginUpdate, so the current pass is this camera's.
		PassClear parsed;
		parsed.depth = (flags & rwCAMERACLEARZ) != 0;
		parsed.stencil = (flags & rwCAMERACLEARSTENCIL) != 0;
		parsed.color = colorPtr && (flags & rwCAMERACLEARIMAGE);
		if (parsed.color)
		{
			parsed.colorValue = toVec4(*guest::ptr<RwRGBA>(colorPtr)); // RwRGBA is r,g,b,a bytes
		}
		// A clear names the raster it clears, through its camera's frame buffer. When that is not the
		// pass currently open, the clear belongs to a pass that has not started yet: the game sets the
		// camera's buffer, clears it, and only then calls BeginUpdate. Applying it to the open pass
		// wipes what that pass just drew, which is Shadow the Hedgehog's whole world.
		const uint32_t clearTarget = cameraAddr ? guest::ptr<RwCamera>(cameraAddr)->frameBuffer.addr : 0;
		const bool noPassYet = queue.buildFrame().passCount() == 0;
		const bool otherTarget = clearTarget && !noPassYet && queue.currentBuildPass().target &&
		                         clearTarget != queue.currentBuildPass().target;
		if (static const bool sLogRt = sky2::env("LOG_RT") != nullptr; sLogRt)
		{
			std::fprintf(stderr, "[sky2][rt] clear flags=0x%X camera=0x%X raster=0x%X -> %s\n", flags,
				cameraAddr, clearTarget,
				(noPassYet || otherTarget) ? "held for the next pass" : "the open pass");
		}
		if (noPassYet || otherTarget)
		{
			queue.pendingClear = parsed;
			queue.havePendingClear = true;
			return;
		}
		queue.currentBuildPass().clear = parsed;
	}

	// Safety bound for accumulating presents into one frame (see ShowRaster): far above any real
	// multi-pass effect's pass count, so it only triggers if a frame never draws the display.
	constexpr size_t MAX_PASSES_PER_FRAME = 256;

	void Device::showRaster(uint32_t rasterAddr)
	{
		if (rasterAddr)
		{
			objects.displayRasters.insert(rasterAddr); // this raster is on-screen
			queue.buildDisplayRaster = rasterAddr; // the target to blit to the window this frame
			objects.registerRaster(rasterAddr);
		}
		// A visual frame can span several presents: multi-pass effects (glows, blurs) render off-screen
		// RTs and flip the buffer between them before the final scene present. End the sky2 frame only
		// once the display itself has been drawn this cycle (a pass targeting an on-screen raster), so
		// off-screen RT passes ride in the same frame as the pass that samples them. Otherwise the render
		// thread, which consumes only the latest frame, drops the RT-producing present and the effect's
		// source is gone. A size cap guards a frame that never draws the display.
		bool hasDisplayPass = queue.buildFrame().passCount() > MAX_PASSES_PER_FRAME;
		for (const FramePass& p : queue.buildFrame().livePasses())
		{
			if (p.isDisplay)
			{
				hasDisplayPass = true;
				break;
			}
		}
		if (hasDisplayPass)
		{
			queue.endFrame(); // hand the ordered pass list to the render thread
			queue.beginFrame(); // start the next frame
			textures.beginFrame(); // age the content cache
		}
		// else: keep accumulating; this intermediate present only produced off-screen RTs.
	}

	const char* rsName(unsigned s)
	{
		static const char* NAMES[] = {
			"NA", "TEXTURERASTER", "TEXTUREADDRESS", "TEXTUREADDRESSU", "TEXTUREADDRESSV",
			"TEXTUREPERSPECTIVE", "ZTESTENABLE", "SHADEMODE", "ZWRITEENABLE", "TEXTUREFILTER",
			"SRCBLEND", "DESTBLEND", "VERTEXALPHAENABLE", "BORDERCOLOR", "FOGENABLE", "FOGCOLOR",
			"FOGTYPE", "FOGDENSITY", "(18)", "(19)", "CULLMODE", "STENCILENABLE", "STENCILFAIL",
			"STENCILZFAIL", "STENCILPASS", "STENCILFUNCTION", "STENCILFUNCTIONREF",
			"STENCILFUNCTIONMASK", "STENCILFUNCTIONWRITEMASK",
			"ALPHATESTFUNCTION", "ALPHATESTFUNCTIONREF"};
		return s < (sizeof(NAMES) / sizeof(NAMES[0])) ? NAMES[s] : "?";
	}

	// How sky2 treats each RwRenderState: consumed (has a real effect), or deliberately ignored
	// (we know it is set and have a reason to skip it). Anything that is NEITHER asserts in
	// SetRenderState, so a new/unclassified state can never be silently dropped.
	enum RsHandling
	{
		RS_CONSUMED,
		RS_IGNORED,
		RS_UNHANDLED
	};

	RsHandling rsHandling(unsigned s)
	{
		switch (s)
		{
			case rwRENDERSTATETEXTUREADDRESS:
			case rwRENDERSTATETEXTUREADDRESSU:
			case rwRENDERSTATETEXTUREADDRESSV:
			case rwRENDERSTATETEXTUREFILTER:
			case rwRENDERSTATEZTESTENABLE:
			case rwRENDERSTATESHADEMODE:
			case rwRENDERSTATEZWRITEENABLE:
			case rwRENDERSTATESRCBLEND:
			case rwRENDERSTATEDESTBLEND:
			case rwRENDERSTATEVERTEXALPHAENABLE:
			case rwRENDERSTATEFOGENABLE:
			case rwRENDERSTATEFOGCOLOR:
			case rwRENDERSTATEFOGTYPE:
			case rwRENDERSTATECULLMODE:
			// Stencil test: folded into queue.stencil (updateStencilFromRwState) and applied per draw.
			case rwRENDERSTATESTENCILENABLE:
			case rwRENDERSTATESTENCILFAIL:
			case rwRENDERSTATESTENCILZFAIL:
			case rwRENDERSTATESTENCILPASS:
			case rwRENDERSTATESTENCILFUNCTION:
			case rwRENDERSTATESTENCILFUNCTIONREF:
			case rwRENDERSTATESTENCILFUNCTIONMASK:
			case rwRENDERSTATESTENCILFUNCTIONWRITEMASK:
				return RS_CONSUMED;
			case rwRENDERSTATETEXTURERASTER: // read by the Im2D path at draw time, not here
			case rwRENDERSTATEFOGDENSITY: // only linear fog is implemented; density feeds exp fog
				return RS_IGNORED;
			case rwRENDERSTATETEXTUREPERSPECTIVE:
			case rwRENDERSTATEBORDERCOLOR:
			case rwRENDERSTATEALPHATESTFUNCTION:
			case rwRENDERSTATEALPHATESTFUNCTIONREF:
			default:
				return RS_UNHANDLED;
		}
	}

	bool rsConsumed(unsigned s) { return rsHandling(s) == RS_CONSUMED; }

	// Recompute queue.rwBlend from the RW SRCBLEND/DESTBLEND states. Kept separate from gsBlend: the
	// GS ALPHA register poke (setGsBlend) wins at read time (blendState()), so this never overwrites it.
	void Device::updateBlendFromRwState()
	{
		const unsigned src = queue.renderState[rwRENDERSTATESRCBLEND];
		const unsigned dst = queue.renderState[rwRENDERSTATEDESTBLEND];
		// RW's classic device interface has no blend-equation render state (SRCBLEND/DESTBLEND only choose
		// factors); blendState() supplies ADD as the base. A non-ADD equation, the FIX const, or the
		// premul is only reachable through the GS ALPHA register (setGsBlend).
		queue.rwBlend.src = src ? rwBlendToGL(src, "rwRENDERSTATESRCBLEND") : GL_SRC_ALPHA;
		queue.rwBlend.dst = dst ? rwBlendToGL(dst, "rwRENDERSTATEDESTBLEND") : GL_ONE_MINUS_SRC_ALPHA;
	}

	// RwStencilOperation -> GL stencil op. INCR/DECR wrap on RW; INCRSAT/DECRSAT clamp.
	// Fold the RW stencil render states into queue.stencil (GL enums). A substate the game never set reads
	// 0 from the zeroed state array, which is not a valid RW value; fall back to the RW/GL default
	// (KEEP / ALWAYS / full mask) via queue.rsHits so an unset op doesn't disable stencil writes.
	void Device::updateStencilFromRwState()
	{
		queue.stencil.enable = queue.renderState[rwRENDERSTATESTENCILENABLE] != 0;
		queue.stencil.sfail = queue.rsHits[rwRENDERSTATESTENCILFAIL] ? rwStencilOpToGL(queue.renderState[rwRENDERSTATESTENCILFAIL]) : GL_KEEP;
		queue.stencil.zfail = queue.rsHits[rwRENDERSTATESTENCILZFAIL] ? rwStencilOpToGL(queue.renderState[rwRENDERSTATESTENCILZFAIL]) : GL_KEEP;
		queue.stencil.zpass = queue.rsHits[rwRENDERSTATESTENCILPASS] ? rwStencilOpToGL(queue.renderState[rwRENDERSTATESTENCILPASS]) : GL_KEEP;
		queue.stencil.func = queue.rsHits[rwRENDERSTATESTENCILFUNCTION] ? rwStencilFuncToGL(queue.renderState[rwRENDERSTATESTENCILFUNCTION]) : GL_ALWAYS;
		queue.stencil.ref = static_cast<int>(queue.renderState[rwRENDERSTATESTENCILFUNCTIONREF]);
		queue.stencil.readMask = queue.rsHits[rwRENDERSTATESTENCILFUNCTIONMASK] ? queue.renderState[rwRENDERSTATESTENCILFUNCTIONMASK] : 0xFFu;
		queue.stencil.writeMask = queue.rsHits[rwRENDERSTATESTENCILFUNCTIONWRITEMASK] ? queue.renderState[rwRENDERSTATESTENCILFUNCTIONWRITEMASK] : 0xFFu;
	}

	bool isStencilState(unsigned s)
	{
		return s >= rwRENDERSTATESTENCILENABLE && s <= rwRENDERSTATESTENCILFUNCTIONWRITEMASK;
	}

	void Device::setRenderState(unsigned state, unsigned value)
	{
		if (state >= queue.renderState.size())
		{
			SKY2_ERROR(false, "RwRenderState index %u out of range (=0x%X)", state, value);
			return;
		}
		queue.renderState[state] = value;
		queue.rsHits[state]++;
		// TEXTUREADDRESS is both axes: the Sky driver keeps no combined field, it writes through to the
		// per-axis ones. Mirroring that means a draw just reads U and V, and any interleaving of the
		// three states resolves itself.
		if (state == rwRENDERSTATETEXTUREADDRESS)
		{
			for (const unsigned axis : {rwRENDERSTATETEXTUREADDRESSU, rwRENDERSTATETEXTUREADDRESSV})
			{
				queue.renderState[axis] = value;
				queue.rsHits[axis]++;
			}
		}
		if (state == rwRENDERSTATESRCBLEND || state == rwRENDERSTATEDESTBLEND)
		{
			updateBlendFromRwState();
		}
		if (isStencilState(state))
		{
			updateStencilFromRwState();
		}
		// A render state we have neither implemented nor consciously chosen to ignore must not be
		// silently dropped: crash with its name so it gets classified.
		SKY2_ERROR(rsHandling(state) != RS_UNHANDLED,
			"unhandled RwRenderState %u (%s) = 0x%X - classify it in rsHandling()",
			state, rsName(state), value);
	}

	// The game sets ALPHA_1 via RpSkyRenderStateSet: 0x44 normal, 0x48 additive, 0x42 subtractive.
	// The decode itself is gsAlphaToBlend, shared with the MatFX second pass (which gets its ALPHA_2
	// from the material data instead).
	void Device::setGsBlend(unsigned v)
	{
		GIFRegALPHA reg{};
		// RpSkyRenderStateSet passes A/B/C/D in the low byte and FIX in the top byte of the same 32
		// bits; FIX sits at bit 32 of the real register, so it moves up by 8.
		reg.U64 = (v & 0xFFu) | (static_cast<uint64_t>(v & 0xFF000000u) << 8);
		SKY2_TRACE_ONCE(v, "GS ALPHA v=0x%X (A=%u B=%u C=%u D=%u FIX=%u)", v, reg.A, reg.B, reg.C,
			reg.D, reg.FIX);
		queue.gsBlend = gsAlphaToBlend(reg);
		queue.gsBlend.active = true; // a real ALPHA_1 poke: gsBlend now wins over rwBlend
	}

	// Decode a GS TEST register into the alpha test we apply as a shader discard. Only AFAIL=KEEP
	// drops the pixel's color; the others (FB/ZB/RGB_ONLY) still write color, and the 2D pass has
	// no Z - so we only discard for ATE + AFAIL=KEEP. Otherwise the test is a no-op for what we
	// render (this was the title bug).
	void Device::setGsAtest(unsigned v)
	{
		GIFRegTEST reg{};
		reg.U64 = v; // the guest hook passes the low 32 bits, which is everything this register uses
		SKY2_TRACE_ONCE(v, "GS TEST v=0x%X (ATE=%u ATST=%u AREF=%u AFAIL=%u)", v, reg.ATE, reg.ATST,
			reg.AREF, reg.AFAIL);
		if (reg.ATE != 0 && reg.AFAIL != AFAIL_KEEP)
		{
			// AFAIL != KEEP (fail writes a mode-specific color/Z subset) is reproduced in the 3D pass by
			// drawOneMesh's two masked draws. The 2D pass has no Z, so it ignores AFAIL and draws all.
			SKY2_TRACE_ONCE(v, "alpha test AFAIL=%u (v=0x%X); the 3D pass splits color/Z writes", reg.AFAIL, v);
		}
		if (reg.DATE != 0)
		{
			SKY2_TRACE_ONCE(v, "GS DATE=1 DATM=%u (v=0x%X): the draw writes only where the frame "
								"buffer alpha %s zero",
				reg.DATM, v, reg.DATM ? "is not" : "is");
		}
		queue.gsAtest = gsTestToAtest(reg);
	}

	// mdlEffect FRAME write-mask: the alpha-only mask pass writes only the framebuffer alpha channel
	// (GS FBMSK masks RGB), then the composite pass restores full RGBA and blends through that alpha.
	void Device::setFrameMask(uint32_t fbmsk)
	{
		// A set FBMSK bit means do not write. glColorMask works per channel, so a byte that is neither
		// all-masked nor all-writable has no GL equivalent; report it rather than round it off.
		const auto channel = [&](unsigned shift) {
			const uint32_t byte = (fbmsk >> shift) & 0xFFu;
			if (byte != 0x00u && byte != 0xFFu)
			{
				SKY2_TRACE_ONCE(fbmsk, "GS FBMSK 0x%08X byte %u = 0x%02X is partial; glColorMask "
										"cannot express it, treating the channel as writable",
					fbmsk, shift / 8, byte);
			}
			return byte != 0xFFu;
		};
		queue.colorMask = {channel(0), channel(8), channel(16), channel(24)};
	}

	// GS CLAMP_1. The screen-distortion effect region-clamps the grabbed frame buffer to its visible
	// rect; the capture texture is the whole PS2 buffer, so GL_CLAMP_TO_EDGE would sample the black pad
	// outside that rect instead of smearing the edge.
	void Device::setGsClamp(uint64_t clamp1)
	{
		GIFRegCLAMP reg{};
		reg.U64 = clamp1;
		SKY2_TRACE_ONCE(clamp1, "GS CLAMP_1 = 0x%llX (WMS=%u WMT=%u u=[%u,%u] v=[%u,%u])",
			static_cast<unsigned long long>(clamp1), unsigned(reg.WMS), unsigned(reg.WMT),
			unsigned(reg.MINU), unsigned(reg.MAXU), unsigned(reg.MINV), unsigned(reg.MAXV));
		queue.gsClamp = GsClamp{};
		queue.gsClamp.reg = clamp1;
		if (!sky2::config().regionClamp)
		{
			return;
		}
		const int gsW = objects.frameBufferW, gsH = objects.frameBufferH;
		if (gsW <= 0 || gsH <= 0)
		{
			return; // no frame buffer registered yet, so there is nothing to normalize against
		}
		if (reg.WMS != CLAMP_REGION_CLAMP || reg.WMT != CLAMP_REGION_CLAMP)
		{
			// REGION_REPEAT reinterprets MINU/MINV as MSK/FIX, and a plain repeat over the padded
			// capture would wrap into the pad. Both are worse than leaving the clamp off.
			SKY2_TRACE_ONCE(clamp1, "GS CLAMP_1 mode WMS=%u WMT=%u unhandled, sampling unclamped",
				unsigned(reg.WMS), unsigned(reg.WMT));
			return;
		}
		// MAX* are inclusive texel indices, so the outer edge is one past them.
		queue.gsClamp.uvMin = {float(reg.MINU) / float(gsW), float(reg.MINV) / float(gsH)};
		queue.gsClamp.uvMax = {float(reg.MAXU + 1) / float(gsW), float(reg.MAXV + 1) / float(gsH)};
		queue.gsClamp.active = true;
	}

	void Device::restoreGsClamp() { queue.gsClamp = GsClamp{}; }

	// rpSKYRENDERSTATEFARFOGPLANE. Unlike the other Sky substates this one carries a pointer to the
	// RwReal rather than the value: the observed argument is an EE data address, and its bit pattern
	// read as a float is a denormal.
	void Device::setFarFogPlane(uint32_t valueAddr)
	{
		if (!valueAddr || !guest::ready())
		{
			return;
		}
		SKY2_TRACE_ONCE(valueAddr, "RpSkyRenderState FARFOGPLANE *0x%08X = %.2f (camera far plane "
									"otherwise)",
			valueAddr, *guest::ptr<float>(valueAddr));
		queue.farFogPlaneAddr = valueAddr;
	}

	// RpSkyRenderStateSet(substate, value): the game pokes GS registers (blend, alpha test) directly.
	void Device::setSkyRenderState(unsigned substate, unsigned value)
	{
		switch (substate)
		{
			case rpSKYRENDERSTATEALPHA_1: setGsBlend(value); break;
			case rpSKYRENDERSTATEATEST_1: setGsAtest(value); break;
			case rpSKYRENDERSTATENARENDERSTATE:
				break;
			case rpSKYRENDERSTATEDITHER:
				SKY2_TRACE_ONCE(value, "RpSkyRenderState DITHER value=%u (assumed inconsequential)", value);
				break;
			case rpSKYRENDERSTATEFARFOGPLANE: setFarFogPlane(value); break;
			case rpSKYRENDERSTATEMAXMIPLEVELS:
				SKY2_TRACE_ONCE(value, "RpSkyRenderState MAXMIPLEVELS value=%u (ignored; we upload a "
										"single level)",
					value);
				break;
			default:
				SKY2_ERROR(false, "unhandled RpSkyRenderState substate %u = 0x%X", substate, value);
		}
	}

	// Report which RwRenderStates the game set and whether we consume them, so ignored
	// state (fog, filter, z modes, etc.) is visible instead of silently dropped.
	void Device::logRenderStateUsage()
	{
		static const char* NAMES[] = {
			"NA", "TEXTURERASTER", "TEXTUREADDRESS", "TEXTUREADDRESSU", "TEXTUREADDRESSV",
			"TEXTUREPERSPECTIVE", "ZTESTENABLE", "SHADEMODE", "ZWRITEENABLE", "TEXTUREFILTER",
			"SRCBLEND", "DESTBLEND", "VERTEXALPHAENABLE", "BORDERCOLOR", "FOGENABLE", "FOGCOLOR",
			"FOGTYPE", "FOGDENSITY", "(18)", "(19)", "CULLMODE", "STENCILENABLE", "STENCILFAIL",
			"STENCILZFAIL", "STENCILPASS", "STENCILFUNCTION", "STENCILFUNCTIONREF",
			"STENCILFUNCTIONMASK", "STENCILFUNCTIONWRITEMASK",
			"ALPHATESTFUNCTION", "ALPHATESTFUNCTIONREF"};
		std::fprintf(stderr, "[sky2] RwRenderState usage (set by the game; * = consumed):\n");
		for (unsigned s = 0; s < queue.rsHits.size(); s++)
		{
			if (queue.rsHits[s] == 0)
			{
				continue;
			}
			const char* name = s < (sizeof(NAMES) / sizeof(NAMES[0])) ? NAMES[s] : "?";
			std::fprintf(stderr, "  %c %2u %-24s set %llu times, last=0x%X\n",
				rsConsumed(s) ? '*' : ' ', s, name,
				static_cast<unsigned long long>(queue.rsHits[s]), queue.renderState[s]);
		}
	}

	void Device::captureFrame(const char* path, void (*onStart)())
	{
		// Name the frame to capture rather than letting the render thread pick: see FrameRing::pinNextFrame.
		const uint64_t target = frames.pinNextFrame(onStart);
		std::lock_guard<std::mutex> lk(renderer.shotMutex);
		renderer.reqShotPath = path;
		renderer.reqShotIndex = target;
	}

	// Build a packed RwTexture.filterAddressing (filter | U<<8 | V<<12) for the Im2D/Im3D immediate
	// paths from the current render state (RW 5.6.6). The immediate path has no RwTexture to read, so
	// it honours the TEXTUREFILTER/TEXTUREADDRESS* states the game set, falling back to the prior
	// linear+clamp default when it set none.
	unsigned Device::im2dFilterAddressing()
	{
		const unsigned filter = queue.rsHits[rwRENDERSTATETEXTUREFILTER] ? queue.renderState[rwRENDERSTATETEXTUREFILTER] : rwFILTERLINEAR;
		// Both axes are kept current by setRenderState's write-through, so there is nothing to resolve.
		const auto axis = [&](unsigned a) {
			return queue.rsHits[a] ? queue.renderState[a] : unsigned(rwTEXTUREADDRESSCLAMP);
		};
		return (filter & 0xFFu) | ((axis(rwRENDERSTATETEXTUREADDRESSU) & 0xFu) << 8) |
		       ((axis(rwRENDERSTATETEXTUREADDRESSV) & 0xFu) << 12);
	}

} // namespace sky2

namespace sky2::guest
{
	// Declared in guest.h; forwards to sky2::reportError so the guest-memory overlay (used
	// before the sky2 subsystem is available, e.g. by p3fes.cpp) can report without depending on it.
	void reportOverflow(const char* fmt, ...)
	{
		char msg[512];
		va_list ap;
		va_start(ap, fmt);
		std::vsnprintf(msg, sizeof(msg), fmt, ap);
		va_end(ap);
		SKY2_ERROR(false, "%s", msg);
	}
} // namespace sky2::guest
