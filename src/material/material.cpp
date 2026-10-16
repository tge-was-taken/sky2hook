#include "material.h"
#include <cstring>

#include <string>

#include "common.h"
#include "object_manager.h"
#include "render_queue.h"
#include "renderer.h"
#include "rw_to_gl.h"
#include "texture_manager.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace sky2
{
	// ---- MatFX (a material-scope concern; moved here from the atomic pipeline) --------------------

	// RW white-material fast path: the material colour modulates shading only when the flag
	// is set AND the colour is not opaque white; "flag set, colour white" behaves like "flag clear".
	static bool materialModulates(uint32_t geomFlags, const RwRGBA& c)
	{
		return (geomFlags & rpGEOMETRYMODULATEMATERIALCOLOR) && c.packed() != 0xFFFFFFFFu;
	}

	// A material's MatFX, reduced to what sky2 consumes: the effect type, the base UV transform, and
	// the second textured pass, which is either a DUAL overlay on the set-1 texcoords or an ENVMAP
	// whose texcoords come from the normal. Bump is absent in P3.
	struct MatFX
	{
		RpMatFXEffect effect = rpMATFXEFFECTNULL; // combined effect type
		bool uvXform = false; // UV/DUALUV: base UV transform present (non-null matrix)
		// 2D affine on the base texcoords: u' = u*r.x + v*u_.x + p.x, v' = u*r.y + v*u_.y + p.y
		float uvR[2] = {1.0f, 0.0f}, uvU[2] = {0.0f, 1.0f}, uvP[2] = {0.0f, 0.0f};
		bool dual = false; // DUAL/DUALUV: second texture present
		uint32_t dualTexture = 0; // RwTexture owning the dual raster
		int dualSrc = 0, dualDst = 0; // RwBlendFunction, the portable API's dual blend
		uint64_t dualAlpha = 0; // the GS ALPHA_2 the driver programs for the second pass
		bool dualXform = false; // DUALUV: dual UV transform present (applies to set-1 texcoords)
		float dR[2] = {1.0f, 0.0f}, dU[2] = {0.0f, 1.0f}, dP[2] = {0.0f, 0.0f};
		bool env = false; // ENVMAP: reflection pass, texcoords generated from the vertex normal
		uint32_t envTexture = 0; // RwTexture owning the environment raster
		uint32_t envFrame = 0; // RwFrame the reflection is anchored to (0 = camera space)
		float envCoef = 1.0f; // scales the reflection's contribution
	};

	// The MatFX UV transform packs a 2D affine into an RwMatrix (right/up rows + pos translation).
	static void readUvAffine(uint32_t matrixAddr, float r[2], float u[2], float p[2])
	{
		const auto m = guest::ptr<RwMatrixTag>(matrixAddr);
		r[0] = m->right.x;
		r[1] = m->right.y;
		u[0] = m->up.x;
		u[1] = m->up.y;
		p[0] = m->pos.x;
		p[1] = m->pos.y;
	}

	static MatFX readMatFX(uint32_t materialAddr)
	{
		MatFX fx;
		const uint32_t moff = sky2::pluginOffset(rwMATERIALBASE, rwID_MATERIALEFFECTSPLUGIN, sky2::addresses().MatFXMaterialDataOffset);
		if (!moff)
		{
			return fx; // MatFX plugin not attached
		}
		const uint32_t matDataAddr = *guest::ptr<uint32_t>(materialAddr + moff);
		if (!matDataAddr)
		{
			return fx; // this material carries no MatFX data
		}
		const auto matData = guest::ptr<RpMatFXMaterialData>(matDataAddr);
		fx.effect = static_cast<RpMatFXEffect>(matData->effect);
		if (sky2::env("MATFX_DUMP"))
		{
			// One line per distinct material: the words the PDS callbacks copy into the packet are in
			// here somewhere, and the GS trace says which value to look for.
			SKY2_TRACE_ONCE(matDataAddr, "MatFX data 0x%08X effect=%d", matDataAddr, matData->effect);
			static std::unordered_set<uint32_t> dumped;
			if (dumped.insert(matDataAddr).second)
			{
				for (int i = 0; i < 36; i += 6)
				{
					std::fprintf(stderr, "  w%02d:", i);
					for (int j = i; j < i + 6 && j < 36; j++)
						std::fprintf(stderr, " %08X", *guest::ptr<uint32_t>(matDataAddr + j * 4u));
					std::fprintf(stderr, "\n");
				}
			}
		}
		for (const RpMatFXSlot& slot : matData->slot)
		{
			// Only the slot owning the second texture carries a precomputed ALPHA_2; the UV-transform
			// slot leaves it zero, so read it from the slot that produces the pass.
			const uint64_t slotAlpha = uint64_t(slot.gsAlphaLo) | (uint64_t(slot.gsAlphaFix) << 32);
			switch (slot.type)
			{
				case rpMATFXEFFECTUVTRANSFORM: // word[0]=baseTransform, word[1]=dualTransform
					if (slot.word[0])
					{
						fx.uvXform = true;
						readUvAffine(slot.word[0], fx.uvR, fx.uvU, fx.uvP);
					}
					if (slot.word[1])
					{
						fx.dualXform = true;
						readUvAffine(slot.word[1], fx.dR, fx.dU, fx.dP);
					}
					break;
				case rpMATFXEFFECTDUAL: // word[0]=texture, word[1]=srcBlend, word[2]=dstBlend
					fx.dual = true;
					fx.dualTexture = slot.word[0];
					fx.dualSrc = static_cast<int>(slot.word[1]);
					fx.dualDst = static_cast<int>(slot.word[2]);
					fx.dualAlpha = slotAlpha;
					break;
				case rpMATFXEFFECTENVMAP: // word[0]=frame, word[1]=texture, word[2]=coef, word[3]=useFBAlpha
					fx.env = sky2::config().matfxEnvmap;
					fx.envFrame = slot.word[0];
					fx.envTexture = slot.word[1];
					std::memcpy(&fx.envCoef, &slot.word[2], sizeof(float));
					fx.dualAlpha = slotAlpha;
					break;
				default:
					break;
			}
		}
		return fx;
	}

	static bool isUvTransformEffect(RpMatFXEffect e)
	{
		return e == rpMATFXEFFECTUVTRANSFORM || e == rpMATFXEFFECTDUALUVTRANSFORM;
	}

	// The blend for the second pass. The PS2 driver programs GS context 2 from ALPHA_2 in the slot, so
	// that register is what the hardware actually used; the portable RwBlendFunction pair is only a
	// fallback for a material whose slot carries no precomputed register (none seen so far).
	static GsBlend pass2Blend(const MatFX& fx)
	{
		if (fx.dualAlpha)
		{
			GIFRegALPHA reg{};
			reg.U64 = fx.dualAlpha;
			return gsAlphaToBlend(reg);
		}
		SKY2_TRACE_ONCE(fx.effect, "MatFX effect %d has no ALPHA_2; using the RwBlendFunction pair", fx.effect);
		GsBlend b;
		b.src = fx.env ? GL_ONE : rwBlendToGL(static_cast<unsigned>(fx.dualSrc), "MatFX dual srcBlend");
		b.dst = fx.env ? GL_ONE : rwBlendToGL(static_cast<unsigned>(fx.dualDst), "MatFX dual dstBlend");
		return b;
	}

	// Read a material's MatFX into a SubMesh. Records the effect type for diagnostics; for an
	// atomic-enabled effect (RW two-level gate) installs the base UV transform and the second pass.
	// Bump is unused in P3.
	static void applyMatFX(SubMesh& sm, uint32_t materialAddr, bool atomicEnabled, TextureManager& textures)
	{
		const MatFX fx = readMatFX(materialAddr);
		sm.matfxEffect = fx.effect;
		if (!atomicEnabled)
		{
			return;
		}
		if (fx.uvXform && isUvTransformEffect(fx.effect))
		{
			sm.uvRow0 = glm::vec3(fx.uvR[0], fx.uvU[0], fx.uvP[0]);
			sm.uvRow1 = glm::vec3(fx.uvR[1], fx.uvU[1], fx.uvP[1]);
		}
		if (fx.dual || fx.env)
		{
			sm.pass2TexKey = textures.convertTexture(fx.dual ? fx.dualTexture : fx.envTexture);
			sm.hasPass2 = sm.pass2TexKey.has_value();
			sm.pass2Blend = pass2Blend(fx);
			sm.pass2Env = fx.env;
			sm.pass2Coef = fx.envCoef;
			if (fx.dualXform)
			{
				sm.dualUvRow0 = glm::vec3(fx.dR[0], fx.dU[0], fx.dP[0]);
				sm.dualUvRow1 = glm::vec3(fx.dR[1], fx.dU[1], fx.dP[1]);
			}
			// RW anchors the reflection to a frame; every P3 envmap material leaves it null, which
			// means camera space. A non-null frame would need its LTM to reach the shader.
			if (fx.envFrame)
			{
				SKY2_TRACE_ONCE(fx.envFrame,
					"MatFX envmap frame 0x%08X is not null; the reflection still uses camera space",
					fx.envFrame);
			}
		}
	}

	// ---- material pipeline families --------------------------------------------------------------

	// One class of PDS material pipe: how it turns a material into draw state. Today it owns configure()
	// (the material read); the shader split will add its own program + draw().
	struct MaterialPipeline
	{
		virtual ~MaterialPipeline() = default;
		virtual void configure(SubMesh& sm, guest::ptr<RpMaterial> material, const MaterialContext& ctx,
			TextureManager& textures) const = 0;
	};

	// Generic lit material: base colour/texture/blend, optional modulate + MatFX (dual/UV) from the
	// plugin, surface props. Covers default/SkSpt/Skin/Grp in P3FES (same material read; those differ
	// only in object-scope skinning/batching, which the object pipeline owns). MatFX (dual/UV) rides the
	// default material pipe as plugin data, so reading it here matches the game.
	struct GenericMaterialPipeline : MaterialPipeline
	{
		void configure(SubMesh& sm, guest::ptr<RpMaterial> material, const MaterialContext& ctx,
			TextureManager& textures) const override
		{
			sm.materialBaseColor = getMaterialColor(material);
			sm.texKey = convertMaterialTextureRaster(material, textures, sm.sampler);
			sm.blend = materialBlends(sm.materialBaseColor);
			sm.modulate = materialModulates(ctx.geomFlags, sm.materialBaseColor);
			if (ctx.applyEffects)
			{
				applyMatFX(sm, material.addr, ctx.matfxAtomicEnabled, textures);
			}
			readSurfaceProps(material, sm);
		}
	};

	static const GenericMaterialPipeline sGeneric;

	// ---- PDS pipe id + probe ---------------------------------------------------------------------

	// The PDS pipe id lives in RxPipeline.pluginData (offset 0x30). pluginId (0x2c) is a separate plugin
	// registration id, not the PDS id (verified against the decomp RxPipeline layout + RpPDSPipeID enum).
	static constexpr uint32_t RX_PIPELINE_PDS_ID_OFFSET = 0x30;

	struct PdsPipeName
	{
		uint32_t id;
		const char* name;
	};
	static const PdsPipeName PDS_PIPE_NAMES[] = {
#include "pds_pipe_ids.inc"
	};

	static const char* pdsPipeName(uint32_t id)
	{
		for (const PdsPipeName& e : PDS_PIPE_NAMES)
		{
			if (e.id == id)
			{
				return e.name;
			}
		}
		return "?";
	}

	// The PDS pipe id an RxPipeline* carries, or 0 for a null pipeline (the RW default/fallback slot).
	static uint32_t pdsPipeId(uint32_t pipelinePtr)
	{
		return pipelinePtr ? *guest::ptr<uint32_t>(pipelinePtr + RX_PIPELINE_PDS_ID_OFFSET) : 0;
	}

	struct MatBucket
	{
		int count = 0;
		int atomicCount = 0;
		int sectorCount = 0;
		int skinnedCount = 0; // owning atomic was skinned
		int matfxCount = 0; // materials carrying MatFX plugin data
		uint32_t sampleTexture = 0;
		int sampleEffect = -1; // RpMatFXMaterialData.effect, -1 if none
	};

	struct ObjBucket
	{
		int count = 0;
		int atomicCount = 0;
		int sectorCount = 0;
		int skinnedCount = 0;
		int noDrawCount = 0; // renders that produced no draw at all
		// Read when the bucket is created, not at the dump: the dump runs at process exit, by which
		// time guest RAM is gone.
		std::string nodes;
	};

	// The probe's accumulators (P3FES_LOG_MATPIPE), written only from the EE build thread and dumped at
	// process exit.
	struct MatPipeStats
	{
		std::map<uint32_t, MatBucket> matBuckets; // keyed by material pipe id
		std::map<uint32_t, ObjBucket> objBuckets; // keyed by object (atomic/sector) pipe id
		std::set<uint32_t> warnedPipes; // unregistered material pipe ids already logged
		bool dumpRegistered = false;
	};

	static MatPipeStats sStats;

	static bool loggingEnabled()
	{
		static const bool on = sky2::env("LOG_MATPIPE") != nullptr;
		return on;
	}

	static void ensureDumpRegistered()
	{
		if (!sStats.dumpRegistered)
		{
			sStats.dumpRegistered = true;
			std::atexit(dumpMaterialPipelines);
		}
	}

	// Read the MatFX combined effect for a material (RpMatFXMaterialData.effect), or -1 if the plugin
	// data is absent. A read-only slice of readMatFX, for the probe's bucketing.
	static int readMatfxEffect(uint32_t materialAddr)
	{
		const uint32_t moff = sky2::pluginOffset(rwMATERIALBASE, rwID_MATERIALEFFECTSPLUGIN, sky2::addresses().MatFXMaterialDataOffset);
		if (!moff)
		{
			return -1;
		}
		const uint32_t matDataAddr = *guest::ptr<uint32_t>(materialAddr + moff);
		if (!matDataAddr)
		{
			return -1;
		}
		return static_cast<int>(guest::ptr<RpMatFXMaterialData>(matDataAddr)->effect);
	}

	static void recordMaterialPipeline(guest::ptr<RpMaterial> material, uint32_t geomFlags, bool fromAtomic,
		bool skinned)
	{
		if (!loggingEnabled() || !material)
		{
			return;
		}
		ensureDumpRegistered();
		const uint32_t id = pdsPipeId(material->pipeline.addr);
		const int effect = readMatfxEffect(material.addr);
		MatBucket& b = sStats.matBuckets[id];
		if (b.count == 0)
		{
			b.sampleTexture = material->texture.addr;
			b.sampleEffect = effect;
		}
		b.count++;
		(fromAtomic ? b.atomicCount : b.sectorCount)++;
		if (skinned)
		{
			b.skinnedCount++;
		}
		if (effect >= 0)
		{
			b.matfxCount++;
		}
		(void)geomFlags;
	}

	uint32_t pdsPipeIdOf(uint32_t pipelinePtr)
	{
		return pdsPipeId(pipelinePtr);
	}

	std::string pipelineNodeNames(uint32_t pipelinePtr)
	{
		if (!pipelinePtr)
		{
			return "<none>";
		}
		// RxPipeline: numNodes at +0x04, nodes at +0x08; RxPipelineNode is 40 bytes with its
		// RxNodeDefinition first, whose name is the leading char*.
		const uint32_t numNodes = *guest::ptr<uint32_t>(pipelinePtr + 0x04);
		const uint32_t nodes = *guest::ptr<uint32_t>(pipelinePtr + 0x08);
		if (!nodes || numNodes == 0 || numNodes > 64)
		{
			return "<no nodes>";
		}
		std::string out;
		for (uint32_t i = 0; i < numNodes; i++)
		{
			const uint32_t def = *guest::ptr<uint32_t>(nodes + i * 40);
			const uint32_t name = def ? *guest::ptr<uint32_t>(def) : 0;
			out += out.empty() ? "" : " -> ";
			if (!name)
			{
				out += "?";
				continue;
			}
			for (uint32_t c = 0; c < 48; c++)
			{
				const char ch = static_cast<char>(*guest::ptr<uint8_t>(name + c));
				if (!ch)
				{
					break;
				}
				out += ch;
			}
		}
		return out;
	}

	void recordObjectPipeline(uint32_t pipelinePtr, bool isSector, bool skinned)
	{
		if (!loggingEnabled())
		{
			return;
		}
		ensureDumpRegistered();
		ObjBucket& b = sStats.objBuckets[pdsPipeId(pipelinePtr)];
		b.count++;
		(isSector ? b.sectorCount : b.atomicCount)++;
		if (skinned)
		{
			b.skinnedCount++;
		}
		if (b.nodes.empty())
		{
			b.nodes = pipelineNodeNames(pipelinePtr);
		}
	}

	// An object render that ended without a draw. Counted per pipe id so the inventory shows which
	// pipeline the drops belong to, which is the question "how many draws did we miss" cannot answer.
	void recordObjectNoDraw(uint32_t pipelinePtr)
	{
		if (!loggingEnabled())
		{
			return;
		}
		ensureDumpRegistered();
		ObjBucket& b = sStats.objBuckets[pdsPipeId(pipelinePtr)];
		b.noDrawCount++;
		if (b.nodes.empty())
		{
			b.nodes = pipelineNodeNames(pipelinePtr);
		}
	}

	void dumpMaterialPipelines()
	{
		if (!sStats.objBuckets.empty())
		{
			std::fprintf(stderr, "[matpipe] object pipelines: %zu distinct pipe ids\n", sStats.objBuckets.size());
			std::fprintf(stderr, "  %-38s %6s %6s %6s %6s %7s  %s\n", "pipeId : name", "count", "atomic",
				"sector", "skin", "noDraw", "nodes");
			for (const auto& [id, b] : sStats.objBuckets)
			{
				char idname[64];
				std::snprintf(idname, sizeof(idname), "0x%X %s", id, pdsPipeName(id));
				std::fprintf(stderr, "  %-38s %6d %6d %6d %6d %7d  %s\n",
					idname, b.count, b.atomicCount, b.sectorCount, b.skinnedCount, b.noDrawCount,
					b.nodes.c_str());
			}
		}
		if (!sStats.matBuckets.empty())
		{
			std::fprintf(stderr, "[matpipe] material pipelines: %zu distinct pipe ids\n", sStats.matBuckets.size());
			std::fprintf(stderr, "  %-38s %6s %6s %6s %6s %6s\n",
				"pipeId : name", "count", "atomic", "sector", "skin", "matfx");
			for (const auto& [id, b] : sStats.matBuckets)
			{
				char idname[64];
				std::snprintf(idname, sizeof(idname), "0x%X %s", id, pdsPipeName(id));
				std::fprintf(stderr, "  %-38s %6d %6d %6d %6d %6d\n",
					idname, b.count, b.atomicCount, b.sectorCount, b.skinnedCount, b.matfxCount);
			}
		}
	}

	// ---- registry + entry point ------------------------------------------------------------------

	// Log a material pipe id we do not specialize (once each): a greppable list of what a future scene
	// hit that might need its own family. Bounded (one line per distinct id).
	static void warnUnregisteredPipeOnce(uint32_t id)
	{
		if (sStats.warnedPipes.insert(id).second)
		{
			std::fprintf(stderr, "[matpipe] material pipe 0x%X %s not specialized: using generic\n",
				id, pdsPipeName(id));
		}
	}

	// Registry: PDS material pipe id -> family. All P3FES material pipes map to generic today (MatFX
	// rides the default pipe as plugin data; SkSpt/SkSptUva share the generic read). Distinct families
	// slot in here as the shader split needs them.
	static const MaterialPipeline& pipelineForId(uint32_t id)
	{
		switch (id)
		{
			case 0x00000: // rwPDS_G3_Generic (default / null pipeline)
			case 0x10020: // rwPDS_G3_SkSpt_MatPipeID
			case 0x1002A: // rwPDS_G3_SkSptUva_MatPipeID
				return sGeneric;
			default:
				warnUnregisteredPipeOnce(id);
				return sGeneric;
		}
	}

	void configureMaterial(SubMesh& sm, guest::ptr<RpMaterial> material, const MaterialContext& ctx,
		TextureManager& textures)
	{
		recordMaterialPipeline(material, ctx.geomFlags, ctx.fromAtomic, ctx.skinned);
		sm.matPipeline = material->pipeline.addr;
		pipelineForId(pdsPipeId(sm.matPipeline)).configure(sm, material, ctx, textures);
	}
} // namespace sky2
