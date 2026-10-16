#include "interpolator.h"

#include "frame.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace sky2
{
	namespace
	{
		// Pixels a named 2D vertex may travel in a tick, in 640x448 source space. A tagged element that
		// jumped further is being placed somewhere else, not animating.
		constexpr float IM2D_CUT_LIMIT = 200.0f;
		// A textured sprite reads its image from an atlas cell, so a texture coordinate this far off
		// last tick's means the sprite is showing a different picture, not a moved one.
		constexpr float IM2D_CELL_LIMIT = 0.01f;
		// Pixels of travel in the previous tick below which a 2D element counts as having been at rest.
		constexpr float IM2D_AT_REST = 0.5f;

		// Anything outside this is a hitch or a stall rather than an update rate: a load, a savestate
		// restore, the render thread losing the CPU. Absolute, because a bound relative to the current
		// estimate cannot tell a wrong estimate from a wrong sample.
		constexpr double PERIOD_MIN_MS = 4.0, PERIOD_MAX_MS = 200.0;
		// Within that, an interval near the estimate refines it, and one far from it is an outlier until
		// enough arrive in a row to mean the rate itself moved. Without the re-seed the estimate locks
		// on wherever it started: the emulator running ahead of realtime publishes at 16.7 ms, every
		// sample reads as an outlier against a 33.4 ms estimate, and alpha never finishes a ramp.
		constexpr double PERIOD_NEAR = 0.4; // fraction either side that counts as near
		constexpr double PERIOD_TRACK_RATE = 0.02;
		constexpr int PERIOD_RESEED_AFTER = 4;

		// World units one bone may travel in a 30 Hz tick and still be the same pose. Measured walk and
		// idle peaks are 2-19; past this it is a cut, a re-pose, or two unrelated draws that matched.
		// P3FES_BONE_CUT retunes it, since the right value is a property of the animation data.
		float boneStepLimit()
		{
			static const float limit = []() {
				const char* set = sky2::env("BONE_CUT");
				return set ? std::strtof(set, nullptr) : 200.0f;
			}();
			return limit;
		}

		// P3FES_INTERP_ALPHA pins the blend so a capture lands at a known point in the tick. Negative
		// means follow the clock.
		float forcedAlpha()
		{
			static const float forced = []() {
				const char* set = sky2::env("INTERP_ALPHA");
				return set ? std::strtof(set, nullptr) : -1.0f;
			}();
			return forced;
		}

		struct RigidTransform
		{
			glm::vec3 translation{0.0f};
			glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
			glm::vec3 scale{1.0f};
		};

		// Fails for any matrix no rotation can be extracted from. With the scale divided out a pure
		// rotation has determinant 1; a mirror is negative, and a matrix that flattens its subject onto
		// a plane, which is what a projected shadow is, collapses to near zero.
		bool decomposeRigid(const glm::mat4& m, RigidTransform& out, float& basisDet)
		{
			const glm::vec3 x(m[0]), y(m[1]), z(m[2]);
			out.scale = {glm::length(x), glm::length(y), glm::length(z)};
			if (out.scale.x < 1e-6f || out.scale.y < 1e-6f || out.scale.z < 1e-6f)
			{
				basisDet = 0.0f;
				return false;
			}
			const glm::mat3 basis(x / out.scale.x, y / out.scale.y, z / out.scale.z);
			basisDet = glm::determinant(basis);
			if (basisDet < 0.9f)
			{
				return false;
			}
			out.translation = glm::vec3(m[3]);
			out.rotation = glm::quat_cast(basis);
			return true;
		}

		glm::mat4 lerpElements(const glm::mat4& a, const glm::mat4& b, float t)
		{
			glm::mat4 out;
			for (int column = 0; column < 4; column++)
			{
				out[column] = glm::mix(a[column], b[column], t);
			}
			return out;
		}

		glm::mat4 blend(const glm::mat4& a, const glm::mat4& b, float t, Interpolator::Stats& stats)
		{
			if (t >= 1.0f || a == b)
			{
				return b;
			}
			if (t <= 0.0f)
			{
				return a;
			}
			RigidTransform from, to;
			float fromDet = 0.0f, toDet = 0.0f;
			const bool rigid = decomposeRigid(a, from, fromDet) && decomposeRigid(b, to, toDet);
			stats.minBasisDet = std::min({stats.minBasisDet, fromDet, toDet});
			if (!rigid)
			{
				stats.nonRigid++;
				// A mirror lerps through a fully collapsed matrix, so it snaps. Everything else here
				// flattens onto a plane, and matrices sharing a null space span a linear subspace: the
				// floor does not move between ticks, so an element-wise blend stays on it and slides the
				// shadow along it, which is what a rigid decomposition cannot express.
				return std::min(fromDet, toDet) < 0.0f ? b : lerpElements(a, b, t);
			}
			glm::quat toRotation = to.rotation;
			if (glm::dot(from.rotation, toRotation) < 0.0f)
			{
				toRotation = -toRotation; // shortest arc
			}
			glm::mat4 out(glm::mat3_cast(glm::normalize(glm::slerp(from.rotation, toRotation, t))));
			const glm::vec3 scale = glm::mix(from.scale, to.scale, t);
			out[0] *= scale.x;
			out[1] *= scale.y;
			out[2] *= scale.z;
			out[3] = glm::vec4(glm::mix(from.translation, to.translation, t), 1.0f);
			return out;
		}

		// Rebuilt the way buildCameraVP builds it, so an endpoint reproduces the frame's own matrix
		// rather than something merely close to it.
		glm::mat4 composeViewProjection(const CameraParts& c, const glm::mat4& camWorld)
		{
			glm::mat4 proj(0.0f);
			proj[0][0] = -1.0f / c.viewWindowX;
			proj[1][1] = 1.0f / c.viewWindowY;
			if (c.projectionType == rwPARALLEL)
			{
				proj[2][2] = 2.0f / (c.farPlane - c.nearPlane);
				proj[3][2] = -(c.farPlane + c.nearPlane) / (c.farPlane - c.nearPlane);
				proj[3][3] = 1.0f;
			}
			else
			{
				proj[2][2] = (c.farPlane + c.nearPlane) / (c.farPlane - c.nearPlane);
				proj[2][3] = 1.0f;
				proj[3][2] = -2.0f * c.farPlane * c.nearPlane / (c.farPlane - c.nearPlane);
			}
			return proj * glm::inverse(camWorld);
		}

		// A named 2D element drawn several times a frame (a trail, one card per party member) keeps the
		// call order the guest emitted, so the tag plus which repeat it is identifies one of them. The
		// texture joins them because a name is often a pooled object address, and the pool hands the same
		// address to something else a tick later; a different sheet then misses instead of matching.
		uint64_t im2dKeyOf(const Im2DDraw& d, std::unordered_map<uint32_t, uint32_t>& seenTag)
		{
			const uint64_t tex = d.texKey.value_or(0);
			const uint64_t repeat = seenTag[d.interpTag]++;
			return (uint64_t(d.interpTag) << 32) | (repeat << 16) | ((tex ^ (tex >> 32)) & 0xFFFF);
		}

		// Which camera drew into which target, and which repeat of that pair this is. Position cannot
		// serve: the game opens a pass per character shadow, so the count moves with how many are in
		// range and every later pass shifts under it.
		uint32_t passKeyOf(uint32_t camera, uint32_t target, uint32_t occurrence)
		{
			uint32_t h = camera * 0x9E3779B9u;
			h = (h ^ target) * 0x85EBCA6Bu;
			h = (h ^ occurrence) * 0xC2B2AE35u;
			return h;
		}

		// The pass identities of one frame, by position, so both sides key their draws the same way.
		void buildPassKeys(const Frame& f, std::vector<uint32_t>& out)
		{
			out.clear();
			out.reserve(f.passes.size());
			std::unordered_map<uint64_t, uint32_t> seen;
			for (const FramePass& p : f.passes)
			{
				const uint64_t group = (uint64_t(p.camera) << 32) | p.target;
				out.push_back(passKeyOf(p.camera, p.target, seen[group]++));
			}
		}

		Interpolator::Key keyOf(const MeshDraw& m, uint32_t pass)
		{
			return {m.diag.atomicAddr, m.diag.materialKey, m.diag.generation,
				static_cast<uint16_t>(pass), m.diag.drawOrdinal};
		}
	} // namespace

	size_t Interpolator::KeyHash::operator()(const Key& k) const
	{
		uint64_t h = k.sourceAddr * 0x9E3779B97F4A7C15ull;
		h = (h ^ k.materialKey) * 0xC2B2AE3D27D4EB4Full;
		h = (h ^ k.generation) * 0x165667B19E3779F9ull;
		h = (h ^ (uint64_t(k.pass) << 20) ^ k.drawOrdinal) * 0x9E3779B97F4A7C15ull;
		return static_cast<size_t>(h);
	}

	void Interpolator::setEnabled(bool enable)
	{
		if (on.exchange(enable) == enable)
		{
			return;
		}
		// Drop the history either way: turning on must not blend from a tick that is now old, and
		// turning off must snap to the current pose rather than leave a half-interpolated frame.
		prevModels.clear();
		prevBones.clear();
		prevIm3D.clear();
		prevIm2D.clear();
		prevIm2DVerts.clear();
		prevPoses.clear();
		prevTransient.clear();
		prevPasses.clear();
		prevPassKeys.clear();
		prevPassByKey.clear();
		pendingModels.clear();
		pendingBones.clear();
		pendingIm3D.clear();
		pendingIm2D.clear();
		pendingIm2DVerts.clear();
		pendingPoses.clear();
		pendingTransient.clear();
		pendingPasses.clear();
		pendingPassKeys.clear();
		currPassKeys.clear();
		boneScratch.clear();
		transientScratch.clear();
		im2dScratch.clear();
		havePrev = havePending = false;
		capturedIndex = 0;
	}

	void Interpolator::captureFrameBeingReplaced(const Frame& f)
	{
		if (!enabled() || f.index == 0 || f.index == capturedIndex)
		{
			return;
		}
		capturedIndex = f.index;
		pendingModels.clear();
		pendingBones.clear();
		pendingIm3D.clear();
		pendingIm2D.clear();
		pendingPoses.assign(f.poses.begin(), f.poses.end());
		pendingTransient.assign(f.transientVerts.begin(), f.transientVerts.end());
		pendingIm2DVerts.assign(f.im2dVerts.begin(), f.im2dVerts.end());
		// Im2D draws are walked in submission order here and in im2dVertsFor, so a tag's repeat counter
		// means the same thing on both sides.
		std::unordered_map<uint32_t, uint32_t> seenTag;
		for (const Im2DDraw& d : f.im2dCmds)
		{
			if (d.interpTag == 0)
			{
				continue;
			}
			// prevIm2D still holds the tick before this one, so this is the step that brought the draw
			// here. A draw with no match there counts as at rest, which is what a new element is.
			const uint64_t key = im2dKeyOf(d, seenTag);
			float step = 0.0f;
			const auto before = prevIm2D.find(key);
			if (before != prevIm2D.end() && before->second.count == d.vertCount &&
				before->second.offset + d.vertCount <= prevIm2DVerts.size())
			{
				for (uint32_t v = 0; v < d.vertCount; v++)
				{
					const Im2DVertex& was = prevIm2DVerts[before->second.offset + v];
					const Im2DVertex& now = f.im2dVerts[d.vertOffset + v];
					step = std::max({step, std::fabs(now.x - was.x), std::fabs(now.y - was.y)});
				}
			}
			pendingIm2D.emplace(key, Slice{d.vertOffset, d.vertCount, 0, step});
		}
		pendingPasses.clear();
		pendingPasses.reserve(f.passes.size());
		buildPassKeys(f, pendingPassKeys);
		uint32_t pass = 0;
		for (const FramePass& p : f.passes)
		{
			pendingPasses.push_back({p.camera, p.target, p.cam});
			for (uint32_t c = p.cmdBegin; c < p.cmdEnd; c++)
			{
				if (f.commandList[c].type != CmdType::Mesh)
				{
					continue;
				}
				const MeshDraw& m = f.meshCmds[f.commandList[c].data];
				const Key key = keyOf(m, pendingPassKeys[pass]);
				pendingModels.emplace(key, m.model);
				if (m.boneCount > 0 && m.boneOffset + m.boneCount <= pendingPoses.size())
				{
					pendingBones.emplace(key, Slice{m.boneOffset, m.boneCount, 0});
				}
				if (m.verts == GEOM_ID_NONE && m.vertCount > 0 &&
					m.vertOffset + m.vertCount <= pendingTransient.size())
				{
					pendingIm3D.emplace(key, Slice{m.vertOffset, m.vertCount, m.material.texKey.value_or(0)});
				}
			}
			pass++;
		}
		havePending = true;
	}

	void Interpolator::beginTick(const Frame& f)
	{
		if (havePending)
		{
			prevModels.swap(pendingModels);
			prevBones.swap(pendingBones);
			prevIm3D.swap(pendingIm3D);
			prevIm2D.swap(pendingIm2D);
			prevIm2DVerts.swap(pendingIm2DVerts);
			prevPoses.swap(pendingPoses);
			prevTransient.swap(pendingTransient);
			prevPasses.swap(pendingPasses);
			prevPassKeys.swap(pendingPassKeys);
			prevPassByKey.clear();
			for (size_t i = 0; i < prevPasses.size() && i < prevPassKeys.size(); i++)
			{
				prevPassByKey.emplace(prevPassKeys[i], prevPasses[i]);
			}
			havePrev = true;
			havePending = false;
		}
		// The captured frame is the one this tick replaced. Anything further back is not the update
		// before this one, and blending across it moves the world by two updates in the time of one.
		if (havePrev && capturedIndex + 1 != f.index)
		{
			stats.droppedTicks++;
			havePrev = false;
		}
		ticks++;

		const Clock::time_point now = Clock::now();
		const auto period = std::chrono::duration<double, std::milli>(periodMs);
		const Clock::time_point expected = tickStart + std::chrono::duration_cast<Clock::duration>(period);
		const double errorMs = std::chrono::duration<double, std::milli>(now - expected).count();
		const bool phaseLost = tickStart.time_since_epoch().count() == 0 || std::fabs(errorMs) > periodMs;
		// Correct a twentieth of the error per tick: fast enough to track drift, far too slow to pass
		// the per-tick jitter into alpha.
		// Resync to the end of a period, not its start. Starting over at `now` puts alpha back at 0,
		// which draws the pose we already left: a hitch would be followed by a step backwards.
		tickStart = phaseLost ? now - std::chrono::duration_cast<Clock::duration>(period)
							  : expected + std::chrono::duration_cast<Clock::duration>(
											   std::chrono::duration<double, std::milli>(errorMs * 0.05));

		const double measuredMs = lastTick.time_since_epoch().count()
									  ? std::chrono::duration<double, std::milli>(now - lastTick).count()
									  : 0.0;
		lastTick = now;
		// Track the rate the emulator is really publishing at. Intervals far from the current estimate
		// are a dropped or doubled tick rather than a rate change, and averaging those in would drag the
		// estimate toward whatever the last hitch looked like.
		if (measuredMs > PERIOD_MIN_MS && measuredMs < PERIOD_MAX_MS)
		{
			if (std::fabs(measuredMs - periodMs) <= periodMs * PERIOD_NEAR)
			{
				periodMs += (measuredMs - periodMs) * PERIOD_TRACK_RATE;
				offRate = 0;
			}
			else if (++offRate >= PERIOD_RESEED_AFTER)
			{
				periodMs = measuredMs; // the rate moved, not the sample
				offRate = 0;
			}
		}
		stats = Stats{};
		stats.tickMs = measuredMs;
	}

	// One clock read per displayed frame. Held through the frame so every draw blends to the same
	// point, and so the cost is a single read rather than one per draw path.
	void Interpolator::beginDisplayFrame(const Frame& f)
	{
		buildPassKeys(f, currPassKeys);
		if (!enabled() || !havePrev)
		{
			displayAlpha = 1.0f;
			return;
		}
		if (const float forced = forcedAlpha(); forced >= 0.0f)
		{
			displayAlpha = std::min(forced, 1.0f);
			return;
		}
		const double elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - tickStart).count();
		displayAlpha = static_cast<float>(std::clamp(elapsedMs / periodMs, 0.0, 1.0));
	}

	glm::mat4 Interpolator::modelFor(const MeshDraw& m, uint32_t pass, const glm::mat4& curr) const
	{
		if (!enabled() || !havePrev)
		{
			return curr;
		}
		if (pass >= currPassKeys.size())
		{
			stats.meshSnapped++;
			return curr;
		}
		const auto prev = prevModels.find(keyOf(m, currPassKeys[pass]));
		if (prev == prevModels.end())
		{
			stats.meshSnapped++; // new this tick: blending it in from anywhere would fly it across
			return curr;
		}
		if (prev->second == curr)
		{
			stats.meshStatic++;
			return curr;
		}
		stats.meshInterpolated++;
		return blend(prev->second, curr, alpha(), stats);
	}

	const std::vector<glm::mat4>& Interpolator::bonesFor(const Frame& f) const
	{
		if (currPassKeys.size() != f.passes.size())
		{
			return f.bones; // keys built for another frame
		}
		const float t = alpha();
		if (!enabled() || !havePrev || f.bones.empty() || f.poses.size() != f.bones.size() || t >= 1.0f)
		{
			return f.bones;
		}
		boneScratch.assign(f.bones.begin(), f.bones.end());
		std::unordered_set<uint32_t> recomposed;
		uint32_t pass = 0;
		for (const FramePass& p : f.passes)
		{
			for (uint32_t c = p.cmdBegin; c < p.cmdEnd; c++)
			{
				if (f.commandList[c].type != CmdType::Mesh)
				{
					continue;
				}
				const MeshDraw& m = f.meshCmds[f.commandList[c].data];
				// One object's submeshes name the same slice, so each is recomposed once.
				if (m.boneCount == 0 || !recomposed.insert(m.boneOffset).second)
				{
					continue;
				}
				const auto prev = prevBones.find(keyOf(m, currPassKeys[pass]));
				if (prev == prevBones.end() || prev->second.count != m.boneCount ||
					prev->second.offset + prev->second.count > prevPoses.size())
				{
					stats.bonesSnapped++;
					continue;
				}
				float step = 0.0f;
				for (uint16_t b = 0; b < m.boneCount; b++)
				{
					step = std::max(step, glm::length(glm::vec3(f.poses[m.boneOffset + b][3]) -
													  glm::vec3(prevPoses[prev->second.offset + b][3])));
				}
				stats.maxBoneStep = std::max(stats.maxBoneStep, step);
				if (step > boneStepLimit())
				{
					stats.bonesSnapped++;
					continue;
				}
				stats.bonesInterpolated++;
				for (uint16_t b = 0; b < m.boneCount; b++)
				{
					// Blend the pose, then re-apply the bind matrix. Blending the product instead mixes
					// the constant bind offset into the interpolated translation and shortens limbs.
					boneScratch[m.boneOffset + b] =
						blend(prevPoses[prev->second.offset + b], f.poses[m.boneOffset + b], t, stats) *
						f.invBinds[m.boneOffset + b];
				}
			}
			pass++;
		}
		return boneScratch;
	}

	const std::vector<Vertex3D>& Interpolator::transientVertsFor(const Frame& f) const
	{
		if (currPassKeys.size() != f.passes.size())
		{
			return f.transientVerts; // keys built for another frame
		}
		const float t = alpha();
		if (!enabled() || !havePrev || f.transientVerts.empty() || t >= 1.0f)
		{
			return f.transientVerts;
		}
		transientScratch.assign(f.transientVerts.begin(), f.transientVerts.end());
		uint32_t pass = 0;
		for (const FramePass& p : f.passes)
		{
			for (uint32_t c = p.cmdBegin; c < p.cmdEnd; c++)
			{
				if (f.commandList[c].type != CmdType::Mesh)
				{
					continue;
				}
				const MeshDraw& m = f.meshCmds[f.commandList[c].data];
				if (m.verts != GEOM_ID_NONE || m.vertCount == 0)
				{
					continue;
				}
				const auto prev = prevIm3D.find(keyOf(m, currPassKeys[pass]));
				// Same corner count and same source texture, or it is a different draw that happened to
				// land at the same position in the pass.
				if (prev == prevIm3D.end() || prev->second.count != m.vertCount ||
					prev->second.texKey != m.material.texKey.value_or(0) ||
					prev->second.offset + prev->second.count > prevTransient.size())
				{
					stats.im3dSnapped++;
					continue;
				}
				stats.im3dInterpolated++;
				for (uint32_t v = 0; v < m.vertCount; v++)
				{
					const Vertex3D& from = prevTransient[prev->second.offset + v];
					Vertex3D& out = transientScratch[m.vertOffset + v];
					// The UV is where this corner samples the caster's render target, so holding it
					// while the corner moves slides the silhouette across the quad.
					out.pos = glm::mix(from.pos, out.pos, t);
					out.uv = glm::mix(from.uv, out.uv, t);
					out.uv2 = glm::mix(from.uv2, out.uv2, t);
				}
			}
			pass++;
		}
		return transientScratch;
	}

	// Only draws a game hook named are blended, and a named draw is blended whole: position, texture
	// coordinate and colour together. The heuristic this replaces accepted any draw whose vertices all
	// moved by one delta, which is exactly what a line of text does when it grows by a character, so it
	// blended one glyph into the next. No host-side rule separates the two; only the guest knows.
	const std::vector<Im2DVertex>& Interpolator::im2dVertsFor(const Frame& f) const
	{
		const float t = alpha();
		if (!enabled() || !havePrev || f.im2dVerts.empty() || t >= 1.0f)
		{
			return f.im2dVerts;
		}
		im2dScratch.assign(f.im2dVerts.begin(), f.im2dVerts.end());
		std::unordered_map<uint32_t, uint32_t> seenTag;
		for (const Im2DDraw& d : f.im2dCmds)
		{
			if (d.interpTag == 0)
			{
				continue;
			}
			const auto prev = prevIm2D.find(im2dKeyOf(d, seenTag));
			if (prev == prevIm2D.end() || prev->second.count != d.vertCount ||
				prev->second.offset + prev->second.count > prevIm2DVerts.size())
			{
				stats.im2dSnapped++;
				stats.im2dNoMatch++;
				continue;
			}
			// An untextured draw's texture coordinates mean nothing, so only a textured one is held to
			// showing the same atlas cell as last tick.
			const float cellLimit = d.texKey ? IM2D_CELL_LIMIT : std::numeric_limits<float>::max();
			bool moved = false;
			bool cut = false;
			for (uint32_t v = 0; v < d.vertCount && !cut; v++)
			{
				const Im2DVertex& from = prevIm2DVerts[prev->second.offset + v];
				const Im2DVertex& to = f.im2dVerts[d.vertOffset + v];
				cut = std::fabs(to.x - from.x) > IM2D_CUT_LIMIT ||
					  std::fabs(to.y - from.y) > IM2D_CUT_LIMIT ||
					  std::fabs(to.u - from.u) > cellLimit || std::fabs(to.v - from.v) > cellLimit;
				moved = moved || to.x != from.x || to.y != from.y;
			}
			if (cut)
			{
				stats.im2dSnapped++;
				stats.im2dCut++;
				continue;
			}
			if (!moved)
			{
				stats.im2dStatic++;
				continue;
			}
			// Only smooth what was already in motion. A cursor stepping to the next row moves from rest,
			// and blending that turns a deliberate jump into a slide. The cost is that the first tick of
			// an animation steps, which is one frame.
			if (prev->second.step < IM2D_AT_REST)
			{
				stats.im2dSnapped++;
				stats.im2dFromRest++;
				continue;
			}
			stats.im2dBlended++;
			for (uint32_t v = 0; v < d.vertCount; v++)
			{
				const Im2DVertex& from = prevIm2DVerts[prev->second.offset + v];
				Im2DVertex& out = im2dScratch[d.vertOffset + v];
				const Im2DVertex to = out;
				out.x = std::lerp(from.x, to.x, t);
				out.y = std::lerp(from.y, to.y, t);
				out.u = std::lerp(from.u, to.u, t);
				out.v = std::lerp(from.v, to.v, t);
				out.r = std::lerp(from.r, to.r, t);
				out.g = std::lerp(from.g, to.g, t);
				out.b = std::lerp(from.b, to.b, t);
				out.a = std::lerp(from.a, to.a, t);
			}
		}
		return im2dScratch;
	}

	glm::mat4 Interpolator::vpFor(const FramePass& p, uint32_t pass) const
	{
		if (!enabled() || !havePrev || pass >= currPassKeys.size())
		{
			return p.vp;
		}
		const auto found = prevPassByKey.find(currPassKeys[pass]);
		if (found == prevPassByKey.end())
		{
			stats.passSnapped++;
			return p.vp;
		}
		const PassPose& prev = found->second;
		if (!prev.cam.valid || !p.cam.valid)
		{
			stats.passSnapped++;
			return p.vp;
		}
		if (prev.cam.camWorld == p.cam.camWorld && prev.cam.viewWindowX == p.cam.viewWindowX)
		{
			stats.passStatic++;
			return p.vp;
		}
		const float t = alpha();
		if (t >= 1.0f)
		{
			stats.passSnapped++;
			return p.vp;
		}
		stats.passInterpolated++;
		CameraParts blended = p.cam;
		blended.viewWindowX = glm::mix(prev.cam.viewWindowX, p.cam.viewWindowX, t);
		blended.viewWindowY = glm::mix(prev.cam.viewWindowY, p.cam.viewWindowY, t);
		blended.nearPlane = glm::mix(prev.cam.nearPlane, p.cam.nearPlane, t);
		blended.farPlane = glm::mix(prev.cam.farPlane, p.cam.farPlane, t);
		return composeViewProjection(blended, blend(prev.cam.camWorld, p.cam.camWorld, t, stats));
	}
} // namespace sky2
