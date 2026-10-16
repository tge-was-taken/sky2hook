// Common sky2 render types, math helpers, and diagnostic macros shared by the device and the
// pipeline modules. Extracted from the old internal header; the cross-thread state stays there.
#pragma once

#include "sky2/api.h"
#include "sky2/guest.h"
#include "sky2/renderware.h"
#include "sky2/hooks.h"
#include "sky2/host.h"
#include "sky2/gs.h"
#include "sky2/addresses.h"
#include "sky2/config.h"
#include "sky2/env.h"

#include <glad/gl.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sky2
{
	// GPU skinning bone-matrix cap (UBO). 160*64 bytes = 10 KB, comfortably under the GL-guaranteed
	// minimum GL_MAX_UNIFORM_BLOCK_SIZE of 16 KB; bumped from 128 after a real character needed 135.
	constexpr int MAX_BONES = 160;
	constexpr int MAX_DIR_LIGHTS = 4; // directional-light cap (a world can have more than one)
	constexpr int MAX_LOCAL_LIGHTS = 8; // per-draw positioned-light cap (point/spot from sector ties)

	// A GL texture cache key. 64-bit so a decoded texture can be keyed by a content hash rather than
	// by the address its pixels happened to sit at; the top three bits are reserved to tag the keys
	// that are not content (render targets, framebuffer feedback, streaming pixels), so a hash is
	// masked below them.
	using TextureId = uint64_t;
	constexpr TextureId TEX_KEY_TAG_MASK = 0xE000000000000000ull;

	// Accumulate elapsed time into a counter. Always on: two clock reads per timed region is far below
	// the cost of what it measures, and a timer you have to enable is one you never have on when the
	// spike happens. Relaxed because a diagnostic read from the other thread only needs to not tear.
	struct ScopedNs
	{
		std::atomic<uint64_t>& sink;
		std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
		~ScopedNs()
		{
			const uint64_t ns = static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - t0)
					.count());
			sink.store(sink.load(std::memory_order_relaxed) + ns, std::memory_order_relaxed);
		}
	};

	inline void bumpStat(std::atomic<uint64_t>& c, uint64_t by = 1)
	{
		c.store(c.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
	}

	void reportError(const char* srcFile, int line, const char* func, const char* fmt, ...)
#ifdef __GNUC__
		__attribute__((format(printf, 4, 5)))
#endif
		;
} // namespace sky2

#define SKY2_ERROR(cond, ...) \
	do \
	{ \
		if (!(cond)) \
		{ \
			sky2::reportError(__FILE__, __LINE__, __func__, __VA_ARGS__); \
		} \
	} while (0)

#define SKY2_ASSERT(cond, ...) \
	do \
	{ \
		if (!(cond)) \
		{ \
			sky2::reportError(__FILE__, __LINE__, __func__, __VA_ARGS__); \
			std::abort(); \
		} \
	} while (0)

namespace sky2
{
	inline float ps2AlphaToUnit(unsigned rawAlpha)
	{
		if (rawAlpha > 0x80)
		{
			SKY2_ERROR(false, "rawAlpha 0x%X exceeds the PS2 0..0x80 range",
				rawAlpha);
		}
		return std::min(static_cast<float>(rawAlpha), 128.0f) / 128.0f;
	}

	// RwRGBA -> unit vec. divisor 255 = plain 8-bit; pass 128 for the PS2 0x80=1.0 convention.
	inline glm::vec4 toVec4(const RwRGBA& c, float divisor = 255.0f)
	{
		if (divisor == 0.0f)
		{
			SKY2_ERROR(false, "called with a zero divisor");
			divisor = 255.0f;
		}
		return glm::vec4(c.red, c.green, c.blue, c.alpha) / divisor;
	}
	inline glm::vec3 toVec3(const RwRGBA& c, float divisor = 255.0f)
	{
		if (divisor == 0.0f)
		{
			SKY2_ERROR(false, "called with a zero divisor");
			divisor = 255.0f;
		}
		return glm::vec3(c.red, c.green, c.blue) / divisor;
	}
	inline glm::vec4 toVec4(const RwRGBAReal& c) { return glm::vec4(c.red, c.green, c.blue, c.alpha); }

