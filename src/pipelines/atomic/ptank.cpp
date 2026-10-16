#include "pipelines/atomic/ptank.h"

#include "device.h"
#include "common.h"
#include "render_queue.h"
#include "rw_to_gl.h"
#include "texture_manager.h"

#include "sky2/config.h"
#include "geometry.h"

#include <cmath>
#include <cstdlib>

namespace sky2
{
	namespace
	{
		// Slot in RpPTankData::clusters, one per attribute.
		constexpr int CL_POSITION = 0;
		constexpr int CL_MATRIX = 1;
		constexpr int CL_SIZE = 3;
		constexpr int CL_COLOR = 4;
		constexpr int CL_VTXCOLOR = 5;
		constexpr int CL_2DROTATE = 6;
		constexpr int CL_VTX2TEXCOORDS = 7;
		constexpr int CL_VTX4TEXCOORDS = 8;

		// The data-flag bit that instances each slot, in slot order.
		constexpr uint32_t CL_DFLAG[9] = {rpPTANKDFLAGPOSITION, rpPTANKDFLAGMATRIX, rpPTANKDFLAGNORMAL,
			rpPTANKDFLAGSIZE, rpPTANKDFLAGCOLOR, rpPTANKDFLAGVTXCOLOR, rpPTANKDFLAG2DROTATE,
			rpPTANKDFLAGVTX2TEXCOORDS, rpPTANKDFLAGVTX4TEXCOORDS};

		constexpr int MAX_PARTICLES = 4096; // four vertices each, well inside 16-bit indices

		// Which sprite generator the tank was built for. The 2D one places particles in screen space
		// and sets up VU1 with half the projection scale, so putting its positions through the camera
		// draws them somewhere else entirely (p3-shuffle-cards 6.78 -> 8.15). Only the 3D generator is
		// handled; the field is 2 for it and 0 for the 2D one on every tank this disc builds.
		constexpr uint32_t PLATFLAG_SPR3DGEN = 2;

		uint32_t ptankDataOffset()
		{
			const uint32_t global = sky2::addresses().rpPTankAtomicDataOffset;
			// Read every time rather than caching: the plugin registers during boot, so a cached zero
			// from before registration would disable the path for the whole run.
			return global && guest::ready() ? *guest::ptr<uint32_t>(global) : 0;
		}

		// A slot the flags do not instance is left holding whatever was in the allocation rather than
		// zeroed, so a null check reads garbage as a live attribute.
		PTankCluster toCluster(const RpPTankData& pub, int slot)
		{
			const RpPTankLockStruct& c = pub.clusters[slot];
			return (pub.format.dataFlags & CL_DFLAG[slot]) ? PTankCluster{c.data.addr, c.stride} : PTankCluster{};
		}

		glm::vec4 readRGBA(uint32_t addr)
		{
			return toVec4(*guest::ptr<RwRGBA>(addr));
		}

		glm::vec2 readV2(uint32_t addr)
		{
			const auto v = guest::ptr<RwV2d>(addr);
			return {v->x, v->y};
		}

		glm::vec2 readUV(uint32_t addr)
		{
			const auto t = guest::ptr<RwTexCoords>(addr);
			return {t->u, t->v};
		}

		// The GIFtag's PRIM field starts at bit 47, and the primitive type is its low three bits.
		constexpr int GIFTAG_PRIM_SHIFT = 47;

