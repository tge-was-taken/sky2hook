#include "rw_to_gl.h"

#include "common.h" // SKY2_ERROR + the rw* enum constants (renderware.h)

namespace sky2
{
	GsBlend gsAlphaToBlend(GIFRegALPHA reg)
	{
		GsBlend b;
		b.reg = reg;
		if (reg.C == ALPHA_C_FIX)
		{
			b.constAlpha = ps2AlphaToUnit(reg.FIX);
		}
		const unsigned cf = reg.C == ALPHA_C_AS ? GL_SRC_ALPHA : reg.C == ALPHA_C_AD ? GL_DST_ALPHA :
		                                                                               GL_CONSTANT_ALPHA;
		const unsigned cf1m = reg.C == ALPHA_C_AS ? GL_ONE_MINUS_SRC_ALPHA : reg.C == ALPHA_C_AD ? GL_ONE_MINUS_DST_ALPHA :
		                                                                                           GL_ONE_MINUS_CONSTANT_ALPHA;
		// A=Cs, B=Cd, C=FIX, D=Cs: (Cs-Cd)*FIX + Cs = Cs*(FIX+1) - Cd*FIX. No GL blend factor equals
		// "FIX+1", so prescale the source alpha by (FIX+1) in the shader (uAlphaPremul) and let the
		// fixed-function stage subtract Cd*FIX. Same technique PCSX2 uses for this exact GS blend
		// combo (its own GSDevice.cpp blend table, entry "0120": BLEND_A_MAX, SUBTRACT, ONE, CONST).
		if (reg.A == ALPHA_ABD_CS && reg.B == ALPHA_ABD_CD && reg.C == ALPHA_C_FIX && reg.D == ALPHA_ABD_CS)
		{
			b.equation = GL_FUNC_SUBTRACT;
			b.src = GL_ONE;
			b.dst = GL_CONSTANT_ALPHA;
			b.alphaPremul = 1.0f + b.constAlpha;
			return b;
		}
		// A=Cs, B=Cd, D=Cd: (Cs-Cd)*C + Cd = src*C + dst*(1-C) -- standard "over" (0x44)
		if (reg.A == ALPHA_ABD_CS && reg.B == ALPHA_ABD_CD && reg.D == ALPHA_ABD_CD)
		{
			b.src = cf;
			b.dst = cf1m;
			return b;
		}
		// A=Cs, B=zero, D=Cd: Cs*C + Cd -- additive (0x48)
		if (reg.A == ALPHA_ABD_CS && reg.B == 2 && reg.D == ALPHA_ABD_CD)
		{
			b.src = cf;
			b.dst = GL_ONE;
			return b;
		}
		// A=zero, B=Cs, D=Cd: Cd - Cs*C -- subtractive (0x42), needs reverse-subtract
		if (reg.A == 2 && reg.B == ALPHA_ABD_CS && reg.D == ALPHA_ABD_CD)
		{
			b.equation = GL_FUNC_REVERSE_SUBTRACT; // dst*df - src*sf
			b.src = cf;
			b.dst = GL_ONE;
			return b;
		}
		// A=Cs, B=zero, D=zero: Cs*C -- modulate, no dest contribution
		if (reg.A == ALPHA_ABD_CS && reg.B == 2 && reg.D == 2)
		{
			b.src = cf;
			b.dst = GL_ZERO;
			return b;
		}
		// A=zero, B=zero, D=Cd: Cd -- result is the dest (draw contributes nothing visible)
		if (reg.A == 2 && reg.B == 2 && reg.D == ALPHA_ABD_CD)
		{
			b.src = GL_ZERO;
			b.dst = GL_ONE;
			return b;
		}
		SKY2_ERROR(false, "unhandled GS ALPHA blend 0x%llX (A=%u B=%u C=%u D=%u)",
			static_cast<unsigned long long>(reg.U64), reg.A, reg.B, reg.C, reg.D);
		return b; // SKY2_ERROR reports without stopping: leave the source-alpha defaults
	}

	GsAtest gsTestToAtest(GIFRegTEST reg)
	{
		GsAtest a;
		a.on = reg.ATE != 0 && reg.AFAIL == AFAIL_KEEP;
		a.enabled = reg.ATE != 0;
		a.method = reg.ATST;
		a.ref = reg.AREF / 255.0f; // unscaled; the shader applies the PS2 unit scale
		a.afail = reg.AFAIL;
		// GS destination-alpha test: the draw only writes where the frame buffer's own alpha matches
		// DATM. Nothing in GL reads dest alpha, so this is carried per draw and resolved at draw time.
		a.date = reg.DATE != 0;
		a.datm = reg.DATM != 0;
		a.reg = reg;
		return a;
	}