	// PS2 raster cFormat byte we decode (low nibble = pixel format; 0x20 = PAL8, 0x40 = PAL4).
	enum Ps2RasterFormat
	{
		ps2FMT_8888 = 0x05, // PSMCT32 (direct 32-bit, no palette) - not yet decoded
		ps2FMT_PAL8_8888 = 0x25, // PSMT8 + 8888 CLUT
		ps2FMT_PAL4_8888 = 0x45, // PSMT4 + 8888 CLUT
		ps2FMT_PAL4_1555 = 0x41 // PSMT4 + 1555 CLUT (16-bit entries)
	};

	// RpSkyRenderStateSet substates (rwcore.h). The game sets the GS ALPHA (blend) and TEST
	// (alpha test) registers DIRECTLY through these, bypassing the RW SRCBLEND/DESTBLEND states.
	enum RpSkyRenderState
	{
		rpSKYRENDERSTATENARENDERSTATE = 0,
		rpSKYRENDERSTATEDITHER = 1,
		rpSKYRENDERSTATEALPHA_1 = 2, // raw GS ALPHA register (blend equation)
		rpSKYRENDERSTATEATEST_1 = 3, // raw GS TEST register (alpha test)
		rpSKYRENDERSTATEFARFOGPLANE = 4,
		rpSKYRENDERSTATEMAXMIPLEVELS = 5
	};

// Log a printf-style message the first time this call site is reached (per-site static flag).
#define SKY2_WARN_ONCE(...) \
	do \
	{ \
		static bool warnedOnce_ = false; \
		if (!warnedOnce_) \
		{ \
			warnedOnce_ = true; \
			std::fprintf(stderr, __VA_ARGS__); \
		} \
	} while (0)

// Log a printf-style trace message the first time this call site sees a given `key` value, so
// enumerating a variable over a finite (but a priori unknown) set of options - e.g. every distinct
// GS ALPHA/TEST bit pattern the game actually sets - prints each value once instead of spamming
// every frame. Pure observability: stderr only, no exception log, no P3FES_STRICT abort (unlike
// SKY2_ERROR).
// The set is guarded: several of these sites are reachable from both the EE build thread and the
// render thread, and a concurrent insert corrupts the container rather than merely losing a message.
#define SKY2_TRACE_ONCE(key, ...) \
	do \
	{ \
		static std::mutex seenMutex_; \
		static std::unordered_set<std::decay_t<decltype(key)>> seenValues_; \
		std::unique_lock<std::mutex> seenLock_(seenMutex_); \
		const bool firstSeen_ = seenValues_.insert(key).second; \
		seenLock_.unlock(); \
		if (firstSeen_) \
		{ \
			std::fprintf(stderr, "[sky2][trace] %s(): ", __func__); \
			std::fprintf(stderr, __VA_ARGS__); \
			std::fprintf(stderr, "\n"); \
		} \
	} while (0)


	// One interleaved 3D vertex; the field layout matches the VBO attributes set in init3D.
	struct Vertex3D
	{
		glm::vec3 pos{0.0f};
		glm::vec3 normal{0.0f};
		glm::vec2 uv{0.0f};
		glm::vec4 color{1.0f}; // prelit color, 0..1 (PS2 0x80=1.0 scaling done in the shader)
		glm::vec4 boneIndex{0.0f}; // up to 4 bone indices, as floats
		glm::vec4 boneWeight{0.0f}; // matching bone weights
		glm::vec2 uv2{0.0f}; // second texcoord set (MatFX dual pass); 0 when the geom has 1 set
	};
	static_assert(sizeof(Vertex3D) == 22 * sizeof(float), "Vertex3D must be 22 floats");

	// One Im2D vertex, in the game's 640x448 screen space with unit UV and colour. The field order is
	// the VBO attribute layout (Im2DRenderer::init), so the arena uploads with no repacking.
	struct Im2DVertex
	{
		float x = 0.0f, y = 0.0f;
		// The GS depth the game submits. 2D on the PS2 shares the GS depth test with 3D, and games
		// clip UI panels by writing a Z barrier and drawing the contents behind it.
		float z = 0.0f;
		float u = 0.0f, v = 0.0f;
		float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
	};
	static_assert(sizeof(Im2DVertex) == 9 * sizeof(float), "Im2DVertex must be tightly packed");