		PTankSkyState readSkyState(const RpPTankData& pub)
		{
			PTankSkyState s;
			if (!pub.userData)
			{
				return s;
			}
			const auto sky = guest::ptr<RpPTankSkyData>(pub.userData.addr);
			const auto xf = sky->transformInfo;
			// Reject rather than trust: this layout is from a different RenderWare build, and a wrong
			// one would hand the renderer a blend mode assembled out of unrelated bytes. Undefined bits
			// in ALPHA are what a misread looks like. transformInfo is not part of the test: it reads
			// null on this disc, which is the fast pipeline rather than a bad offset, since the two
			// registers ahead of it decode to the blends the effect visibly uses.
			constexpr uint64_t ALPHA_DEFINED = 0x000000FF000000FFull;
			s.alpha = sky->alpha;
			s.test = sky->test;
			s.tex1 = sky->tex1;
			s.transformInfo = xf.addr;
			if (xf)
			{
				s.prim = xf->prim;
				s.primType = static_cast<uint32_t>((xf->prim >> GIFTAG_PRIM_SHIFT) & 7u);
				s.batchSize = xf->batchSize;
				s.numClusters = xf->numClusters;
			}
			s.valid = (sky->alpha & ~ALPHA_DEFINED) == 0 && (sky->test >> 19) == 0;
			return s;
		}
	} // namespace

	bool PTankPipeline::isPTank(uint32_t atomicAddr)
	{
		const uint32_t off = ptankDataOffset();
		return off && *guest::ptr<uint32_t>(atomicAddr + off) != 0;
	}

