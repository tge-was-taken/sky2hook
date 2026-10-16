#include "sky2/hooks.h"

#include "sky2/addresses.h"
#include "sky2/api.h"
#include "sky2/env.h"
#include "sky2/gs.h" // FBMSK_* write masks
#include "sky2/guest.h" // read the RwPluginRegistry at plugin registration
#include "sky2/host.h"
#include "sky2/renderware.h" // rwRENDERSTATETEXTURERASTER

#include <algorithm>
#include <map>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sky2::hooks
{
	namespace
	{
		bool s_probeOn = false;
		std::unordered_map<uint32_t, uint64_t> s_probeCounts; // callee -> call count
	} // namespace
	// Count one render entry-point call. Cheap no-op unless the probe is enabled; the render handlers
	// call this so already-hooked paths are counted without a second (overwriting) hook registration.
	void probeBump(uint32_t callee)
	{
		if (s_probeOn)
			s_probeCounts[callee]++;
	}

	hooks::Result im2dRenderPrimitive(hooks::Context& c)
	{
		sky2::api::im2dRenderPrimitive(c.a0, c.a1, static_cast<int>(c.a2));
		return hooks::Result::Continue;
	}
	hooks::Result im2dRenderTriangle(hooks::Context& c)
	{
		sky2::api::im2dRenderTriangle(c.a0, static_cast<int>(c.a1), static_cast<int>(c.a2),
			static_cast<int>(c.a3), static_cast<int>(c.t0));
		return hooks::Result::Continue;
	}
	hooks::Result im2dRenderLine(hooks::Context& c)
	{
		sky2::api::im2dRenderLine(c.a0, static_cast<int>(c.a1), static_cast<int>(c.a2), static_cast<int>(c.a3));
		return hooks::Result::Continue;
	}
	hooks::Result im2dRenderIndexed(hooks::Context& c)
	{
		sky2::api::im2dRenderIndexedPrimitive(c.a0, c.a1, static_cast<int>(c.a2), c.a3, static_cast<int>(c.t0));
		return hooks::Result::Continue;
	}
	hooks::Result skyCameraBeginUpdate(hooks::Context& c)
	{
		// P3FES_LOG_RT already prints each pass's target; the EE caller is what names the subsystem
		// that opened it, which a raster address alone never does.
		if (static const bool sLogRt = sky2::env("LOG_RT") != nullptr; sLogRt)
		{
			static std::set<uint32_t> seen;
			if (seen.insert(c.ra).second)
			{
				std::fprintf(stderr, "[eesym] camera begin-update from %s (camera 0x%X)\n",
					sky2::host().resolveSymbol(c.ra).c_str(), c.a1);
			}
		}
		sky2::api::cameraBeginUpdate(c.a1); // fn(NULL, camera, 0): $a1 = camera
		return hooks::Result::Continue;
	}
	hooks::Result skyCameraEndUpdate(hooks::Context&)
	{
		sky2::api::cameraEndUpdate(); // pass commit: finalize vp/lights
		return hooks::Result::Continue;
	}
	hooks::Result rwCameraClear(hooks::Context& c)
	{
		// RwCameraClear(camera, RwRGBA*, flags). The camera matters: a clear names the raster it
		// clears, which is not always the pass being built.
		sky2::api::cameraClearColor(c.a0, c.a1, static_cast<int>(c.a2));
		return hooks::Result::Continue;
	}
	hooks::Result skySetRenderState(hooks::Context& c)
	{
		// Binding a frame buffer as the texture is the framebuffer-feedback tap: trace who does it.
		if (c.a0 == rwRENDERSTATETEXTURERASTER && sky2::api::isFrameBufferRaster(c.a1))
		{
			sky2::api::traceEeChain("bind as texture", c.a1, c.ra, c.sp);
		}
		sky2::api::setRenderState(c.a0, c.a1);
		return hooks::Result::Continue;
	}
	hooks::Result rpSkyRenderStateSet(hooks::Context& c)
	{
		sky2::api::setSkyRenderState(c.a0, c.a1);
		return hooks::Result::Continue;
	}
	// The rwSTANDARD device functions are RwStandardFunc(void *pOut, void *pInOut, RwInt32 nIn), so the
	// raster is the SECOND argument. Reading a0 passed the unused pOut, which is always 0: raster
	// destroys silently evicted nothing and the texture cache never released anything.
	hooks::Result skyRasterCreate(hooks::Context& c)
	{
		sky2::api::rasterCreate(c.a1);
		return hooks::Result::Continue;
	}
	hooks::Result skyRasterUnlock(hooks::Context& c)
	{
		sky2::api::rasterUnlock(c.a1);
		return hooks::Result::Continue;
	}
	hooks::Result skyRasterUnlockPalette(hooks::Context& c)
	{
		sky2::api::rasterUnlockPalette(c.a1);
		return hooks::Result::Continue;
	}
	hooks::Result skyRasterDestroy(hooks::Context& c)
	{
		sky2::api::rasterDestroy(c.a1);
		return hooks::Result::Continue;
	}
	hooks::Result sceMpegGetPicture(hooks::Context& c)
	{
		sky2::api::setMovieFrame(c.a1);
		return hooks::Result::Continue;
	}
	// The draw context has to span the *guest* callback, not our render of it: this is a pre-hook, so
	// the guest body that builds the DMA packet runs after we return, and that is what reaches the VIF.
	// Pushing here and popping in the post-hook is what lets a VU1 upload name the draw that caused it.
	bool drawContextArmed()
	{
		static const bool on = sky2::env("VU_TRACE") != nullptr;
		return on;
	}
	void drawContextPop(hooks::Context&) { sky2::host().popDraw(); }

	hooks::Result atomicRenderCallback(hooks::Context& c)
	{
		probeBump(c.pc);
		sky2::api::renderAtomic(c.a0);
		if (drawContextArmed())
			sky2::host().pushDraw("atomic", c.a0, 0);
		return hooks::Result::Continue;
	}
	hooks::Result sectorRenderCallback(hooks::Context& c)
	{
		probeBump(c.pc);
		sky2::api::renderWorldSector(c.a0);
		if (drawContextArmed())
			sky2::host().pushDraw("sector", c.a0, 0);
		return hooks::Result::Continue;
	}
	// Im3D funnel (A2): Transform(verts, n, ltm, flags) -> RenderPrimitive(primType) -> End.
	hooks::Result im3dTransform(hooks::Context& c)
	{
		probeBump(c.pc);
		sky2::api::im3dTransform(c.a0, static_cast<int>(c.a1), c.a2, c.a3);
		return hooks::Result::Continue;
	}
	hooks::Result im3dRenderLine(hooks::Context& c)
	{
		probeBump(c.pc);
		sky2::api::im3dRenderLine(static_cast<int>(c.a0), static_cast<int>(c.a1));
		return hooks::Result::Continue;
	}
	hooks::Result im3dRenderPrimitive(hooks::Context& c)
	{
		probeBump(c.pc);
		sky2::api::im3dRenderPrimitive(c.a0);
		return hooks::Result::Continue;
	}
	hooks::Result im3dRenderIndexed(hooks::Context& c)
	{
		probeBump(c.pc);
		sky2::api::im3dRenderIndexedPrimitive(c.a0, c.a1, static_cast<int>(c.a2));
		return hooks::Result::Continue;
	}
	hooks::Result im3dEnd(hooks::Context&)
	{
		sky2::api::im3dEnd();
		return hooks::Result::Continue;
	}
	hooks::Result rt2dPathFill(hooks::Context& c)
	{
		probeBump(c.pc);
		sky2::api::rt2dPathFill(c.a0, c.a1);
		return hooks::Result::Continue;
	}
	void rt2dPathFillPost(hooks::Context&)
	{
		sky2::api::rt2dDumpGuestCtm();
	}
	hooks::Result rt2dPathStroke(hooks::Context& c)
	{
		probeBump(c.pc);
		sky2::api::rt2dPathStroke(c.a0, c.a1);
		return hooks::Result::Continue;
	}
	hooks::Result rt2dFontShow(hooks::Context& c)
	{
		probeBump(c.pc);
		sky2::api::rt2dFontShow(c.a0, c.a1, c.freg(12), c.a2, c.a3);
		return hooks::Result::Continue;
	}
	// PDS G3x Clone: instrumentation only. Each entry point renders a cloned copy of already-instanced
	// geometry, distinguished from its sibling by a matrix and (for the OPL variant) a flat colour,
	// both consumed before the pipeline runs. Dump them and decide what the family is from the data.
	hooks::Result cloneProbe(hooks::Context& c)
	{
		probeBump(c.pc);
		static const bool log = sky2::env("CLONE_LOG") != nullptr;
		if (!log)
		{
			return hooks::Result::Continue;
		}
		static std::unordered_set<uint64_t> seen; // per entry point + call site
		if (seen.insert((static_cast<uint64_t>(c.pc) << 32) | c.ra).second)
		{
			std::fprintf(stderr, "[clone] %s a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X <- %s\n",
				sky2::host().resolveSymbol(c.pc).c_str(), c.a0, c.a1, c.a2, c.a3,
				sky2::host().resolveSymbol(c.ra).c_str());
			// A matrix is four rows of four floats with the last row a position. Dumping each pointer
			// argument that lands in EE RAM is enough to recognise one by eye.
			for (const uint32_t arg : {c.a0, c.a1, c.a2, c.a3})
			{
				if (arg < 0x00100000 || arg >= guest::RAM_SIZE)
				{
					continue;
				}
				std::fprintf(stderr, "         [0x%08X]", arg);
				for (int i = 0; i < 16; i++)
				{
					std::fprintf(stderr, " %.3f", static_cast<float>(*guest::ptr<float>(arg + i * 4u)));
				}
				std::fprintf(stderr, "\n");
			}
		}
		return hooks::Result::Continue;
	}
	// mdlEffect screen-effect state: the game emits raw GIF packets for the GS FRAME write mask and
	// CLAMP_1, bypassing RpSkyRenderStateSet, so each entry point is hooked directly. The masks are
	// absolute stores, not a stack, so the restore is a plain assignment.
	hooks::Result mdlEffectPushAlphaMask(hooks::Context&)
	{
		sky2::api::setFrameMask(FBMSK_ALPHA_ONLY);
		return hooks::Result::Continue;
	}
	hooks::Result mdlEffectPushColorMask(hooks::Context&)
	{
		sky2::api::setFrameMask(FBMSK_COLOR_ONLY);
		return hooks::Result::Continue;
	}
	hooks::Result mdlEffectRestoreFrameMask(hooks::Context&)
	{
		sky2::api::setFrameMask(FBMSK_NONE);
		return hooks::Result::Continue;
	}
	// The CLAMP_1 value needs 44 bits, so $a0 has to be read as a full 64-bit register.
	hooks::Result mdlEffectSetClamp(hooks::Context& c)
	{
		sky2::api::setGsClamp(c.reg64(4));
		return hooks::Result::Continue;
	}
	hooks::Result mdlEffectRestoreClamp(hooks::Context&)
	{
		sky2::api::restoreGsClamp();
		return hooks::Result::Continue;
	}
	hooks::Result skyShowRaster(hooks::Context& c)
	{
		sky2::api::showRaster(c.a0);
		return hooks::Result::Continue;
	}
	// Public RpSky entry points we don't reimplement yet. Hooked as passthrough probes: fail-log once
	// per function when the game actually calls one, so unhandled Sky behavior stays visible instead of
	// silently running only on the emulated GS side. The original always runs (Continue).
	namespace
	{
		struct RpSkyProbe
		{
			uint32_t addr;
			const char* name;
		};
		const char* rpSkyName(uint32_t addr)
		{
			// Built on first call, after setAddresses (addresses are zero before that).
			static const RpSkyProbe probes[] = {
				{sky2::addresses().RpSkySelectDeepZBuffer, "RpSkySelectDeepZBuffer"},
				{sky2::addresses().RpSkySelectTrueTSClipper, "RpSkySelectTrueTSClipper"},
				{sky2::addresses().RpSkySelectTrueTLClipper, "RpSkySelectTrueTLClipper"},
				{sky2::addresses().RpSkyTextureSetMipmapK, "RpSkyTextureSetMipmapK"},
				{sky2::addresses().RpSkyGetDrawBufferRaster, "RpSkyGetDrawBufferRaster"},
				{sky2::addresses().RpSkyTexCacheAccessSpeculate, "RpSkyTexCacheAccessSpeculate"},
				{sky2::addresses().RpSkyTexCacheAccessRaster, "RpSkyTexCacheAccessRaster"},
				{sky2::addresses().RpSkyTexCacheReleaseRaster, "RpSkyTexCacheReleaseRaster"},
				{sky2::addresses().RpSkyTexCacheRasterLock, "RpSkyTexCacheRasterLock"},
				{sky2::addresses().RpSkyTexCacheRestore, "RpSkyTexCacheRestore"},
				{sky2::addresses().RpSkyTexGetTex0, "RpSkyTexGetTex0"},
				{sky2::addresses().RpSkyTexCalcTex1, "RpSkyTexCalcTex1"},
			};
			for (const RpSkyProbe& p : probes)
				if (p.addr == addr)
					return p.name;
			return "RpSky?";
		}
	} // namespace

	// RpSkyTexCacheAccessRaster(raster, useContext2) is the only thing that programs the GS's second
	// drawing context, and the context is not a game-visible render state, so the callers are the only
	// way to learn which draws land there. One line per distinct (caller, context).
	hooks::Result rpSkyTexCacheAccessRaster(hooks::Context& c)
	{
		static const bool log = sky2::env("TEXCACHE_LOG") != nullptr;
		if (log)
		{
			static std::unordered_set<uint64_t> seen;
			if (seen.insert((static_cast<uint64_t>(c.ra) << 1) | (c.a1 & 1)).second)
			{
				std::fprintf(stderr, "[texcache] ctx%u raster=0x%08X <- %s\n", (c.a1 & 1) + 1, c.a0,
					sky2::host().resolveSymbol(c.ra).c_str());
			}
		}
		return hooks::Result::Continue;
	}

	hooks::Result rpSkyUnhandled(hooks::Context& c)
	{
		static std::unordered_set<uint32_t> seen;
		if (seen.insert(c.pc).second)
			std::fprintf(stderr, "[rpsky][UNHANDLED] %s (0x%08X): a0=0x%X a1=0x%X a2=0x%X <- ra=0x%X\n",
				rpSkyName(c.pc), c.pc, c.a0, c.a1, c.a2, c.ra);
		return hooks::Result::Continue; // passthrough: the original still runs
	}

	// The RpSkyTexCache family allocates the GS's 4 MB of texture memory and computes the TEX0/TEX1
	// registers that address it. sky2 uploads textures to GL and holds them there, so none of it has
	// a counterpart to mirror; the emulated driver still runs and still keeps the GS coherent. Logged
	// once each so the family is visibly accounted for rather than absent.
	// RpSkyTexCacheAccessRaster is the exception and has its own handler: its useContext2 argument is
	// the only signal of which GS drawing context a draw lands in.
	hooks::Result rpSkyGsTextureMemory(hooks::Context& c)
	{
		static std::unordered_set<uint32_t> seen;
		if (seen.insert(c.pc).second)
			std::fprintf(stderr, "[rpsky] %s: GS texture memory, not mirrored (sky2 holds textures in GL)\n",
				rpSkyName(c.pc));
		return hooks::Result::Continue;
	}

	// RpSkyRenderStateGet(substate, ...) reads Sky state back. Passthrough is correct on its own (the
	// original still owns the state sky2 only mirrors), so this logs rather than fails, once per
	// substate: a read of a state sky2 reinterprets (ALPHA_1/ATEST_1) is worth seeing.
	hooks::Result rpSkyRenderStateGet(hooks::Context& c)
	{
		static std::unordered_set<uint32_t> seenSubstates;
		if (seenSubstates.insert(c.a0).second)
			std::fprintf(stderr, "[rpsky][read] RpSkyRenderStateGet(substate=%u) <- ra=0x%X\n", c.a0, c.ra);
		return hooks::Result::Continue;
	}

	// The RpSkyGet*BufferRaster getters return a GS frame buffer in $v0. Record it either way: a draw
	// binding a frame buffer as a texture samples the display FBO (framebuffer feedback), whichever of
	// the pair it is. The source label is what identifies a raster's role in the log.
	void recordBufferGetter(const char* name, int role, uint32_t raster, uint32_t c_ra, uint32_t c_sp)
	{
		static std::unordered_set<uint64_t> seen; // per getter + raster: the pair can alternate on a flip
		if (seen.insert((static_cast<uint64_t>(reinterpret_cast<uintptr_t>(name)) << 32) ^ raster).second)
			std::fprintf(stderr, "[rpsky] %s -> raster 0x%X\n", name, raster);
		sky2::api::registerFrameBufferRaster(raster, role, name);
		sky2::api::traceEeChain(name, raster, c_ra, c_sp);
	}
	void rpSkyGetDrawBufferRaster(hooks::Context& c)
	{
		recordBufferGetter("draw-buffer getter", 2, c.v0, c.ra, c.sp);
	}
	void rpSkyGetDisplayBufferRaster(hooks::Context& c)
	{
		recordBufferGetter("display-buffer getter", 1, c.v0, c.ra, c.sp);
	}

	// RW plugin registration. At entry, reg->sizeOfStruct (0x00) is the offset this plugin will get (RW
	// assigns offset = sizeOfStruct, then grows it), and reg->origSizeOfStruct (0x04) is the object's
	// base size, a stable per-type key. Discovery pass: log (baseSize, pluginID) -> offset so the
	// per-serial plugin data offsets can stop being hardcoded.
	hooks::Result pluginRegistryAddPlugin(hooks::Context& c)
	{
		if (!c.a0)
			return hooks::Result::Continue;
		const uint32_t offset = *guest::ptr<uint32_t>(c.a0 + 0x00); // sizeOfStruct at entry = this offset
		const uint32_t baseSize = *guest::ptr<uint32_t>(c.a0 + 0x04); // origSizeOfStruct = object type
		sky2::recordPluginOffset(baseSize, c.a2, offset);
		if (sky2::env("PLUGINLOG"))
			std::fprintf(stderr, "[pluginlog] baseSize=0x%-4X pluginID=0x%-6X offset=0x%X\n",
				baseSize, c.a2, offset);
		return hooks::Result::Continue;
	}

	std::span<const Entry> table()
	{
		// Built on first call, after setAddresses (addresses are zero before that).
		static const std::vector<Entry> t = {
			{sky2::addresses().SkyIm2DRenderPrimitive, &im2dRenderPrimitive, true},
			{sky2::addresses().SkyIm2DRenderTriangle, &im2dRenderTriangle, true},
			{sky2::addresses().SkyIm2DRenderLine, &im2dRenderLine, true},
			{sky2::addresses().SkyIm2DRenderIndexedPrimitive, &im2dRenderIndexed, true},
			{sky2::addresses().SkyCameraBeginUpdate, &skyCameraBeginUpdate, true},
			{sky2::addresses().SkyCameraEndUpdate, &skyCameraEndUpdate, true},
			{sky2::addresses().RwCameraClear, &rwCameraClear, true},
			{sky2::addresses()._rwSkySetRenderState, &skySetRenderState, true},
			{sky2::addresses().RpSkyRenderStateSet, &rpSkyRenderStateSet, true},
			{sky2::addresses().SkyRasterCreate, &skyRasterCreate, true},
			{sky2::addresses().SkyRasterUnlock, &skyRasterUnlock, true},
			{sky2::addresses().SkyRasterUnlockPalette, &skyRasterUnlockPalette, true},
			{sky2::addresses().SkyRasterDestroy, &skyRasterDestroy, true},
			{sky2::addresses().sceMpegGetPicture, &sceMpegGetPicture, true},
			{sky2::addresses().AtomicDefaultRenderCallBack, &atomicRenderCallback, true,
				drawContextArmed() ? &drawContextPop : nullptr},
			{sky2::addresses()._rpSectorDefaultRenderCallBack, &sectorRenderCallback, true,
				drawContextArmed() ? &drawContextPop : nullptr},
			{sky2::addresses().RwIm3DTransform, &im3dTransform, true},
			{sky2::addresses().RwIm3DRenderPrimitive, &im3dRenderPrimitive, true},
			{sky2::addresses().RwIm3DRenderIndexedPrimitive, &im3dRenderIndexed, true},
			{sky2::addresses().RwIm3DRenderLine, &im3dRenderLine, true},
			{sky2::addresses().RwIm3DEnd, &im3dEnd, true},
			// The post-hook only exists to print the guest's own CTM next to the composed one, and every
			// post-hook costs a rewritten return address per call. Fill runs thousands of times a frame,
			// so enable it only when the diagnostic is asked for.
			{sky2::addresses()._rt2dPS2PathFill, &rt2dPathFill, true,
				sky2::env("RT2D_LOG") ? &rt2dPathFillPost : nullptr},
			{sky2::addresses()._rt2dPS2PathStroke, &rt2dPathStroke, true},
			{sky2::addresses()._rt2dPS2FontShow, &rt2dFontShow, true},
			{sky2::addresses().mdlEffectDMAOpenPushAlphaOnlyMaskVIFPkt, &mdlEffectPushAlphaMask, true},
			{sky2::addresses().mdlEffectDMAOpenPushColorOnlyMaskVIFPkt, &mdlEffectPushColorMask, true},
			// Two pop entry points emit the same packet; both restore full RGBA.
			{sky2::addresses().mdlEffectDMAOpenEmitPopFrameMaskPkt, &mdlEffectRestoreFrameMask, true},
			{sky2::addresses().mdlEffectDMAOpenPopFrameMaskVIFPkt, &mdlEffectRestoreFrameMask, true},
			{sky2::addresses().mdlEffectDMAOpenSetClampVIFPkt, &mdlEffectSetClamp, true},
			{sky2::addresses().mdlEffectDMAOpenRestoreClampVIFPkt, &mdlEffectRestoreClamp, true},
			{sky2::addresses().SkyRasterShowRaster, &skyShowRaster, true},
			{sky2::addresses()._rwPluginRegistryAddPlugin, &pluginRegistryAddPlugin, false},
			// RpSky probes: log once on first call, passthrough. rpSkyUnhandled means unclassified.
			{sky2::addresses().RpSkyRenderStateGet, &rpSkyRenderStateGet, true},
			{sky2::addresses().RpSkySelectDeepZBuffer, &rpSkyUnhandled, true},
			{sky2::addresses().RpSkySelectTrueTSClipper, &rpSkyUnhandled, true},
			{sky2::addresses().RpSkySelectTrueTLClipper, &rpSkyUnhandled, true},
			{sky2::addresses().RpSkyTextureSetMipmapK, &rpSkyUnhandled, true},
			{sky2::addresses().RpSkyGetDrawBufferRaster, nullptr, true, &rpSkyGetDrawBufferRaster},
			{sky2::addresses().RpSkyGetDisplayBufferRaster, nullptr, true, &rpSkyGetDisplayBufferRaster},
			{sky2::addresses().RpSkyTexCacheAccessSpeculate, &rpSkyGsTextureMemory, true},
			{sky2::addresses().RpSkyTexCacheAccessRaster, &rpSkyTexCacheAccessRaster, true},
			{sky2::addresses().RpSkyTexCacheReleaseRaster, &rpSkyGsTextureMemory, true},
			{sky2::addresses().RpSkyTexCacheRasterLock, &rpSkyGsTextureMemory, true},
			{sky2::addresses().RpSkyTexCacheRestore, &rpSkyUnhandled, true},
			{sky2::addresses().RpSkyTexGetTex0, &rpSkyGsTextureMemory, true},
			{sky2::addresses().RpSkyTexCalcTex1, &rpSkyGsTextureMemory, true},
		};
		return t;
	}

	hooks::Handler handlerFor(uint32_t callee)
	{
		if (!callee)
			return nullptr; // 0 is "absent from this serial's toml", not a callee
		for (const Entry& e : table())
			if (e.address == callee)
				return e.handler;
		return nullptr;
	}

	// Passthrough hook for the render entry points sky2 does not otherwise hook: count and continue.
	hooks::Result probePassthrough(hooks::Context& c)
	{
		probeBump(c.pc);
		return hooks::Result::Continue;
	}

	// P3FES_TRACE_CALLERS=<hex>[,<hex>...]: passthrough hooks that print each distinct caller of the
	// listed EE addresses, resolved through the disc's symbol map, with the arguments it passed.
	// "Which subsystem opens this pass" is a recurring question and a raster address never answers it;
	// the immediate caller is often a thin library wrapper, so trace that wrapper in turn.
	// Counted per call site, not just seen once: "who calls this" and "how often" are different
	// questions, and a site printed once reads as a site called once.
	std::map<uint64_t, uint64_t> s_callerCounts;

	hooks::Result callerTrace(hooks::Context& c)
	{
		const uint64_t key = (uint64_t(c.pc) << 32) | c.ra;
		if (s_callerCounts[key]++ == 0)
		{
			std::fprintf(stderr, "[eetrace] %s <- %s (a0=0x%X a1=0x%X a2=0x%X a3=0x%X)\n",
				sky2::host().resolveSymbol(c.pc).c_str(), sky2::host().resolveSymbol(c.ra).c_str(),
				c.a0, c.a1, c.a2, c.a3);
		}
		return hooks::Result::Continue;
	}

	// Render entry points sky2 does not otherwise hook (the hooked ones self-count via probeBump).
	// Passthrough hooks here let the probe see the whole render funnel without disturbing rendering.
	// Per-serial like every other address: hardcoded literals counted whatever code happened to live at
	// a SLUS-21621 address on some other disc, which made the counts on those discs meaningless.
	std::span<const Entry> probeTable()
	{
		static const std::vector<Entry> t = [] {
			std::vector<Entry> v = {
				{sky2::addresses().RxPipelineExecute, &probePassthrough, false}, // chokepoint: total render count
				{sky2::addresses().rpPDS_G3x_ADLCloneRenderMtx, &cloneProbe, false},
				{sky2::addresses().rpPDS_G3x_OPLCloneRenderMtxRGBA, &cloneProbe, false},
			};
			if (const char* spec = sky2::env("TRACE_CALLERS"))
			{
				for (const char* p = spec; *p;)
				{
					char* end = nullptr;
					const uint32_t addr = static_cast<uint32_t>(std::strtoul(p, &end, 16));
					if (end == p)
						break;
					if (addr)
						v.push_back({addr, &callerTrace, false});
					p = (*end == ',') ? end + 1 : end;
				}
			}
			return v;
		}();
		return s_probeOn ? std::span<const Entry>(t) : std::span<const Entry>();
	}

	void enableProbe()
	{
		s_probeOn = sky2::env("NO_HOOK_PROBE") == nullptr; // on by default; opt out for CI/parallel
		if (s_probeOn)
			sky2::host().loadSymbols();
	}

	void reportProbe(std::FILE* out)
	{
		if (!s_probeOn)
			return;
		std::vector<std::pair<uint32_t, uint64_t>> rows(s_probeCounts.begin(), s_probeCounts.end());
		std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		if (!s_callerCounts.empty())
		{
			std::fprintf(out, "[eetrace] call-site counts:\n");
			for (const auto& [key, n] : s_callerCounts)
				std::fprintf(out, "  %-8llu %s <- %s\n", static_cast<unsigned long long>(n),
					sky2::host().resolveSymbol(uint32_t(key >> 32)).c_str(),
					sky2::host().resolveSymbol(uint32_t(key)).c_str());
		}
		std::fprintf(out, "[hookprobe] render entry-point call counts:\n");
		for (const auto& r : rows)
			std::fprintf(out, "  %-8llu %s\n", static_cast<unsigned long long>(r.second),
				sky2::host().resolveSymbol(r.first).c_str());
	}
} // namespace sky2::hooks