	// GS/RW stencil test, snapshotted per draw (like blend) and applied on the render thread. Fields
	// hold GL enums; defaults are the RW/GL defaults (test off, ALWAYS pass, keep on every outcome).
	struct StencilState
	{
		bool enable = false;
		unsigned func = 0x0207; // GL_ALWAYS
		int ref = 0;
		unsigned readMask = 0xFFu; // glStencilFunc mask
		unsigned writeMask = 0xFFu; // glStencilMask
		unsigned sfail = 0x1E00; // GL_KEEP (stencil test failed)
		unsigned zfail = 0x1E00; // GL_KEEP (stencil passed, depth failed)
		unsigned zpass = 0x1E00; // GL_KEEP (stencil and depth passed)
		bool operator==(const StencilState&) const = default;
	};

	// The resolved per-draw blend a pipeline applies: GL equation/factors, the FIX constant alpha, the
	// shader-side premul, and whether this draw blends at all. Built by RenderQueue::blendState().
	struct Blend
	{
		unsigned equation = GL_FUNC_ADD; // GL_FUNC_ADD / SUBTRACT / REVERSE_SUBTRACT
		unsigned src = GL_SRC_ALPHA; // GL blend factors
		unsigned dst = GL_ONE_MINUS_SRC_ALPHA;
		float constAlpha = 1.0f; // glBlendColor alpha, when src/dst reference GL_CONSTANT_ALPHA (C=FIX)
		float alphaPremul = 1.0f; // shader-side output-alpha prescale some GS ALPHA combos need
		bool enable = false; // this draw blends (VERTEXALPHAENABLE / material / prelit alpha)
		bool operator==(const Blend&) const = default;
	};

	// RW SRCBLEND/DESTBLEND blend source: the classic device chooses GL factors only (equation is ADD).
	struct RwBlend
	{
		unsigned src = GL_SRC_ALPHA;
		unsigned dst = GL_ONE_MINUS_SRC_ALPHA;
	};

	// GS ALPHA register blend source (setGsBlend decode): a full blend spec that, when active, overrides
	// the RW factors on top of the RW base.
	struct GsBlend
	{
		unsigned equation = GL_FUNC_ADD; // GL_FUNC_ADD / SUBTRACT / REVERSE_SUBTRACT
		unsigned src = GL_SRC_ALPHA; // GL blend factors
		unsigned dst = GL_ONE_MINUS_SRC_ALPHA;
		float constAlpha = 1.0f; // glBlendColor alpha (C=FIX)
		float alphaPremul = 1.0f; // shader-side output-alpha prescale
		GIFRegALPHA reg{}; // decoded poke, diagnostics only
		bool active = false; // an ALPHA_1 poke set this; it overrides the RW factors
	};

	// GS alpha-test state (GS TEST register, decoded by setGsAtest), applied as a shader discard.
	struct GsAtest
	{
		bool on = false; // ATE set with AFAIL=KEEP: the only case we discard
		bool enabled = false; // GS TEST.ATE (kept regardless of AFAIL)
		int method = ATST_ALWAYS; // GS_ATST
		float ref = 0.0f; // 0..1
		int afail = AFAIL_KEEP; // GS_AFAIL
		bool date = false; // GS TEST.DATE: gate the write on the frame buffer's existing alpha
		bool datm = false; // GS TEST.DATM: the alpha value that passes (false = write where alpha is 0)
		GIFRegTEST reg{}; // decoded GS TEST register poke (diagnostics)
		// Hand-written because GIFRegTEST is a union, which blocks a defaulted comparison.
		bool operator==(const GsAtest& o) const
		{
			return on == o.on && enabled == o.enabled && method == o.method && ref == o.ref &&
			       afail == o.afail && reg.U64 == o.reg.U64;
		}
	};