	bool PTankPipeline::parse(uint32_t atomicAddr, ParsedPTank& out) const
	{
		out = {};
		// The plugin slot holds a pointer to the private struct, not the struct inline.
		const auto prv = guest::ptr<RpPTankAtomicExtPrv>(*guest::ptr<uint32_t>(atomicAddr + ptankDataOffset()));
		const RpPTankData& pub = prv->publicData;
		out.maxCount = prv->maxPCount;
		out.actCount = prv->actPCount;
		out.numClusters = static_cast<uint32_t>(pub.format.numClusters);
		out.structStride = static_cast<uint32_t>(pub.format.stride);
		out.dataFlags = pub.format.dataFlags;
		out.vertexAlphaBlend = pub.vertexAlphaBlend != 0;
		out.platFlags = prv->platFlags;
		out.sky = readSkyState(pub);
		if (sky2::env("PTANK_LOG"))
		{
			SKY2_TRACE_ONCE(atomicAddr,
				"ptank 0x%X: %d/%d particles nClusters=%u stride=%u flags=0x%X valpha=%d "
				"sky=%d xf=0x%X nCl=%u prim=0x%llX type=%u batch=%u alpha=0x%llX test=0x%llX",
				atomicAddr, out.actCount, out.maxCount, out.numClusters, out.structStride, out.dataFlags,
				int(out.vertexAlphaBlend), int(out.sky.valid), out.sky.transformInfo, out.sky.numClusters,
				static_cast<unsigned long long>(out.sky.prim), out.sky.primType, out.sky.batchSize,
				static_cast<unsigned long long>(out.sky.alpha),
				static_cast<unsigned long long>(out.sky.test));
			// The instancer addresses name the variant the tank was built for, which the data flags only
			// imply: rpPTankSkyInsPosNCPSPR is no-centre, per-particle size, per-particle rotate. Resolve
			// them against the disc's symbol map.
			SKY2_TRACE_ONCE(atomicAddr + 1,
				"ptank 0x%X instancers: setup=0x%X pos=0x%X uv=0x%X color=0x%X normals=0x%X end=0x%X "
				"lock=0x%X inst=0x%X plat=0x%X userData=0x%X",
				atomicAddr, prv->insSetupCB.addr, prv->insPosCB.addr, prv->insUVCB.addr,
				prv->insColorsCB.addr, prv->insNormalsCB.addr, prv->insEndingCB.addr, prv->lockFlags,
				prv->instFlags, prv->platFlags, pub.userData.addr);
			// Which attributes a tank actually has, and where in the per-particle block each one sits.
			for (int k = 0; k < 9; k++)
			{
				SKY2_TRACE_ONCE(atomicAddr + 2 + static_cast<uint32_t>(k), "ptank 0x%X cluster %d: data=0x%X stride=%d",
					atomicAddr, k, pub.clusters[k].data.addr, pub.clusters[k].stride);
			}
		}
		if (out.platFlags != PLATFLAG_SPR3DGEN)
		{
			SKY2_TRACE_ONCE(out.platFlags, "ptank platFlags=0x%X is not the 3D sprite generator; "
											"skipped, since its positions are not world space",
				out.platFlags);
			return false;
		}
		// Only the live particles: the buffers past actPCount hold whatever the last burst left there,
		// which draws as a cloud of dead particles frozen where they died.
		if (out.actCount <= 0 || out.actCount > out.maxCount || out.maxCount > MAX_PARTICLES)
		{
			return false;
		}
		out.position = toCluster(pub, CL_POSITION);
		if (!out.position)
		{
			return false; // no positions: nothing this can place
		}
		out.matrix = toCluster(pub, CL_MATRIX);
		out.size = toCluster(pub, CL_SIZE);
		out.color = toCluster(pub, CL_COLOR);
		out.vtxColor = toCluster(pub, CL_VTXCOLOR);
		out.rotate = toCluster(pub, CL_2DROTATE);
		out.uv2 = toCluster(pub, CL_VTX2TEXCOORDS);
		out.uv4 = toCluster(pub, CL_VTX4TEXCOORDS);
		out.cSize = {pub.cSize.x, pub.cSize.y};
		out.cRotate = pub.cRotate;
		out.cColor = toVec4(pub.cColor);
		out.cVtxColor = (pub.format.dataFlags & rpPTANKDFLAGCNSVTXCOLOR) != 0;
		for (int k = 0; k < 4; k++)
		{
			out.cUV[k] = {pub.cUV[k].u, pub.cUV[k].v};
			out.cVtxColorValue[k] = toVec4(pub.cVtxColor[k]);
		}
		if (pub.format.dataFlags & rpPTANKDFLAGUSECENTER)
		{
			// No tank on this disc sets it, so where cCenter shifts the quad is unread rather than
			// modelled. Report it instead of guessing a sign and an axis.
			SKY2_TRACE_ONCE(atomicAddr, "ptank 0x%X uses cCenter (%f,%f); the offset is not applied",
				atomicAddr, pub.cCenter.x, pub.cCenter.y);
		}
		const auto geometry = guest::ptr<RpAtomic>(atomicAddr)->geometry;
		if (sky2::env("PTANK_LOG") && geometry && geometry->matList.numMaterials > 0)
		{
			const auto m = guest::ptr<RpMaterial>(geometry->matList.materials[0].addr);
			SKY2_TRACE_ONCE(atomicAddr + 0x10000, "ptank 0x%X material 0x%X colour %u,%u,%u,%u tex=0x%X cColor %.0f,%.0f,%.0f,%.0f p0 %u,%u,%u,%u",
				atomicAddr, geometry->matList.materials[0].addr, m->color.red, m->color.green,
				m->color.blue, m->color.alpha, m->texture.addr, out.cColor.r, out.cColor.g,
				out.cColor.b, out.cColor.a,
				out.color ? guest::ptr<RwRGBA>(out.color.at(0))->red : 0,
				out.color ? guest::ptr<RwRGBA>(out.color.at(0))->green : 0,
				out.color ? guest::ptr<RwRGBA>(out.color.at(0))->blue : 0,
				out.color ? guest::ptr<RwRGBA>(out.color.at(0))->alpha : 0);
		}
		if (geometry && geometry->matList.numMaterials > 0)
		{
			out.material = geometry->matList.materials[0].addr;
		}
		return true;
	}

	void PTankPipeline::render(uint32_t atomicAddr)
	{
		if (!device->queue.haveCam || !parse(atomicAddr, pt))
		{
			return;
		}
		static const bool sEnabled = sky2::env("NO_PTANK") == nullptr;
		if (sEnabled)
		{
			submitQuads(atomicAddr, pt);
		}
	}

