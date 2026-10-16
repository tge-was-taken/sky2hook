#include "shadow_detail.h"

#include "sky2/config.h" // sky2::Config: shadow_rt_scale

#include <algorithm>

namespace sky2::enhancements
{
	namespace
	{
		// 1024x1024
		constexpr int MAX_SHADOW_DETAIL = 8;
	} // namespace

	void initShadowDetail()
	{
		shadowDetail.store(sky2::config().shadowRtScale, std::memory_order_relaxed);
	}

	int offscreenTargetScale()
	{
		return std::clamp(shadowDetail.load(std::memory_order_relaxed), 1, MAX_SHADOW_DETAIL);
	}
} // namespace sky2::enhancements