	// The TEST register the RW PS2 driver leaves programmed: alpha below half writes colour but not Z,
	// which is how an alpha-blended surface avoids occluding what is behind it.
	inline GsAtest skyDefaultAtest()
	{
		GsAtest a;
		a.enabled = true;
		a.method = ATST_GEQUAL;
		a.ref = 64.0f / 255.0f;
		a.afail = AFAIL_FB_ONLY;
		a.reg.ATE = 1;
		a.reg.ATST = ATST_GEQUAL;
		a.reg.AREF = 64;
		a.reg.AFAIL = AFAIL_FB_ONLY;
		return a;
	}

	// Material + geometry-lighting state for a mesh (from RpMaterial + the geometry flags).
	struct MeshMaterial
	{
		glm::vec4 color; // material base color, 0..1
		std::optional<TextureId> texKey;
		// Packed RwTexture filterAddressing. Sampling lives here rather than on the texture because a
		// content-keyed texture is shared between rasters that may sample it differently.
		unsigned sampler = 0;
		bool lit; // rpGEOMETRYLIGHT set: lit regime (ambient + directional)
		bool hasNormals = false; // normals present: gates the directional term inside the lit regime
		bool modulate = false; // rpGEOMETRYMODULATEMATERIALCOLOR set and material color != white
		bool vcolor; // has prelit vertex colors: modulate by them
		bool unlit = false; // Im3D: no lighting, color = texture x vertex color (like Im2D)
		float surfAmbient = 1.0f; // RpMaterial surfaceProps: scales ambient/directional light so
		float surfDiffuse = 1.0f; // low-coefficient materials (floor) don't over-brighten
	};

	// RpMatFX effect: the base UV transform + an optional second blended pass.
	struct MeshMatFx
	{
		RpMatFXEffect effect = rpMATFXEFFECTNULL; // RpMatFX combined effect type
		bool enabled = false; // RpMatFXAtomicEnableEffects flag on the source atomic
		glm::vec3 uvRow0{1.0f, 0.0f, 0.0f}; // base UV transform rows (identity = passthrough)
		glm::vec3 uvRow1{0.0f, 1.0f, 0.0f};
		// The second pass: a DUAL overlay on the set-1 texcoords, or an ENVMAP reflection whose
		// texcoords come from the vertex normal. Blended with GS context 2's ALPHA_2.
		bool hasPass2 = false;
		std::optional<TextureId> pass2TexKey;
		GsBlend pass2Blend;
		bool pass2Env = false; // ENVMAP: generate texcoords rather than read set 1
		float pass2Coef = 1.0f; // ENVMAP reflection strength (RpMatFX coef)
		glm::vec3 dualUvRow0{1.0f, 0.0f, 0.0f}; // DUAL UV transform (set-1 texcoords)
		glm::vec3 dualUvRow1{0.0f, 1.0f, 0.0f};
	};

	// GS CLAMP_1 region clamp (mdlEffectDMAOpenSetClampVIFPkt). Bounds a sub-rect of the GS frame
	// buffer, which no GL wrap mode expresses: those clamp to the whole texture.
	struct GsClamp
	{
		bool active = false; // false = plain GL sampling, bit-identical to no clamp at all
		glm::vec2 uvMin{0.0f};
		glm::vec2 uvMax{1.0f};
		uint64_t reg = 0; // raw poke, diagnostics only
		bool operator==(const GsClamp&) const = default;
	};

	// GS/RW per-draw render state: blend, stencil, cull, depth, fog, alpha test, color mask.
	// Per-draw GS/RW state, shared by 3D meshes and Im2D batches. Interned into Frame::states, so a
	// draw carries an index rather than a copy.
	//
	// blend.enable selects the opaque/translucent partition for MESH draws only. Im2D never writes it
	// and always blends, so a shared table must not be used to partition 2D work.
	struct DrawState
	{
		Blend blend; // resolved per-draw blend (blend.enable gates blending)
		StencilState stencil; // RW stencil render states, applied per draw
		int cull = rwCULLMODECULLNONE; // RwCullMode (1 none, 2 back, 3 front)
		bool ztest = true; // rwRENDERSTATEZTESTENABLE
		bool zwrite = true; // rwRENDERSTATEZWRITEENABLE
		bool fog = false; // rwRENDERSTATEFOGENABLE
		glm::bvec4 colorMask{true}; // GS FRAME write mask (mdlEffect mask passes); default RGBA
		GsClamp clamp; // GS CLAMP_1 region clamp; inactive by default
		// The raw GS ALPHA register the game poked, kept beside the resolved blend so a capture can be
		// compared against the ground-truth GS trace register-for-register. 0 when the blend came from
		// the RW SRCBLEND/DESTBLEND states instead.
		uint64_t gsAlphaReg = 0;
		// GS alpha test captured at build time (frame-capture manifest; foundation for AFAIL handling).
		GsAtest atest;
		bool operator==(const DrawState&) const = default;
	};