	GLenum rwBlendToGL(unsigned b, const char* what)
	{
		switch (b)
		{
			case rwBLENDZERO: return GL_ZERO;
			case rwBLENDONE: return GL_ONE;
			case rwBLENDSRCCOLOR: return GL_SRC_COLOR;
			case rwBLENDINVSRCCOLOR: return GL_ONE_MINUS_SRC_COLOR;
			case rwBLENDSRCALPHA: return GL_SRC_ALPHA;
			case rwBLENDINVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
			case rwBLENDDESTALPHA: return GL_DST_ALPHA;
			case rwBLENDINVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
			case rwBLENDDESTCOLOR: return GL_DST_COLOR;
			case rwBLENDINVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
			case rwBLENDSRCALPHASAT: return GL_SRC_ALPHA_SATURATE;
			default:
				// Hex as well as decimal: a value this far out of range is usually not a blend at all
				// but something else written into the slot, and the bit pattern is what says so.
				SKY2_ERROR(false, "unhandled RwBlendFunction %u (0x%08X) from %s, falling back to "
								   "GL_ONE_MINUS_SRC_ALPHA",
					b, b, what);
				return GL_ONE_MINUS_SRC_ALPHA;
		}
	}

	GLenum rwStencilOpToGL(unsigned op)
	{
		switch (op)
		{
			case rwSTENCILOPERATIONKEEP: return GL_KEEP;
			case rwSTENCILOPERATIONZERO: return GL_ZERO;
			case rwSTENCILOPERATIONREPLACE: return GL_REPLACE;
			case rwSTENCILOPERATIONINCRSAT: return GL_INCR;
			case rwSTENCILOPERATIONDECRSAT: return GL_DECR;
			case rwSTENCILOPERATIONINVERT: return GL_INVERT;
			case rwSTENCILOPERATIONINCR: return GL_INCR_WRAP;
			case rwSTENCILOPERATIONDECR: return GL_DECR_WRAP;
			default: SKY2_ERROR(false, "unhandled RwStencilOperation %u", op); return GL_KEEP;
		}
	}

	// RwStencilFunction -> GL comparison func (same order as GL_NEVER..GL_ALWAYS).
	GLenum rwStencilFuncToGL(unsigned f)
	{
		switch (f)
		{
			case rwSTENCILFUNCTIONNEVER: return GL_NEVER;
			case rwSTENCILFUNCTIONLESS: return GL_LESS;
			case rwSTENCILFUNCTIONEQUAL: return GL_EQUAL;
			case rwSTENCILFUNCTIONLESSEQUAL: return GL_LEQUAL;
			case rwSTENCILFUNCTIONGREATER: return GL_GREATER;
			case rwSTENCILFUNCTIONNOTEQUAL: return GL_NOTEQUAL;
			case rwSTENCILFUNCTIONGREATEREQUAL: return GL_GEQUAL;
			case rwSTENCILFUNCTIONALWAYS: return GL_ALWAYS;
			default: SKY2_ERROR(false, "unhandled RwStencilFunction %u", f); return GL_ALWAYS;
		}
	}

	GLint rwAddressToGL(unsigned a)
	{
		switch (a)
		{
			case rwTEXTUREADDRESSWRAP: return GL_REPEAT;
			case rwTEXTUREADDRESSMIRROR: return GL_MIRRORED_REPEAT;
			case rwTEXTUREADDRESSCLAMP: return GL_CLAMP_TO_EDGE;
			case rwTEXTUREADDRESSBORDER: return GL_CLAMP_TO_BORDER;
			default:
				SKY2_ERROR(false, "unhandled RwTextureAddressMode %u", a);
				return GL_REPEAT;
		}
	}

	void rwFilterToGL(unsigned f, GLint& min, GLint& mag)
	{
		const bool linear = (f == rwFILTERLINEAR || f == rwFILTERLINEARMIPNEAREST || f == rwFILTERLINEARMIPLINEAR); // linear base (magnification) filter
		mag = linear ? GL_LINEAR : GL_NEAREST;
		// rwFILTERNEAREST / rwFILTERLINEAR are mip-none; only modes 3-6 sample a mip chain.
		switch (f)
		{
			case rwFILTERNEAREST: min = GL_NEAREST; break;
			case rwFILTERLINEAR: min = GL_LINEAR; break;
			case rwFILTERMIPNEAREST: min = GL_NEAREST_MIPMAP_NEAREST; break;
			case rwFILTERMIPLINEAR: min = GL_NEAREST_MIPMAP_LINEAR; break;
			case rwFILTERLINEARMIPNEAREST: min = GL_LINEAR_MIPMAP_NEAREST; break;
			case rwFILTERLINEARMIPLINEAR: min = GL_LINEAR_MIPMAP_LINEAR; break;
			default:
				SKY2_ERROR(false, "unhandled RwTextureFilterMode %u", f);
				min = GL_LINEAR_MIPMAP_LINEAR;
				mag = GL_LINEAR;
				break;
		}
	}
} // namespace sky2
