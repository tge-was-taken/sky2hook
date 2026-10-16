#include "interpolator.h"

#include <cstdio>
#include <cstdlib>

namespace sky2
{
	// Interpolation is image-neutral when off and cannot prove itself by image when on, so these counts
	// are the only evidence it is doing the right thing. Printed per display frame, because the failure
	// that matters is one that only shows on the second and later frames of a tick.
	void Interpolator::logFrame() const
	{
		static const bool wanted = sky2::env("LOG_INTERP") != nullptr;
		if (!wanted || !enabled())
		{
			return;
		}
		std::fprintf(stderr,
			"[sky2][interp] tick %llu +%.2f %.1f ms (period %.2f)  mesh %d/%d/%d  skin %d/%d  im3d %d/%d  "
			"pass %d/%d/%d  im2d %d blend / %d static / %d snap (%d nomatch, %d cut, %d rest)  "
			"nonrigid %d (det %.3f, step %.1f) dropped %d\n",
			static_cast<unsigned long long>(ticks), alpha(), stats.tickMs, tickPeriodMs(), stats.meshInterpolated,
			stats.meshStatic, stats.meshSnapped, stats.bonesInterpolated, stats.bonesSnapped,
			stats.im3dInterpolated, stats.im3dSnapped, stats.passInterpolated, stats.passStatic,
			stats.passSnapped, stats.im2dBlended, stats.im2dStatic, stats.im2dSnapped, stats.im2dNoMatch,
			stats.im2dCut, stats.im2dFromRest, stats.nonRigid,
			stats.minBasisDet, stats.maxBoneStep, stats.droppedTicks);
	}
} // namespace sky2