	// Which pipeline built a draw. `atomicAddr` holds an RpAtomic address for Atomic and an
	// RpWorldSector address for Sector, so the tag is what tells them apart; Im3D synthesizes a draw
	// with no source object at all.
	enum class MeshSource : uint8_t
	{
		Atomic,
		Sector,
		Im3D,
	};

	// Which 2D funnel built a draw. Rt2d's leaves draw straight through the device rather than through
	// SkyIm2DRenderPrimitive, so the two arrive as the same struct and only this tells them apart.
	enum class Im2DSource : uint8_t
	{
		Im2D,
		Rt2d,
	};

	// Provenance for the capture manifest, and the identity a draw is matched by across frames.
	// Initialized because the Im3D path synthesizes a MeshDraw without a source atomic: left
	// indeterminate, those fields print recycled frame-pool bytes and make the manifest differ run to
	// run, which silently breaks it as a comparison reference.
	struct MeshDiag
	{
		uint32_t atomicAddr = 0;
		uint32_t geomFlags = 0;
		int numTexCoordSets = 0;
		uint32_t texCoordsAddr = 0;
		MeshSource source = MeshSource::Im3D;
		bool native = false; // parsed from the pre-instanced VIF table
		// Im3D only: the RwMatrixTag RwIm3DTransform named, and its flags. "The matrix is wrong" and
		// "the matrix is right and the vertices are wrong" are indistinguishable without the source.
		uint32_t ltmAddr = 0;
		uint32_t im3dFlags = 0;
		uint32_t materialKey = 0;
		uint32_t generation = 0;
		uint8_t rsCull = 0; // the RwCullMode the game asked for, which is not what we apply
		uint16_t drawOrdinal = 0;
		// P3FES_PIPE_DUMP: the pipelines this draw ran through, and the identity of the VU1
		// microprogram they upload. Two draws that differ only in colour either share a program or
		// they do not, and nothing else recorded per draw answers that.
		uint32_t objPipeline = 0;
		uint32_t matPipeline = 0;
		uint64_t objVuHash = 0;
		uint64_t matVuHash = 0;
	};

	// Immutable geometry buffers, shared (not copied) between the parsed-geometry cache and every
	// MeshDraw that references them. Their pointer identity is stable across frames for cached geometry,
	// which the render thread uses to key its GPU-resident VBO/EBO cache.
	// Handle to a buffer in the geometry pool: low 24 bits slot index, high 40 bits generation, 0 =
	// none. The generation is wide because per-frame geometry recycles a slot every frame: once it
	// wraps, a stale handle in the GPU residency cache matches a live slot and draws wrong geometry.
	using GeomId = uint64_t;
	constexpr GeomId GEOM_ID_NONE = 0;
	constexpr uint32_t GEOM_SLOT_BITS = 24;
	constexpr uint32_t GEOM_MAX_SLOTS = 1u << GEOM_SLOT_BITS;

	constexpr uint32_t geomSlot(GeomId id) { return static_cast<uint32_t>(id) & (GEOM_MAX_SLOTS - 1); }
	constexpr uint64_t geomGen(GeomId id) { return id >> GEOM_SLOT_BITS; }
	constexpr GeomId geomId(uint32_t slot, uint64_t gen) { return (gen << GEOM_SLOT_BITS) | slot; }