	void PTankPipeline::submitQuads(uint32_t atomicAddr, const ParsedPTank& tank)
	{
		RenderQueue& queue = device->queue;
		// The billboard basis is the camera's, and the atomic's LTM still applies to the positions: a
		// tank parented to a moving frame moves with it.
		CameraParts cam;
		buildCameraVP(queue.activeCamera, &cam);
		if (!cam.valid)
		{
			return;
		}
		const glm::vec3 camRight = glm::vec3(cam.camWorld[0]);
		const glm::vec3 camUp = glm::vec3(cam.camWorld[1]);
		const uint32_t frameAddr = guest::ptr<RpAtomic>(atomicAddr)->object.object.parent.addr;
		const glm::mat4 ltm = toMat4(guest::ptr<RwFrame>(frameAddr)->ltm);

		MeshDraw m;
		m.model = glm::mat4(1.0f); // the LTM is applied per particle, into world space
		m.diag.source = MeshSource::Atomic;
		m.material.unlit = true; // colour = texture x particle colour, no scene lighting
		// spr3dgenPS2AllBridgeCallBack feeds RpMaterial::color through rxPS2AllMatColASMFunc; the raw
		// byte is what belongs here, same as the mesh path, since ps2AScale renormalizes it in the shader.
		m.material.color = glm::vec4(1.0f);
		if (tank.material)
		{
			m.material.color = toVec4(guest::ptr<RpMaterial>(tank.material)->color);
		}

		DrawState st;
		st.blend = queue.blendState();
		st.blend.enable = true;
		st.atest = queue.gsAtest;
		st.gsAlphaReg = queue.gsBlend.reg.U64;
		st.stencil = queue.stencil;
		st.cull = rwCULLMODECULLNONE; // billboards face the camera; a cull mode would only drop them
		st.ztest = queue.rsHits[rwRENDERSTATEZTESTENABLE] ? queue.renderState[rwRENDERSTATEZTESTENABLE] != 0 : true;
		st.zwrite = queue.rsHits[rwRENDERSTATEZWRITEENABLE] ? queue.renderState[rwRENDERSTATEZWRITEENABLE] != 0 : false;
		st.colorMask = queue.colorMask;
		st.clamp = queue.gsClamp;
		// The tank carries its own GS registers, which the render node sends ahead of the particles
		// rather than going through RpSkyRenderStateSet, so they override the render state. And
		// vertexAlphaBlend is what becomes PRIM.ABE: a tank without it does not blend at all.
		if (tank.sky.valid)
		{
			GIFRegALPHA alpha{};
			alpha.U64 = tank.sky.alpha;
			const GsBlend b = gsAlphaToBlend(alpha);
			st.blend.equation = b.equation;
			st.blend.src = b.src;
			st.blend.dst = b.dst;
			st.blend.constAlpha = b.constAlpha;
			st.blend.alphaPremul = b.alphaPremul;
			st.blend.enable = tank.vertexAlphaBlend;
			st.gsAlphaReg = tank.sky.alpha;
			GIFRegTEST test{};
			test.U64 = tank.sky.test;
			st.atest = gsTestToAtest(test);
			st.ztest = test.ZTE != 0 && test.ZTST >= ZTST_GEQUAL;
			// ATST=NEVER with AFAIL=FB_ONLY is how a particle writes colour without touching Z: the
			// test rejects every fragment and the fail mode writes the frame buffer anyway.
			if (test.ATE != 0 && test.ATST == ATST_NEVER && test.AFAIL == AFAIL_FB_ONLY)
			{
				st.zwrite = false;
			}
		}
		m.stateIndex = queue.stateIndexFor(st);
		m.lightsIndex = queue.lightsIndexFor(FrameLights{}); // unlit: the shader ignores these
		m.material.sampler = device->im2dFilterAddressing();
		const auto material = guest::ptr<RpMaterial>(tank.material);
		if (tank.material && material->texture)
		{
			m.material.texKey =
				device->textures.convertRaster(material->texture->raster.addr, m.material.sampler);
		}

		Frame& frame = queue.buildFrame();
		m.vertOffset = static_cast<uint32_t>(frame.transientVerts.size());
		m.vertCount = static_cast<uint32_t>(tank.actCount) * 4;
		m.idxOffset = static_cast<uint32_t>(frame.transientIdx.size());
		m.idxCount = static_cast<uint32_t>(tank.actCount) * 6;
		frame.transientVerts.resize(m.vertOffset + m.vertCount, Vertex3D{});
		frame.transientIdx.resize(m.idxOffset + m.idxCount, 0);

		for (int i = 0; i < tank.actCount; i++)
		{
			const auto p = guest::ptr<RwV3d>(tank.position.at(i));
			const glm::vec3 center = glm::vec3(ltm * glm::vec4(p->x, p->y, p->z, 1.0f));
			glm::vec3 right = camRight;
			glm::vec3 up = camUp;
			if (tank.matrix)
			{
				// A per-particle matrix supplies orientation and scale together, which is why the size
				// cluster is dead in that configuration.
				const glm::mat4 pm = toMat4(*guest::ptr<RwMatrixTag>(tank.matrix.at(i)));
				right = glm::vec3(pm[0]);
				up = glm::vec3(pm[1]);
			}
			else
			{
				const glm::vec2 s = tank.size ? readV2(tank.size.at(i)) : tank.cSize;
				const float rot = tank.rotate ? *guest::ptr<float>(tank.rotate.at(i)) : tank.cRotate;
				if (rot != 0.0f)
				{
					const float c = std::cos(rot), sn = std::sin(rot);
					const glm::vec3 r = right, u = up;
					right = r * c + u * sn;
					up = u * c - r * sn;
				}
				right *= s.x * 0.5f;
				up *= s.y * 0.5f;
			}
			// rpPTANKDFLAGUSECENTER picks the CC instancers, which hang the quad off cCenter. This disc
			// picks NC (rpPTankSkyInsPosNCPSPR), so the quad centres on the particle.
			const glm::vec3& pivot = center;
			// Colour precedence, from _rpPTankSkyInstanceSelectCB: per particle, then per vertex, then
			// one set of four corner colours for the tank, then one colour for the tank.
			const bool perVertex = !tank.color && (tank.vtxColor || tank.cVtxColor);
			const glm::vec4 color = tank.color ? readRGBA(tank.color.at(i)) : tank.cColor;
			const glm::vec3 corner[4] = {
				pivot - right + up, pivot + right + up, pivot + right - up, pivot - right - up};
			glm::vec2 uv[4];
			if (tank.uv4)
			{
				for (int k = 0; k < 4; k++)
				{
					uv[k] = readUV(tank.uv4.at(i) + static_cast<uint32_t>(k) * 8);
				}
			}
			else
			{
				// Two texcoords are the sprite rect's opposite corners, not two of the four vertices, so
				// the other two are the mixed pair. Constant when the cluster is absent (rpPTankSkyInsUV2C).
				const glm::vec2 a = tank.uv2 ? readUV(tank.uv2.at(i)) : tank.cUV[0];
				const glm::vec2 b = tank.uv2 ? readUV(tank.uv2.at(i) + 8) : tank.cUV[1];
				uv[0] = {a.x, a.y};
				uv[1] = {b.x, a.y};
				uv[2] = {b.x, b.y};
				uv[3] = {a.x, b.y};
			}

			Vertex3D* v = &frame.transientVerts[m.vertOffset + static_cast<size_t>(i) * 4];
			for (int k = 0; k < 4; k++)
			{
				v[k].pos = corner[k];
				v[k].color =
					perVertex ? (tank.vtxColor ? readRGBA(tank.vtxColor.at(i) + static_cast<uint32_t>(k) * 4) : tank.cVtxColorValue[k]) : color;
				v[k].uv = uv[k];
			}
			uint16_t* idx = &frame.transientIdx[m.idxOffset + static_cast<size_t>(i) * 6];
			constexpr uint16_t QUAD[6] = {0, 1, 2, 0, 2, 3};
			for (int k = 0; k < 6; k++)
			{
				idx[k] = static_cast<uint16_t>(i * 4 + QUAD[k]);
			}
		}
		queue.submitMesh(std::move(m));
	}
} // namespace sky2
