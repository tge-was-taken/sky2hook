#pragma once

#include <atomic>

namespace sky2::enhancements
{
	inline std::atomic<int> shadowDetail{1};

	void initShadowDetail();
	int offscreenTargetScale();
} // namespace sky2::enhancements