	// Scene lighting. Colors are RwRGBAReal (0..1); dir is toward the light. Gathered per object at its
	// render callback (gatherLights) because the game re-scopes the world's lights between draws.
	struct FrameLights
	{
		glm::vec3 ambient{0.4f, 0.4f, 0.4f};
		// A world can have more than one directional light; gatherLights fills [0, dirCount).
		std::array<glm::vec3, MAX_DIR_LIGHTS> dirColor{glm::vec3{0.6f, 0.6f, 0.6f}};
		std::array<glm::vec3, MAX_DIR_LIGHTS> dirDir{glm::vec3{0.0f, 1.0f, 0.0f}};
		int dirCount = 1;
		// Positioned lights (point/spot) from the object's sector ties. params = (radius, minusCosAngle,
		// light type, 0); axis is the spot direction.
		std::array<glm::vec3, MAX_LOCAL_LIGHTS> localColor{};
		std::array<glm::vec3, MAX_LOCAL_LIGHTS> localPos{};
		std::array<glm::vec3, MAX_LOCAL_LIGHTS> localAxis{};
		std::array<glm::vec4, MAX_LOCAL_LIGHTS> localParams{};
		int localCount = 0;
		glm::vec3 fogColor{0.5f, 0.5f, 0.5f}; // RwRenderState FOGCOLOR
		float fogStart{1.0f}; // camera fog plane
		float fogEnd{1000.0f}; // camera far plane
		// Where this lighting came from, so a capture can answer it without re-deriving the gather.
		// Compared like everything else: two objects lit by the same lights still share one entry.
		uint32_t worldAddr = 0; // the RpWorld gatherLights walked
		std::array<uint32_t, MAX_DIR_LIGHTS> dirAddr{}; // contributing RpLight per directional slot
		std::array<uint32_t, MAX_LOCAL_LIGHTS> localAddr{}; // per positioned slot
		bool operator==(const FrameLights&) const = default;
	};

	enum class CmdType : uint8_t
	{
		Mesh,
		Im2d
	};

	// One entry of the frame's command list. `data` indexes Frame::meshCmds or Frame::im2dCmds by type.
	struct FrameCmd
	{
		CmdType type = CmdType::Mesh;
		uint32_t data = 0;
	};

	struct MeshDraw
	{
		// Either the mesh owns pool buffers (verts/idx name them) or it lives in the frame's transient
		// arena (verts == GEOM_ID_NONE, and the four slice fields below name its span). The arena path
		// draws with a base vertex, so its indices stay mesh-local.
		GeomId verts = GEOM_ID_NONE;
		GeomId idx = GEOM_ID_NONE;
		uint32_t vertOffset = 0, vertCount = 0; // into Frame::transientVerts
		uint32_t idxOffset = 0, idxCount = 0; // into Frame::transientIdx
		glm::mat4 model; // object->world; the pass VP is applied on the render thread
		// Slice of the frame's bone arena (Frame::bones) holding this mesh's pose * invBind matrices.
		// boneCount 0 = not skinned. Every submesh of one object names the same slice rather than
		// carrying its own copy of the palette.
		uint32_t boneOffset = 0;
		uint16_t boneCount = 0;
		MeshMaterial material;
		MeshMatFx matfx;
		// Im3D can submit a line (RwIm3DRenderLine), which is two indices and not a degenerate
		// triangle. The GS has its own line primitive, so widening it into geometry would be a guess.
		bool lineList = false;
		uint32_t stateIndex = 0; // into Frame::states
		MeshDiag diag;
		uint32_t lightsIndex = 0; // into Frame::lights; every submesh of an object shares one entry
	};

	// One RwDevice Im2D primitive submission (SkyIm2DRenderPrimitive): screen-space triangles
	// with per-vertex UV + color, plus the bound texture (0 = flat color). Kept in submission
	// order so backgrounds draw under sprites/text. verts = 8 floats each (x,y in 640x448
	// source space, u, v, r, g, b, a in 0..1).
	// GS/RW render state carried per Im2D draw: blend, alpha test, color mask, stencil.
	struct Im2DDraw
	{
		std::optional<TextureId> texKey;
		unsigned sampler = 0; // packed filterAddressing; see MeshMaterial::sampler
		Im2DSource source = Im2DSource::Im2D;
		// What the game submitted, before expansion to triangles. A draw's own vertex count is the
		// expanded one, which cannot distinguish a 44-vertex fan (a disc) from an 86-vertex strip
		// (a ring): both land on a similar triangle count.
		uint16_t srcPrim = 0; // RwPrimitiveType
		uint16_t srcVerts = 0; // vertices the game submitted
		uint16_t srcIndices = 0; // indices, for the indexed entry point; 0 for the plain one
		uint32_t stateIndex = 0; // into Frame::states
		uint32_t vertOffset = 0; // slice of Frame::im2dVerts, in vertices
		uint32_t vertCount = 0;
		// Names which on-screen element this draw is, for interpolation. A 2D draw carries no source
		// object, and no host-side rule tells "the panel moved" apart from "the glyph list shifted", so
		// only draws a game-specific hook named are ever blended. 0 means unnamed: never blend.
		uint32_t interpTag = 0;
	};

