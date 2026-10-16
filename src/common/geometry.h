// Shared 3D geometry back-end for the atomic + world-sector pipelines: the parsed-geometry types
// and the material/texture helpers both use to turn RW geometry in EE RAM into the render queue.
// Pipeline-specific parsing (RpAtomic skin/MatFX; RpWorldSector mesh header) lives in the pipelines/
// modules; the enqueue/render-state services they share are RenderQueue members.
#pragma once

#include "common.h"
#include "texture_manager.h" // TextureManager, used by the material-texture template below

namespace sky2
{
	// RwMatrix (right/up/at rows + pos translation) as a column-major mat4.
	inline glm::mat4 toMat4(const RwMatrixTag& m)
	{
		return glm::mat4(m.right.x, m.right.y, m.right.z, 0.0f,
			m.up.x, m.up.y, m.up.z, 0.0f,
			m.at.x, m.at.y, m.at.z, 0.0f,
			m.pos.x, m.pos.y, m.pos.z, 1.0f);
	}

	constexpr RwRGBA DEFAULT_MATERIAL_COLOR{0xFF, 0xFF, 0xFF, 0xFF};

	// RW's vertex-alpha blend term: does any vertex this submesh references carry non-opaque prelit
	// alpha? A property of the geometry and its index span, both of which are cached, so RW computes it
	// once when the geometry is instanced and so do we. Scanning it per draw instead re-walks cached
	// data every frame, and the opaque case (the common one) is the one that never short-circuits.
	inline bool anyPrelitAlpha(std::span<const Vertex3D> verts, std::span<const uint16_t> indices)
	{
		constexpr float PS2_OPAQUE = 128.0f / 255.0f; // raw prelit alpha 0x80
		for (const uint16_t idx : indices)
		{
			if (idx < verts.size() && verts[idx].color.a < PS2_OPAQUE)
			{
				return true;
			}
		}
		return false;
	}

	// The RpWorldSector fields sky2 reads, taken from whichever layout this disc's RenderWare uses.
	struct SectorFields
	{
		uint32_t triangles, vertices, normals, texCoords0, preLitLum, mesh, pipeline;
		uint32_t lightsListHead; // address of the light list's head link, for the tie walk
		int numVertices, numTriangles, matListWindowBase;
	};

	template <class Sector>
	SectorFields readSectorFieldsAs(uint32_t sectorAddr)
	{
		const auto s = guest::ptr<Sector>(sectorAddr);
		return {s->triangles.addr, s->vertices.addr, s->normals.addr, s->texCoords[0].addr,
			s->preLitLum.addr, s->mesh.addr, s->pipeline.addr,
			sectorAddr + static_cast<uint32_t>(offsetof(Sector, lightsInWorldSector)),
			s->numVertices, s->numTriangles, s->matListWindowBase};
	}

	inline SectorFields readSectorFields(uint32_t sectorAddr)
	{
		return sky2::addresses().RenderWareVersion == 35 ? readSectorFieldsAs<RpWorldSectorRW35>(sectorAddr) : readSectorFieldsAs<RpWorldSector>(sectorAddr);
	}

	// Also reports the texture's packed filterAddressing, which the draw carries so the render thread
	// can bind a sampler object instead of relying on state baked into a shared texture.
	template <class Material>
	std::optional<TextureId> convertMaterialTextureRaster(Material material, TextureManager& textures,
		unsigned& sampler)
	{
		if (const auto texture = material->texture)
		{
			sampler = static_cast<unsigned>(texture->filterAddressing);
		}
		return textures.convertTexture(material->texture.addr);
	}

	template <class Material>
	RwRGBA getMaterialColor(Material material)
	{
		return material->color;
	}

	inline bool materialBlends(const RwRGBA& c) { return !c.opaque(); }

