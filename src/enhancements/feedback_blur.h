#pragma once

#include "common.h" // Blend, TextureId

namespace sky2::enhancements
{
	// Drops the PS2 screen-effect motion trail (config feedback_blur). A tap on the presented frame
	// adds a fraction of the last frame every frame, so it trails behind moving geometry; this reads
	// the frame so far instead and rescales what compounded back to the steady state it converged to.
	// Owned by Im2DRenderer: the render thread is where both halves of that apply.
	class FeedbackBlurRemovalEnhancement
	{
	public:
		// Which alpha a tap compounds by, and so which one im2d.frag rescales with. Values are
		// im2d.frag's uFeedbackFixedPoint.
		enum class Rescale
		{
			None = 0,
			SrcAlpha = 1,
			DestAlpha = 2,
		};

		void init(GLuint prog);

		// The buffer a 2D draw samples, redirected when it taps the presented frame and this is on.
		TextureId sourceFor(TextureId texKey) const;
		// Set up im2d.frag to rescale this draw. `destReadable`: whether `passColorTex` can be fetched.
		void applyRescale(TextureId texKey, const Blend& blend, bool destReadable, GLuint passColorTex);

	private:
		static constexpr int NOT_UPLOADED = -1; // no Rescale value, so the first draw always uploads
		static constexpr GLint DEST_TEX_UNIT = 1;

		static bool on();
		static bool redirects(TextureId texKey);
		static Rescale rescaleFor(TextureId texKey, const Blend& blend, bool destReadable);

		GLint uRescale = -1, uDestTex = -1;
		int uploaded = NOT_UPLOADED;
	};
} // namespace sky2::enhancements