	// Decoded textures pending GL upload (filled on the EE thread, uploaded on the
	// render thread), and the live GL texture cache keyed by guest pixel address.
	struct PendingTex
	{
		TextureId key;
		// The guest address the pixels were decoded from. Diagnostics only: content is keyed by hash,
		// so the same texture arrives from many addresses and this is just the most recent one. Carried
		// on the upload rather than read back from the EE-thread tables, which the render thread must
		// not touch.
		uint32_t srcPx = 0;
		int w;
		int h;
		std::vector<uint8_t> rgba;
		GLint wrapS = GL_REPEAT;
		GLint wrapT = GL_REPEAT;
		GLint minFilter = GL_LINEAR_MIPMAP_LINEAR;
		GLint magFilter = GL_LINEAR;
	};

	// What a camera pass clears its target with (RwCameraClear color/flags). Applied to the
	// pass's target FBO on the render thread; color/depth toggled by the clear flags.
	struct PassClear
	{
		bool color = false;
		bool depth = false;
		bool stencil = false;
		glm::vec4 colorValue{0.0f, 0.0f, 0.0f, 1.0f};
	};

	// One camera pass: everything drawn between an RwCameraBeginUpdate and the next Begin/present,
	// bound to one render target. The frame is an ordered list of these (submission order), so
	// interleaved 2D/3D passes and off-screen render targets layer correctly. Draws within a pass
	// run in submission order, except 3D atomics keep an opaque-before-translucent partition (the
	// game submits them mixed and relies on the pipeline for that).
	struct CameraParts
	{
		glm::mat4 camWorld{1.0f};
		float viewWindowX = 0.0f, viewWindowY = 0.0f;
		float nearPlane = 0.0f, farPlane = 0.0f;
		int projectionType = 0;
		bool valid = false;
	};

	struct FramePass
	{
		uint32_t target = 0; // render-target raster addr (camera frameBuffer); 0 = unknown
		uint32_t camera = 0; // the RwCamera this pass renders from (for the debug overlay)
		int targetW = 0; // target raster size (for the off-screen FBO); 0 = unknown
		int targetH = 0;
		bool isDisplay = false; // this target reaches the screen (resolved against gDisplayRasters)
		glm::mat4 vp{1.0f};
		CameraParts cam;
		FrameLights lights;
		PassClear clear;
		// This pass's slice of Frame::commandList. Submission order is pass-contiguous, which is what
		// makes Frame::drawOrder a permutation within the range rather than a global sort.
		uint32_t cmdBegin = 0;
		uint32_t cmdEnd = 0;
	};

	// A texture id that samples an off-screen render target's color attachment rather than a decoded
	// PS2 raster. Tagged with the high bit so it never collides with a real EE pixel address (EE RAM
	// is 32 MB, addresses < 0x02000000). The low bits are the target raster's guest address.
	constexpr TextureId RENDER_TARGET_TEX_BIT = 0x8000000000000000ull;
	inline bool isRenderTargetTexId(TextureId id) { return (id & RENDER_TARGET_TEX_BIT) != 0; }
	// Keys the renderer owns rather than decodes: render targets and the feedback snapshots. Their GL
	// textures are created here with their own clamp/filter, so a sampler object must not override it.
	// Streaming keys are decoded like any other texture and are deliberately not in this set.
	constexpr TextureId RENDERER_OWNED_TEX_MASK = 0xC000000000000000ull;
	inline bool isRendererOwnedTexId(TextureId id) { return (id & RENDERER_OWNED_TEX_MASK) != 0; }

