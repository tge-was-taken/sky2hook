#pragma once

#include <atomic>

namespace sky2::enhancements
{
	inline std::atomic<int> displaySamples{1};
	inline std::atomic<int> offscreenSamples{1};

	void initMsaa();
	int displaySampleCount();
	int offscreenSampleCount();
} // namespace sky2::enhancements
