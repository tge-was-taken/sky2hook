#include "feedback_blur.h"

#include "sky2/config.h" // sky2::Config: feedback_blur

namespace sky2::enhancements
{
	bool FeedbackBlurRemovalEnhancement::on()
	{
		return !sky2::config().feedbackBlur;
	}

	bool FeedbackBlurRemovalEnhancement::redirects(TextureId texKey)
	{
		return texKey == DISPLAY_BUFFER_FEEDBACK_TEX_ID && on();
	}

	TextureId FeedbackBlurRemovalEnhancement::sourceFor(TextureId texKey) const
	{
		return redirects(texKey) ? DRAW_BUFFER_FEEDBACK_TEX_ID : texKey;
	}

	FeedbackBlurRemovalEnhancement::Rescale FeedbackBlurRemovalEnhancement::rescaleFor(
		TextureId texKey, const Blend& blend, bool destReadable)
	{
		// A tap that leaves the destination whole compounds frame over frame; one that fades it by the
		// factor it adds by (GS ALPHA 0x54) settles at its own source, so rescaling it would brighten.
		const bool keepsDestination = blend.equation == GL_FUNC_ADD && blend.dst == GL_ONE;
		if (!redirects(texKey) || !keepsDestination)
		{
			return Rescale::None;
		}
		if (blend.src == GL_SRC_ALPHA) // GS ALPHA 0x48
		{
			return Rescale::SrcAlpha;
		}
		if (blend.src == GL_DST_ALPHA && destReadable) // GS ALPHA 0x58
		{
			return Rescale::DestAlpha;
		}
		return Rescale::None;
	}

	void FeedbackBlurRemovalEnhancement::init(GLuint prog)
	{
		uRescale = glGetUniformLocation(prog, "uFeedbackFixedPoint");
		uDestTex = glGetUniformLocation(prog, "uDestTex");
		glProgramUniform1i(prog, uDestTex, DEST_TEX_UNIT); // sampler unit is program state, set once
	}

	void FeedbackBlurRemovalEnhancement::applyRescale(
		TextureId texKey, const Blend& blend, bool destReadable, GLuint passColorTex)
	{
		const Rescale rescale = rescaleFor(texKey, blend, destReadable);
		if (const int mode = static_cast<int>(rescale); uploaded != mode)
		{
			glUniform1i(uRescale, mode);
			uploaded = mode;
		}
		if (rescale == Rescale::DestAlpha)
		{
			glTextureBarrier(); // uDestTex is the attachment this draw writes
			glActiveTexture(GL_TEXTURE0 + DEST_TEX_UNIT);
			glBindTexture(GL_TEXTURE_2D, passColorTex);
			glActiveTexture(GL_TEXTURE0);
		}
	}
} // namespace sky2::enhancements
