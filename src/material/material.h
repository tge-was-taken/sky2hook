// Material-pipeline layer. RenderWare routes geometry through two orthogonal PDS pipeline slots: an
// object pipeline (RpAtomic/RpWorldSector::pipeline) and a per-mesh material pipeline
// (RpMaterial::pipeline). Each is an RxPipeline* whose pluginData field (offset 0x30) holds the PDS
// pipe id (RpPDSPipeID). This layer owns the material axis: it reads a material into a SubMesh via the
// family selected by the material's pipe id, replacing the flag-inference the atomic pipeline used to
// do inline. The object axis (rigid vs skinned) stays in the object pipelines. recordObjectPipeline +
// P3FES_LOG_MATPIPE inventory which pipelines P3FES uses per slot.
#pragma once

#include <string>

#include "geometry.h"

namespace sky2
{
	// Object-scope state the object pipeline passes down to the material pipeline.
	struct MaterialContext
	{
		// Flags holding the modulate bit: rpGEOMETRYMODULATEMATERIALCOLOR from the owning geometry for
		// an atomic, rpWORLDMODULATEMATERIALCOLOR from the world for a sector. The bit is 0x40 in both.
		uint32_t geomFlags = 0;
		bool matfxAtomicEnabled = false; // RpMatFXAtomicEnableEffects on the owning atomic
		bool applyEffects = false; // MatFX rides the atomic path only; sectors carry no effect data
		bool fromAtomic = false; // probe: owning object class
		bool skinned = false; // probe: owning atomic was skinned
	};

	// Read a material into a SubMesh via its PDS material pipeline: looks up the family for the pipe id
	// and delegates. Unregistered ids fall back to the generic family (logged once).
	void configureMaterial(SubMesh& sm, guest::ptr<RpMaterial> material, const MaterialContext& ctx,
		TextureManager& textures);

	// Probe (P3FES_LOG_MATPIPE): inventory the object-pipeline slot. Material-pipeline recording is folded
	// into configureMaterial. Dumps at process exit.
	void recordObjectPipeline(uint32_t pipelinePtr, bool isSector, bool skinned);
	// The PDS pipe id an RxPipeline carries, 0 when it has none. Names the family a draw came from.
	uint32_t pdsPipeIdOf(uint32_t pipelinePtr);
	// An RxPipeline's node names, joined. The only thing that says what a pipeline actually does.
	std::string pipelineNodeNames(uint32_t pipelinePtr);
	void recordObjectNoDraw(uint32_t pipelinePtr);
	void dumpMaterialPipelines();
} // namespace sky2
