#include "render_queue.h"

#include "material/pipeline_dump.h"

#include "common.h"
#include "sky2/addresses.h" // kwln light globals (sAmbientLight etc.) for the world-membership diagnostic
#include "object_manager.h"
#include "renderer.h" // renderer->submit at end-of-frame
#include "rw_to_gl.h"
#include "texture_manager.h"

#include "geometry.h" // ParsedGeometry/SubMesh + toMat4 for the enqueue services

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace sky2
{

	// The pass currently being built (EE thread): the last camera pass, or a default one if a draw
	// arrives before any RwCameraBeginUpdate (defensive; shouldn't happen in practice).
	FramePass& RenderQueue::currentBuildPass()
	{
		if (buildFrame().passCount() == 0)
		{
			buildFrame().openPass();
			if (havePendingClear)
			{
				buildFrame().passes[0].clear = pendingClear;
				havePendingClear = false;
			}
		}
		return buildFrame().passes[buildFrame().passCount() - 1];
	}

	// Does the current render target reach the screen? The raster type answers it: rwRASTERTYPECAMERA
	// is a frame buffer, rwRASTERTYPECAMERATEXTURE is render-to-texture. ShowRaster is kept only as
	// direct evidence for the one raster it names, not as the rule.
	bool RenderQueue::onScreenTarget()
	{
		if (objects->displayRasters.count(activeTarget))
		{
			return true;
		}
		return activeTarget &&
			(guest::ptr<RwRaster>(activeTarget)->cType & rwRASTERTYPEMASK) == rwRASTERTYPECAMERA;
	}

	void RenderQueue::submitMesh(MeshDraw&& m)
	{
		Frame& f = buildFrame();
		FramePass& p = currentBuildPass();
		// Position within the pass. Set here rather than in enqueueSubMesh so the Im3D path, which
		// builds a MeshDraw of its own, gets one too.
		m.diag.drawOrdinal = static_cast<uint16_t>(p.cmdEnd - p.cmdBegin);
		f.meshCmds.push_back(std::move(m));
		f.commandList.push_back({CmdType::Mesh, static_cast<uint32_t>(f.meshCmds.size() - 1)});
		p.cmdEnd = static_cast<uint32_t>(f.commandList.size());
	}

	void RenderQueue::submitIm2D(Im2DDraw&& d)
	{
		Frame& f = buildFrame();
		FramePass& p = currentBuildPass();
		if (im2dTagDraws > 0)
		{
			d.interpTag = im2dTag;
			im2dTagDraws--;
		}
		f.im2dCmds.push_back(std::move(d));
		f.commandList.push_back({CmdType::Im2d, static_cast<uint32_t>(f.im2dCmds.size() - 1)});
		p.cmdEnd = static_cast<uint32_t>(f.commandList.size());
	}

	void RenderQueue::beginFrame()
	{
		wantPoses = renderer->interp.enabled(); // sampled here, then fixed for the whole frame
		im2dTagDraws = 0; // a name enabled last frame and never drawn dies here
		build = nullptr; // the next draw acquires a fresh slot, with last frame's storage retained
		buildDisplayRaster = 0;
		objects->dumpWatchedRasters();
	}

	uint32_t RenderQueue::stateIndexFor(const DrawState& st)
	{
		std::vector<DrawState>& table = buildFrame().states;
		if (sky2::config().stateDedupe && !table.empty() && table.back() == st)
			return static_cast<uint32_t>(table.size() - 1);
		table.push_back(st);
		return static_cast<uint32_t>(table.size() - 1);
	}

	uint32_t RenderQueue::lightsIndexFor(const FrameLights& l)
	{
		std::vector<FrameLights>& table = buildFrame().lights;
		if (sky2::config().lightDedupe && !table.empty() && table.back() == l)
			return static_cast<uint32_t>(table.size() - 1);
		table.push_back(l);
		return static_cast<uint32_t>(table.size() - 1);
	}

	void RenderQueue::endFrame()
	{
		Frame& f = buildFrame();
		f.displayRaster = buildDisplayRaster;
		f.endBuild();
		// Alongside the frame pool's own line: geometry is the larger pool, and whether it converges is
		// the whole point of recycling buffers through scratch().
		// P3FES_LOG_BATCH: how long a run of consecutive draws shares the state a multi-draw call would
		// have to hold constant. Short runs mean MDI has nothing to merge and is not worth building.
		static const bool sLogBatch = sky2::env("LOG_BATCH") != nullptr;
		if (sLogBatch && f.index % 60 == 0)
		{
			size_t runs = 0, longest = 0, run = 0;
			uint32_t prevState = ~0u, prevTex = ~0u;
			for (const uint32_t ci : f.drawOrder)
			{
				const FrameCmd& c = f.commandList[ci];
				const uint32_t st = c.type == CmdType::Mesh ? f.meshCmds[c.data].stateIndex
															: f.im2dCmds[c.data].stateIndex;
				const uint32_t tex = c.type == CmdType::Mesh
										 ? f.meshCmds[c.data].material.texKey.value_or(0)
										 : f.im2dCmds[c.data].texKey.value_or(0);
				if (st == prevState && tex == prevTex)
				{
					run++;
				}
				else
				{
					longest = std::max(longest, run);
					runs++;
					run = 1;
				}
				prevState = st;
				prevTex = tex;
			}
			longest = std::max(longest, run);
			std::fprintf(stderr, "[sky2][batch] frame %llu draws=%zu runs=%zu avg=%.2f longest=%zu\n",
				static_cast<unsigned long long>(f.index), f.drawOrder.size(), runs,
				runs ? static_cast<double>(f.drawOrder.size()) / static_cast<double>(runs) : 0.0, longest);
		}
		static const bool sLogAlloc = sky2::env("LOG_FRAME_ALLOC") != nullptr;
		if (sLogAlloc && f.index % 60 == 0)
		{
			std::fprintf(stderr,
				"[sky2][alloc] geom verts=%.1f KB (%zu live, %zu reused/%zu new) "
				"idx=%.1f KB (%zu live, %zu reused/%zu new)\n",
				geometry->verts.bytes() / 1024.0, geometry->verts.liveCount(),
				geometry->verts.reused(), geometry->verts.allocated(),
				geometry->indices.bytes() / 1024.0, geometry->indices.liveCount(),
				geometry->indices.reused(), geometry->indices.allocated());
			// The same split the overlay shows, for headless runs: parsing on this thread, uploading on
			// the render thread, each mostly avoided by a cache hit.
			const GeomStats& g = geometry->stats;
			const auto get = [](const std::atomic<uint64_t>& c) { return c.load(std::memory_order_relaxed); };
			const uint64_t parsed = get(g.atomics.misses) + get(g.sectors.misses);
			const uint64_t cached = get(g.atomics.hits) + get(g.sectors.hits);
			std::fprintf(stderr,
				"[sky2][geomcost] parsed=%llu (%llu unchanged) cached=%llu (%.1f%% hit) parse=%.0f ms | "
				"uploads=%llu (%.1f MB) upload=%.0f ms creates=%llu evictions=%llu\n",
				static_cast<unsigned long long>(parsed),
				static_cast<unsigned long long>(get(g.atomics.unchanged) + get(g.sectors.unchanged)),
				static_cast<unsigned long long>(cached),
				(parsed + cached) ? 100.0 * double(cached) / double(parsed + cached) : 0.0,
				double(get(g.atomics.ns) + get(g.sectors.ns)) / 1.0e6,
				static_cast<unsigned long long>(get(g.vertUploads) + get(g.idxUploads)),
				double(get(g.uploadBytes)) / (1024.0 * 1024.0), double(get(g.uploadNs)) / 1.0e6,
				static_cast<unsigned long long>(get(g.bufferCreates)),
				static_cast<unsigned long long>(get(g.evictions)));
			std::fprintf(stderr, "[sky2][im3d] batches=%llu (%llu identity-model) %.1f MB\n",
				static_cast<unsigned long long>(get(g.im3dBatches)),
				static_cast<unsigned long long>(get(g.im3dIdentityModel)),
				double(get(g.im3dBytes)) / (1024.0 * 1024.0));
		}
		renderer->submit(&f);
	}

	// RwMatrixTag is right/up/at basis + pos, each padded to 16 bytes. Free rather than a member: it
	// reads only guest memory and holds no state.
	glm::mat4 buildCameraVP(uint32_t camAddr, CameraParts* parts)
	{
		if (parts)
			*parts = CameraParts{}; // every early exit below leaves it invalid
		if (!camAddr)
		{
			SKY2_ERROR(false, "called with a null camera address");
			return glm::mat4(1.0f);
		}
		const auto camera = guest::ptr<RwCamera>(camAddr);
		const float vwx = camera->viewWindow.x;
		const float vwy = camera->viewWindow.y;
		const float nearP = camera->nearPlane;
		const float farP = camera->farPlane;
		if (vwx == 0.0f || vwy == 0.0f || farP - nearP < 0.001f)
		{
			// Reproduces every time entering Tartarus, during the fade transition: near/far are real
			// (100/3000), only viewWindow is zeroed, suggesting a transitional camera caught before
			// RW finishes configuring it. Screen is faded near-black then, so the identity-matrix
			// fallback below is likely imperceptible - still reported since it's an unverified guess.
			SKY2_ERROR(false, "degenerate camera 0x%X (viewWindow=%g,%g near=%g far=%g)",
				camAddr, vwx, vwy, nearP, farP);
			return glm::mat4(1.0f);
		}
		const uint32_t camFrame = camera->object.object.parent.addr;
		if (!camFrame)
		{
			SKY2_ERROR(false, "camera 0x%X has no parent frame", camAddr);
			return glm::mat4(1.0f);
		}
		const glm::mat4 ltm = toMat4(guest::ptr<RwFrame>(camFrame)->ltm);
		const float det = glm::determinant(ltm);
		if (!std::isfinite(det) || std::fabs(det) < 1e-8f)
		{
			SKY2_ERROR(false, "camera 0x%X frame 0x%X LTM is singular/non-finite (det=%g)",
				camAddr, camFrame, det);
			return glm::mat4(1.0f);
		}
		const glm::mat4 view = glm::inverse(ltm);
		// viewWindow is the half-extent at unit depth (perspective) or in world units (parallel).
		// Negate X to un-mirror RW's left-handedness for GL.
		glm::mat4 proj(0.0f);
		proj[0][0] = -1.0f / vwx;
		proj[1][1] = 1.0f / vwy;
		if (camera->projectionType == rwPARALLEL) // orthographic; e.g. the shadow light
		{
			proj[2][2] = 2.0f / (farP - nearP);
			proj[3][2] = -(farP + nearP) / (farP - nearP);
			proj[3][3] = 1.0f;
		}
		else if (camera->projectionType == rwPERSPECTIVE)
		{
			proj[2][2] = (farP + nearP) / (farP - nearP);
			proj[2][3] = 1.0f;
			proj[3][2] = -2.0f * farP * nearP / (farP - nearP);
		}
		else
		{
			// Falls through with a partly-zero proj, so the vp is singular rather than identity. Report
			// the parts as invalid: this is the one degenerate case that is not an early return.
			SKY2_ERROR(false, "unhandled camera projectionType=%d", camera->projectionType);
			return proj * view;
		}
		if (parts)
		{
			parts->camWorld = ltm;
			parts->viewWindowX = vwx;
			parts->viewWindowY = vwy;
			parts->nearPlane = nearP;
			parts->farPlane = farP;
			parts->projectionType = camera->projectionType;
			parts->valid = true;
		}
		return proj * view;
	}

	// Direction toward a directional RpLight, from its frame's -at (or a default up if unframed).
	static glm::vec3 dirLightToward(uint32_t lightAddr)
	{
		const uint32_t frame = guest::ptr<RpLight>(lightAddr)->object.object.parent.addr;
		if (!frame)
		{
			return glm::vec3(0.0f, 1.0f, 0.0f);
		}
		const RwV3d& at = guest::ptr<RwFrame>(frame)->ltm.at;
		const glm::vec3 a(at.x, at.y, at.z);
		return glm::dot(a, a) > 1e-6f ? -glm::normalize(a) : glm::vec3(0.0f, 1.0f, 0.0f);
	}

	// Cap every light-list walk in case a list is mid-mutation or corrupt.
	static constexpr int LIGHT_WALK_CAP = 64;

	// Fill one FrameLights local-light slot from a positioned RpLight (point/spot). Position/axis come
	// from the light's frame; params packs (radius, minusCosAngle, light type, 0) for the shader.
	static void addLocalLight(FrameLights& fl, uint32_t lightAddr)
	{
		if (fl.localCount >= MAX_LOCAL_LIGHTS)
			return;
		const auto light = guest::ptr<RpLight>(lightAddr);
		glm::vec3 pos(0.0f), axis(0.0f, 0.0f, 1.0f);
		if (const uint32_t frame = light->object.object.parent.addr)
		{
			const RwMatrixTag& ltm = guest::ptr<RwFrame>(frame)->ltm;
			pos = glm::vec3(ltm.pos.x, ltm.pos.y, ltm.pos.z);
			const glm::vec3 at(ltm.at.x, ltm.at.y, ltm.at.z);
			if (glm::dot(at, at) > 1e-6f)
				axis = glm::normalize(at); // shader dots against this unnormalized; normalize here
		}
		const int i = fl.localCount++;
		fl.localAddr[i] = lightAddr;
		fl.localColor[i] = glm::vec3(light->color.red, light->color.green, light->color.blue);
		fl.localPos[i] = pos;
		fl.localAxis[i] = axis;
		// params.z is the shader's light kind: 0 point, 1 hard spot, 2 soft spot (subType - rpLIGHTPOINT).
		fl.localParams[i] = glm::vec4(light->radius, light->minusCosAngle,
			static_cast<float>(light->object.object.subType - rpLIGHTPOINT), 0.0f);
	}

	// A sector's positioned lights, from its light-tie list. No dedupe, no range test: the tie exists
	// because the light's volume overlaps the sector (§6.3).
	static void gatherSectorLocalLights(FrameLights& fl, uint32_t sectorAddr)
	{
		const uint32_t listHead = readSectorFields(sectorAddr).lightsListHead;
		uint32_t node = *guest::ptr<uint32_t>(listHead); // link.next
		int found = 0;
		for (int n = 0; node && node != listHead && n < LIGHT_WALK_CAP && fl.localCount < MAX_LOCAL_LIGHTS; n++)
		{
			const auto tie = guest::ptr<RpLightTie>(node); // lightInWorldSector link is at tie offset 0
			const uint32_t lightAddr = tie->light.addr;
			node = tie->lightInWorldSector.next.addr;
			if (lightAddr && (guest::ptr<RpLight>(lightAddr)->object.object.flags & rpLIGHTLIGHTWORLD))
			{
				addLocalLight(fl, lightAddr);
				found++;
			}
		}
		SKY2_TRACE_ONCE(sectorAddr, "sector 0x%X local lights found %d", sectorAddr, found);
	}

	// An atomic's positioned lights, gathered through the atomic's own sector ties (an atomic can span
	// several sectors), deduped, and range-tested against the atomic's world bounding sphere (§6.2).
	static void gatherAtomicLocalLights(FrameLights& fl, uint32_t atomicAddr)
	{
		const auto atomic = guest::ptr<RpAtomic>(atomicAddr);
		const RwSphere& ws = atomic->worldBoundingSphere;
		const glm::vec3 aCenter(ws.center.x, ws.center.y, ws.center.z);
		const float aRadius = ws.radius;
		uint32_t seen[MAX_LOCAL_LIGHTS];
		int seenCount = 0;
		auto markSeen = [&](uint32_t a) {
			for (int i = 0; i < seenCount; i++)
				if (seen[i] == a)
					return true;
			if (seenCount < MAX_LOCAL_LIGHTS)
				seen[seenCount++] = a;
			return false;
		};
		const uint32_t tieHead = atomicAddr + offsetof(RpAtomic, llWorldSectorsInAtomic);
		uint32_t tnode = atomic->llWorldSectorsInAtomic.link.next.addr;
		for (int n = 0; tnode && tnode != tieHead && n < LIGHT_WALK_CAP; n++)
		{
			const uint32_t tieAddr = tnode - offsetof(RpTie, lWorldSectorInAtomic);
			const auto tie = guest::ptr<RpTie>(tieAddr);
			tnode = tie->lWorldSectorInAtomic.next.addr;
			const uint32_t sectorAddr = tie->worldSector.addr;
			if (!sectorAddr)
				continue;
			const uint32_t listHead = readSectorFields(sectorAddr).lightsListHead;
			uint32_t lnode = *guest::ptr<uint32_t>(listHead); // link.next
			for (int m = 0; lnode && lnode != listHead && m < LIGHT_WALK_CAP && fl.localCount < MAX_LOCAL_LIGHTS; m++)
			{
				const auto ltie = guest::ptr<RpLightTie>(lnode);
				const uint32_t lightAddr = ltie->light.addr;
				lnode = ltie->lightInWorldSector.next.addr;
				if (!lightAddr || markSeen(lightAddr))
					continue;
				const auto light = guest::ptr<RpLight>(lightAddr);
				if (!(light->object.object.flags & rpLIGHTLIGHTATOMICS))
					continue;
				glm::vec3 lpos(0.0f);
				if (const uint32_t lframe = light->object.object.parent.addr)
				{
					const RwMatrixTag& ltm = guest::ptr<RwFrame>(lframe)->ltm;
					lpos = glm::vec3(ltm.pos.x, ltm.pos.y, ltm.pos.z);
				}
				const glm::vec3 d = aCenter - lpos;
				const float r = aRadius + light->radius;
				if (glm::dot(d, d) < r * r)
					addLocalLight(fl, lightAddr);
			}
		}
	}

	// The world's lights for one object, filtered by target class (rpLIGHTLIGHTATOMICS vs
	// rpLIGHTLIGHTWORLD). Ambient accumulates; colours are read live so a mid-frame re-scope lands right.
	std::optional<glm::vec3> RenderQueue::lightLock(int pass, int slot) const
	{
		const auto read = [this](int key) -> std::optional<glm::vec3> {
			if (key < 0 || key >= static_cast<int>(lightLocks.size()))
				return std::nullopt;
			const LightLock& l = lightLocks[static_cast<size_t>(key)];
			if (!l.held.load(std::memory_order_acquire))
				return std::nullopt;
			return glm::vec3(l.r.load(std::memory_order_relaxed), l.g.load(std::memory_order_relaxed),
				l.b.load(std::memory_order_relaxed));
		};
		if (const auto scoped = read(lightLockKey(pass, slot)))
			return scoped;
		return read(lightLockKey(-1, slot)); // the all-pass hold
	}

	void RenderQueue::setLightLock(int pass, int slot, const glm::vec3& c)
	{
		const int key = lightLockKey(pass, slot);
		if (key < 0 || key >= static_cast<int>(lightLocks.size()))
			return;
		LightLock& l = lightLocks[static_cast<size_t>(key)];
		l.r.store(c.x, std::memory_order_relaxed);
		l.g.store(c.y, std::memory_order_relaxed);
		l.b.store(c.z, std::memory_order_relaxed);
		l.held.store(true, std::memory_order_release); // publish the colour before the flag
	}

	void RenderQueue::clearLightLock(int pass, int slot)
	{
		const int key = lightLockKey(pass, slot);
		if (key >= 0 && key < static_cast<int>(lightLocks.size()))
			lightLocks[static_cast<size_t>(key)].held.store(false, std::memory_order_release);
	}

	void RenderQueue::clearLightLocks()
	{
		for (LightLock& l : lightLocks)
			l.held.store(false, std::memory_order_release);
	}

	bool RenderQueue::anyLightLockHeld() const
	{
		for (const LightLock& l : lightLocks)
			if (l.held.load(std::memory_order_acquire))
				return true;
		return false;
	}

	FrameLights RenderQueue::gatherLights(uint32_t targetFlag, uint32_t objAddr, bool isAtomic, bool hasNormals)
	{
		FrameLights fl;
		fl.fogColor = buildLights.fogColor;
		fl.fogStart = buildLights.fogStart;
		fl.fogEnd = buildLights.fogEnd;
		fl.ambient = glm::vec3(0.0f);
		fl.dirCount = 0;

		const uint32_t worldAddr = curWorld; // RW's curWorld, captured from the camera at begin-update
		fl.worldAddr = worldAddr;
		if (worldAddr)
		{
			// Circular list: nodes are each light's RpLight::inWorld link; the terminator is the list head
			// inside RpWorld. It holds both ambient and directional lights (discriminate on subType).
			const uint32_t terminator = worldAddr + offsetof(RpWorld, directionalLightList);
			uint32_t node = guest::ptr<RpWorld>(worldAddr)->directionalLightList.link.next.addr;
			for (int n = 0; node && node != terminator && n < LIGHT_WALK_CAP; n++)
			{
				const uint32_t lightAddr = node - offsetof(RpLight, inWorld);
				const auto light = guest::ptr<RpLight>(lightAddr);
				node = light->inWorld.next.addr; // advance before any early-out
				SKY2_TRACE_ONCE(lightAddr, "world 0x%X node 0x%X type %u colour %f,%f,%f flags 0x%X target 0x%X",
					worldAddr, lightAddr, light->object.object.subType, light->color.red, light->color.green,
					light->color.blue, light->object.object.flags, targetFlag);
				if (!(light->object.object.flags & targetFlag))
				{
					continue; // not for this object class (rpLIGHTLIGHTATOMICS vs rpLIGHTLIGHTWORLD)
				}
				const uint8_t type = light->object.object.subType; // RpLightGetType reads subType
				const glm::vec3 color(light->color.red, light->color.green, light->color.blue);
				if (type == rpLIGHTAMBIENT)
				{
					SKY2_TRACE_ONCE(lightAddr, "world 0x%X ambient light 0x%X colour %f,%f,%f flags 0x%X",
						worldAddr, lightAddr, color.r, color.g, color.b, light->object.object.flags);
					fl.ambient += color; // RW accumulates every ambient with the right target flag
				}
				else if (type == rpLIGHTDIRECTIONAL && fl.dirCount < MAX_DIR_LIGHTS)
				{
					fl.dirAddr[fl.dirCount] = lightAddr;
					fl.dirColor[fl.dirCount] = color;
					fl.dirDir[fl.dirCount] = dirLightToward(lightAddr);
					fl.dirCount++;
				}
				// positioned lights (type >= rpLIGHTPOSITIONINGSTART) come from the sector ties below
			}
		}
		// Positioned (point/spot) lights from the object's sector ties. The atomic path is normals-gated
		// and range-tested (§6.2); the sector path takes its whole tie list (§6.3).
		if (objAddr)
		{
			if (isAtomic)
			{
				if (hasNormals)
					gatherAtomicLocalLights(fl, objAddr);
			}
			else
			{
				gatherSectorLocalLights(fl, objAddr);
			}
		}
		// Debug-overlay holds for the pass being built, by slot (ambient, directional 0, directional 1).
		const int passIdx = static_cast<int>(buildFrame().passCount()) - 1;
		if (const auto c = lightLock(passIdx, 0))
			fl.ambient = *c;
		if (const auto c = lightLock(passIdx, 1); c && fl.dirCount >= 1)
			fl.dirColor[0] = *c;
		if (const auto c = lightLock(passIdx, 2); c && fl.dirCount >= 2)
			fl.dirColor[1] = *c;
		return fl;
	}

	// Read the render-state latches the game set for this draw (blend/cull/depth/fog). Shared by the
	// atomic and world-sector paths; both run with the game's render state already configured.
	void RenderQueue::readRenderStateInto(ParsedGeometry& out)
	{
		out.blend = blendState(); // reconciled GS blend (RW SRCBLEND/DESTBLEND or GS ALPHA register, GS wins)
		out.blend.enable = renderState[rwRENDERSTATEVERTEXALPHAENABLE] != 0;
		out.stencil = stencil;
		// A draw the game never set CULLMODE/ZTEST/ZWRITE for reads 0 from the zeroed state array; that
		// is not "disabled" but RW's default (no cull, depth on), so fall back when the latch is unhit.
		out.cull = rsHits[rwRENDERSTATECULLMODE] ? static_cast<int>(renderState[rwRENDERSTATECULLMODE]) : rwCULLMODECULLNONE;
		out.ztest = rsHits[rwRENDERSTATEZTESTENABLE] ? renderState[rwRENDERSTATEZTESTENABLE] != 0 : true;
		out.zwrite = rsHits[rwRENDERSTATEZWRITEENABLE] ? renderState[rwRENDERSTATEZWRITEENABLE] != 0 : true;
		out.fog = renderState[rwRENDERSTATEFOGENABLE] != 0;
	}

	void RenderQueue::enqueueSubMesh(const ParsedGeometry& pg, const SubMesh& sm)
	{
		MeshDraw m;
		m.lightsIndex = lightsIndexFor(currentLights); // set per object by the render callback (gatherLights)
		m.verts = pg.verts; // shared across the atomic's submeshes
		m.idx = sm.indices;
		// Stamp every reference, cache hits included: the pool reclaims a slot once the retire fence
		// passes its last use, so a missed stamp frees geometry a live frame still draws.
		geometry->verts.touch(m.verts);
		geometry->indices.touch(m.idx);
		m.model = pg.model;
		m.diag.rsCull = static_cast<uint8_t>(pg.cull);
		m.diag.objPipeline = pg.objPipeline;
		m.diag.matPipeline = sm.matPipeline;
		if (pipeDumpEnabled())
		{
			m.diag.objVuHash = dumpPipeline(pg.objPipeline).vuHash;
			m.diag.matVuHash = dumpPipeline(sm.matPipeline).vuHash;
		}
		m.material.color = toVec4(sm.materialBaseColor); // PS2 0x80=1.0 material-alpha scaling is applied in the shader
		m.material.lit = pg.lit;
		m.material.hasNormals = pg.hasNormals;
		m.material.modulate = sm.modulate;
		m.matfx.effect = sm.matfxEffect;
		m.matfx.enabled = pg.matfxEnabled;
		m.matfx.uvRow0 = sm.uvRow0;
		m.matfx.uvRow1 = sm.uvRow1;
		m.matfx.hasPass2 = sm.hasPass2;
		m.matfx.pass2TexKey = sm.pass2TexKey;
		m.matfx.pass2Blend = sm.pass2Blend;
		m.matfx.pass2Env = sm.pass2Env;
		m.matfx.pass2Coef = sm.pass2Coef;
		m.matfx.dualUvRow0 = sm.dualUvRow0;
		m.matfx.dualUvRow1 = sm.dualUvRow1;
		m.material.vcolor = pg.hasVertexColor;
		m.boneOffset = pg.boneOffset;
		m.boneCount = pg.skinned ? pg.boneCount : 0;
		// Blend predicate (RW 7.9), the PS2-applicable terms: the render-state VERTEXALPHAENABLE latch,
		// a translucent material colour, or non-opaque prelit alpha anywhere in this submesh. The last
		// is cached at parse rather than rescanned here, the way RW decides it when the geometry is
		// instanced.
		//
		// The texture term is the fourth: PRIM.ABE is set on every textured batch the GS draws in
		// p3-velvet-room, and source-alpha blending is a no-op only while the alpha is opaque, so a
		// texture carrying translucent texels has to blend. Without it the Velvet Room's gate renders
		// as a solid panel.
		DrawState st;
		st.blend = pg.blend;
		// The PS2 driver leaves PRIM.ABE set and lets an opaque alpha make the blend a no-op: across
		// the traced scenes the GS runs one unblended batch per frame at most, and Sonic Heroes runs
		// 234 blended against 1. Predicating it on the render state, the material, the prelit alpha and
		// the texture reproduced that only where one of the four happened to fire.
		st.blend.enable = true;
		st.stencil = pg.stencil;
		st.cull = pg.cull;
		st.ztest = pg.ztest;
		st.zwrite = pg.zwrite;
		st.fog = pg.fog;
		st.colorMask = colorMask;
		st.clamp = gsClamp;
		st.gsAlphaReg = gsBlend.reg.U64;
		st.atest = gsAtest;
		m.stateIndex = stateIndexFor(st);
		m.material.surfAmbient = sm.surfAmbient;
		m.material.surfDiffuse = sm.surfDiffuse;
		m.material.texKey = sm.texKey;
		m.material.sampler = sm.sampler;
		m.diag.atomicAddr = pg.sourceAddr;
		m.diag.geomFlags = pg.geomFlags;
		m.diag.numTexCoordSets = pg.numTexCoordSets;
		m.diag.texCoordsAddr = pg.texCoordsAddr;
		m.diag.source = pg.source;
		m.diag.native = pg.native;
		m.diag.materialKey = sm.materialKey;
		m.diag.generation = objects->generationOf(pg.sourceAddr);
		submitMesh(std::move(m));
	}
} // namespace sky2
