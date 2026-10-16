// One handler per hooked guest function. Each handler translates a decoded hooks::Context into a
// call on the renderer, so there is no central dispatch switch: the host binds each address to its
// handler. Emulator-agnostic, by construction: a handler touches only the plain-integer Context.
#pragma once

#include <cstdint>
#include <cstdio>
#include <span>

#include "sky2/host.h"

namespace sky2::hooks
{
	enum class Result
	{
		Continue,
		Exit // skip the guest function body
	};

	// The register file at a hooked call, as named references. The host hands over a flat view of the
	// EE register file: 32 GPRs of four 32-bit words each (the 128-bit stride), and 32 FPRs.
	struct Regs
	{
		uint32_t* gpr;
		float* fpr;
		uint32_t pc;
	};

	struct Context
	{
		explicit Context(const Regs& r)
			: regs(r)
			, a0(r.gpr[4 * 4])
			, a1(r.gpr[5 * 4])
			, a2(r.gpr[6 * 4])
			, a3(r.gpr[7 * 4])
			, t0(r.gpr[8 * 4])
			, t1(r.gpr[9 * 4])
			, t2(r.gpr[10 * 4])
			, t3(r.gpr[11 * 4])
			, sp(r.gpr[29 * 4])
			, ra(r.gpr[31 * 4])
			, v0(r.gpr[2 * 4])
			, pc(r.pc)
		{
		}

		uint32_t& reg(int n) { return regs.gpr[n * 4]; }
		float& freg(int n) { return regs.fpr[n]; }

		// By value rather than a reference through a cast: the file has a 128-bit stride and no
		// 8-byte alignment guarantee.
		uint64_t reg64(int n) const
		{
			return uint64_t(regs.gpr[n * 4]) | (uint64_t(regs.gpr[n * 4 + 1]) << 32);
		}

		Regs regs;
		uint32_t &a0, &a1, &a2, &a3, &t0, &t1, &t2, &t3, &sp, &ra, &v0;
		uint32_t pc;
	};

	using Handler = Result (*)(Context&);
	using PostHandler = void (*)(Context&);

	struct Entry;

	// Per-function handlers, one-to-one with the engine's Sky/RW render entry points. The host
	// registers them; it may also register entries of its own alongside.
	hooks::Result im2dRenderPrimitive(hooks::Context& c);
	hooks::Result im2dRenderTriangle(hooks::Context& c);
	hooks::Result im2dRenderLine(hooks::Context& c);
	hooks::Result im2dRenderIndexed(hooks::Context& c);
	hooks::Result skyCameraBeginUpdate(hooks::Context& c);
	hooks::Result skyCameraEndUpdate(hooks::Context& c);
	hooks::Result rwCameraClear(hooks::Context& c);
	hooks::Result skySetRenderState(hooks::Context& c);
	hooks::Result rpSkyRenderStateSet(hooks::Context& c);
	hooks::Result skyRasterCreate(hooks::Context& c);
	hooks::Result skyRasterUnlock(hooks::Context& c);
	hooks::Result skyRasterDestroy(hooks::Context& c);
	hooks::Result sceMpegGetPicture(hooks::Context& c);
	hooks::Result atomicRenderCallback(hooks::Context& c);
	hooks::Result sectorRenderCallback(hooks::Context& c);
	hooks::Result im3dTransform(hooks::Context& c);
	hooks::Result im3dRenderPrimitive(hooks::Context& c);
	hooks::Result im3dRenderIndexed(hooks::Context& c);
	hooks::Result im3dEnd(hooks::Context& c);
	hooks::Result rt2dPathFill(hooks::Context& c);
	void rt2dPathFillPost(hooks::Context& c);
	hooks::Result rt2dPathStroke(hooks::Context& c);
	hooks::Result rt2dFontShow(hooks::Context& c);
	hooks::Result cloneProbe(hooks::Context& c);
	hooks::Result mdlEffectPushAlphaMask(hooks::Context& c);
	hooks::Result mdlEffectPushColorMask(hooks::Context& c);
	hooks::Result mdlEffectRestoreFrameMask(hooks::Context& c); // shared by both pop entry points
	hooks::Result mdlEffectSetClamp(hooks::Context& c);
	hooks::Result mdlEffectRestoreClamp(hooks::Context& c);
	hooks::Result skyShowRaster(hooks::Context& c);
	hooks::Result rpSkyTexCacheAccessRaster(hooks::Context& c); // P3FES_TEXCACHE_LOG: who uses context 2
	hooks::Result pluginRegistryAddPlugin(hooks::Context& c); // discover plugin data offsets at registration

	// The address-to-handler wiring, the single source of truth for what each hooked address does.
	// `render` is true for the render handlers, so a host can enable them only when the renderer is
	// active; render=false handlers enable whenever hooks are enabled.
	struct Entry
	{
		uint32_t address;
		hooks::Handler handler; // pre-hook (runs at entry)
		bool render;
		hooks::PostHandler post = nullptr; // optional post-hook (runs after the original; c.v0 = return)
	};
	// Built on first call, so setAddresses() must have run. A zero address is one the host's
	// address table does not carry, and the host skips those.
	std::span<const Entry> table();

	// Look up a handler by callee address (used by offline replay, which has no hook registry).
	hooks::Handler handlerFor(uint32_t callee);

	// Render-coverage probe: count calls to every RxPipelineExecute caller, so a scene's render paths
	// and any the renderer still does not hook are visible. On unless SKY2_NO_HOOK_PROBE is set.
	// Non-destructive: the already-hooked render handlers self-count; `probeTable()` only adds
	// passthrough hooks on the unhooked entry points. The host registers probeTable() and reports.
	void enableProbe();
	std::span<const Entry> probeTable();
	void reportProbe(std::FILE* out);
} // namespace sky2::hooks