	// Pixels that keep changing at one address (an FMV frame, an animated overlay). Keying those by
	// content makes every frame a new cache entry that nothing will ever sample again, so they are
	// keyed by address instead and overwrite one GL texture. The low bits are that pixel address.
	constexpr TextureId STREAMING_TEX_BIT = 0x2000000000000000ull;
	inline bool isStreamingTexId(TextureId id) { return (id & STREAMING_TEX_BIT) != 0; }

	// A texture id for a screen-effect draw that binds the current draw buffer as a texture (the game's
	// framebuffer-feedback pattern: RpSkyGetDrawBufferRaster -> rwRENDERSTATETEXTURERASTER -> fullscreen
	// Im2D). The render thread samples a snapshot of the display FBO instead of decoding PS2 pixels.
	// Fixed value, not a raster address, so it never collides with a real key.
	constexpr TextureId DRAW_BUFFER_FEEDBACK_TEX_ID = 0x4000000000000000ull;

	// The same idea for the other half of the double buffer: a draw that binds the raster being
	// displayed wants the frame already presented, not the one being built, so it samples a copy of the
	// display taken at the previous present. Captured once per frame, and only if a draw asks for it.
	constexpr TextureId DISPLAY_BUFFER_FEEDBACK_TEX_ID = 0x4000000000000001ull;

	// Either half of the double buffer sampled as a texture. The GS CLAMP_1 region is in frame-buffer
	// texel space, so it applies to these two keys and to nothing else.
	inline bool isFrameBufferFeedbackTexId(TextureId id)
	{
		return id == DRAW_BUFFER_FEEDBACK_TEX_ID || id == DISPLAY_BUFFER_FEEDBACK_TEX_ID;
	}

	// Draw isolation: gate rendering to one draw (drawOnly) or a 0..N prefix (drawUpto) by its unified
	// per-frame draw index; the render walk threads a running base index through the passes.
	inline bool drawEnabled(int index)
	{
		const Config& o = sky2::config();
		if (!o.drawSkip.empty() && o.drawSkip.count(index))
		{
			return false;
		}
		if (o.drawOnly >= 0)
		{
			return index == o.drawOnly;
		}
		if (o.drawUpto >= 0)
		{
			return index <= o.drawUpto;
		}
		return true;
	}

	constexpr bool isLinePrimitive(uint32_t primType)
	{
		return primType == rwPRIMTYPELINELIST || primType == rwPRIMTYPEPOLYLINE;
	}

	// Expand a line primitive over `count` items (list = disjoint pairs, polyline = consecutive),
	// calling emit(i0, i1) per segment.
	template <typename Emit>
	void expandLinePrimitive(uint32_t primType, int count, Emit emit)
	{
		const int step = (primType == rwPRIMTYPELINELIST) ? 2 : 1;
		for (int i = 0; i + 1 < count; i += step)
		{
			emit(i, i + 1);
		}
	}

	// Expand a triangle primitive over `count` items (list/strip/fan), calling emit(i) for each triangle
	// corner in winding order. `emit` maps an item index to a vertex (directly, or through an index
	// buffer). Shared by the Im2D and Im3D paths.
	template <typename Emit>
	void expandTrianglePrimitive(const char* pipeline, uint32_t primType, int count, Emit emit)
	{
		if (primType == rwPRIMTYPETRILIST)
		{
			for (int i = 0; i + 2 < count; i += 3)
			{
				emit(i);
				emit(i + 1);
				emit(i + 2);
			}
		}
		else if (primType == rwPRIMTYPETRISTRIP)
		{
			for (int i = 0; i + 2 < count; i++)
			{
				if (i & 1)
				{
					emit(i + 1);
					emit(i);
					emit(i + 2);
				} // keep winding
				else
				{
					emit(i);
					emit(i + 1);
					emit(i + 2);
				}
			}
		}
		else if (primType == rwPRIMTYPETRIFAN)
		{
			for (int i = 1; i + 1 < count; i++)
			{
				emit(0);
				emit(i);
				emit(i + 1);
			}
		}
		else
		{
			SKY2_ERROR(false, "%s primType %u unhandled (count=%d)", pipeline, primType, count);
		}
	}
} // namespace sky2