	struct SubMesh
	{
		GeomId indices = GEOM_ID_NONE;
		RwRGBA materialBaseColor;
		std::optional<TextureId> texKey;
		unsigned sampler = 0; // packed filterAddressing for texKey
		bool blend = false; // translucent material (alpha < opaque)
		bool prelitAlpha = false; // any vertex of this submesh has non-opaque prelit alpha
		bool modulate = false; // rpGEOMETRYMODULATEMATERIALCOLOR set and this material's color != white
		uint32_t matPipeline = 0; // RpMaterial::pipeline, for P3FES_PIPE_DUMP
		RpMatFXEffect matfxEffect = rpMATFXEFFECTNULL; // RpMatFX combined effect type
		// MatFX UV transform (effect 5/6, atomic-enabled): 2D affine rows for the base texcoords.
		// Identity by default, so the shader applies it unconditionally as a no-op otherwise.
		glm::vec3 uvRow0{1.0f, 0.0f, 0.0f}; // u' = dot(vec3(uv,1), uvRow0)
		glm::vec3 uvRow1{0.0f, 1.0f, 0.0f}; // v' = dot(vec3(uv,1), uvRow1)
		// MatFX second pass: a DUAL overlay sampled on the set-1 texcoords, or an ENVMAP reflection
		// whose texcoords come from the vertex normal. Both draw the geometry again through GS
		// context 2, so the blend is that context's ALPHA_2.
		bool hasPass2 = false;
		std::optional<TextureId> pass2TexKey;
		GsBlend pass2Blend;
		bool pass2Env = false; // ENVMAP: generate texcoords rather than read set 1
		float pass2Coef = 1.0f; // ENVMAP reflection strength (RpMatFX coef)
		glm::vec3 dualUvRow0{1.0f, 0.0f, 0.0f}; // DUAL UV transform (set-1 texcoords)
		glm::vec3 dualUvRow1{0.0f, 1.0f, 0.0f};
		float surfAmbient = 1.0f; // RpMaterial.surfaceProps: light coefficients (RwD3D9SetSurfaceProperties)
		float surfDiffuse = 1.0f;
		// Identifies this submesh within its object across frames. The guest material address where the
		// pipeline has one, else the material index; a submesh's position in the list is not usable,
		// because a re-parse that leaves one material with no triangles shifts every later entry.
		uint32_t materialKey = 0;
	};

	// Read a material's surface light coefficients (RwSurfaceProperties). These scale the ambient and
	// directional contributions in the RW lighting model, so a low-coefficient material (e.g. a floor)
	// doesn't over-brighten while a character material lights normally.
	template <class Material>
	void readSurfaceProps(Material material, SubMesh& sm)
	{
		sm.surfAmbient = material->surfaceProps.ambient;
		sm.surfDiffuse = material->surfaceProps.diffuse;
	}

	struct ParsedGeometry
	{
		GeomId verts = GEOM_ID_NONE;
		int vcount = 0;
		bool lit = false; // rpGEOMETRYLIGHT set: lit regime
		bool hasNormals = false; // normals present: gates the directional term
		bool matfxEnabled = false; // RpMatFXAtomicEnableEffects flag on this atomic
		bool hasVertexColor = false;
		bool native = false; // geometry came from the pre-instanced VIF table, not the RW arrays
		bool skinned = false; // GPU-skin via the frame's bone arena
		uint32_t boneOffset = 0; // slice of Frame::bones for this object
		uint16_t boneCount = 0;
		Blend blend; // resolved per-draw blend (from render state / GS ALPHA register; blend.enable gates it)
		StencilState stencil; // RW stencil render states (from render state)
		int cull = rwCULLMODECULLNONE;
		bool ztest = true; // depth test + write (from render state)
		bool zwrite = true;
		bool fog = false; // distance fog (from render state)
		uint32_t objPipeline = 0; // RpAtomic/RpWorldSector::pipeline, for P3FES_PIPE_DUMP
		uint32_t sourceAddr = 0; // atomic or world-sector address (manifest, and the cross-frame identity)
		MeshSource source = MeshSource::Atomic; // which of the two sourceAddr means
		uint32_t geomFlags = 0;
		int numTexCoordSets = 0;
		uint32_t texCoordsAddr = 0;
		glm::mat4 model{1.0f};
		std::vector<SubMesh> subs;

		// Clear for reuse, keeping the subs allocation. The pipelines hold one of these rather than
		// building a fresh one per object, which malloc'd a vector for every draw call.
		void reset()
		{
			std::vector<SubMesh> keep = std::move(subs);
			keep.clear();
			*this = ParsedGeometry{};
			subs = std::move(keep);
		}
	};
} // namespace sky2
