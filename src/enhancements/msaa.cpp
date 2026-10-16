#include "msaa.h"

#include "sky2/config.h" // sky2::Config: msaa / msaa_offscreen

#include <algorithm>

namespace sky2::enhancements
{
	namespace
	{
		// GL guarantees 4 and every desktop driver offers 8; a request past what the driver reports
		// leaves the framebuffer incomplete, which the renderer handles by falling back to one sample.
		constexpr int MAX_SAMPLES = 16;
	} // namespace

	void initMsaa()
	{
		displaySamples.store(sky2::config().msaa, std::memory_order_relaxed);
		offscreenSamples.store(sky2::config().msaaOffscreen, std::memory_order_relaxed);
	}

	int displaySampleCount()
	{
		return std::clamp(displaySamples.load(std::memory_order_relaxed), 1, MAX_SAMPLES);
	}

	int offscreenSampleCount()
	{
		return std::clamp(offscreenSamples.load(std::memory_order_relaxed), 1, MAX_SAMPLES);
	}
} // namespace sky2::enhancements
