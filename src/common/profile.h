// P3FES_PROFILE: coarse render-thread phase timing. Each zone glFinishes on both ends, so the
// elapsed CPU time is that phase's real cost. Zones must be sequential, never nested: a zone inside
// another adds two GPU syncs per entry, which both double-counts and serializes work that would
// otherwise overlap, inflating the whole frame. For unperturbed totals use P3FES_LOG_FPS, which
// times the render loop without any glFinish.
// Header-only, render-thread only (no locking); inline vars hold the shared accumulators.
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <glad/gl.h>
#include <map>
#include <string>

namespace sky2::prof
{
	inline bool gEnabled = sky2::env("PROFILE") != nullptr;
	inline std::map<std::string, std::pair<double, long>> gBuckets; // name -> (summed ms, count)
	inline int gFrame = 0;
	inline int gPeriod = sky2::env("PROFILE_EVERY") ? std::atoi(sky2::env("PROFILE_EVERY")) : 120;

	inline double nowMs()
	{
		return std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now().time_since_epoch())
		    .count();
	}

	inline void add(const char* name, double ms)
	{
		auto& b = gBuckets[name];
		b.first += ms;
		b.second++;
	}

	// Time a sequential phase: glFinish on both ends so the elapsed CPU time is the phase's real cost.
	struct Zone
	{
		const char* name;
		double t0;
		explicit Zone(const char* n)
			: name(n)
			, t0(0.0)
		{
			if (gEnabled)
			{
				glFinish();
				t0 = nowMs();
			}
		}
		~Zone()
		{
			if (gEnabled)
			{
				glFinish();
				add(name, nowMs() - t0);
			}
		}
		Zone(const Zone&) = delete;
		Zone& operator=(const Zone&) = delete;
	};

	// End of frame: every gPeriod frames, print each bucket's per-frame average and reset.
	inline void frame()
	{
		if (!gEnabled)
			return;
		if (++gFrame < gPeriod)
			return;
		double total = 0.0;
		std::fprintf(stderr, "[prof] avg over %d frames (ms/frame, calls/frame):\n", gFrame);
		for (const auto& [name, v] : gBuckets)
		{
			std::fprintf(stderr, "  %-14s %7.3f  (%.1f/frame)\n", name.c_str(), v.first / gFrame,
				double(v.second) / gFrame);
			if (name[0] != '-') // '-' names are a subset of another bucket; don't double-count in SUM
				total += v.first;
		}
		std::fprintf(stderr, "  %-14s %7.3f\n", "SUM", total / gFrame);
		gBuckets.clear();
		gFrame = 0;
	}
} // namespace sky2::prof
