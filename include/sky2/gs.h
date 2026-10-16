#pragma once
// PS2 GS register overlays (GIF register bitfields + PSM/TFX/CLAMP/ZTST/ATST/AFAIL/ALPHA enums) as
// the game writes them. Declared at global scope; names mirror the GS hardware registers.
#include <cstdint>

enum GS_PSM
{
	PSMCT32 = 0, // 0000-0000
	PSMCT24 = 1, // 0000-0001
	PSMCT16 = 2, // 0000-0010
	PSMCT16S = 10, // 0000-1010
	PSGPU24 = 18, // 0001-0010
	PSMT8 = 19, // 0001-0011
	PSMT4 = 20, // 0001-0100
	PSMT8H = 27, // 0001-1011
	PSMT4HL = 36, // 0010-0100
	PSMT4HH = 44, // 0010-1100
	PSMZ32 = 48, // 0011-0000
	PSMZ24 = 49, // 0011-0001
	PSMZ16 = 50, // 0011-0010
	PSMZ16S = 58, // 0011-1010
};

enum GS_TFX
{
	TFX_MODULATE = 0,
	TFX_DECAL = 1,
	TFX_HIGHLIGHT = 2,
	TFX_HIGHLIGHT2 = 3,
	TFX_NONE = 4,
};

enum GS_CLAMP
{
	CLAMP_REPEAT = 0,
	CLAMP_CLAMP = 1,
	CLAMP_REGION_CLAMP = 2,
	CLAMP_REGION_REPEAT = 3,
};

enum GS_ZTST
{
	ZTST_NEVER = 0,
	ZTST_ALWAYS = 1,
	ZTST_GEQUAL = 2,
	ZTST_GREATER = 3,
};

enum GS_ATST
{
	ATST_NEVER = 0,
	ATST_ALWAYS = 1,
	ATST_LESS = 2,
	ATST_LEQUAL = 3,
	ATST_EQUAL = 4,
	ATST_GEQUAL = 5,
	ATST_GREATER = 6,
	ATST_NOTEQUAL = 7,
};

// The ATST that passes exactly the fragments its index fails, so a renderer that cannot vary the
// write mask per fragment can draw the failing half as a second pass.
constexpr int ATST_COMPLEMENT[8] = {ATST_ALWAYS, ATST_NEVER, ATST_GEQUAL, ATST_GREATER,
	ATST_NOTEQUAL, ATST_LESS, ATST_LEQUAL, ATST_EQUAL};

enum GS_AFAIL
{
	AFAIL_KEEP = 0,
	AFAIL_FB_ONLY = 1,
	AFAIL_ZB_ONLY = 2,
	AFAIL_RGB_ONLY = 3,
};

enum GS_ALPHA_BITS
{
	ALPHA_ABD_CS = 0,
	ALPHA_ABD_CD = 1,
	ALPHA_C_AS = 0,
	ALPHA_C_AD = 1,
	ALPHA_C_FIX = 2,
};

#pragma pack(push, 1)
union GIFRegALPHA
{
	uint64_t U64;
	struct
	{
		uint32_t A : 2; // GS_ALPHA_BITS: Cs=ALPHA_ABD_CS, Cd=ALPHA_ABD_CD, 0=2
		uint32_t B : 2;
		uint32_t C : 2; // GS_ALPHA_BITS: As=ALPHA_C_AS, Ad=ALPHA_C_AD, FIX=ALPHA_C_FIX
		uint32_t D : 2;
		uint32_t _PAD1 : 24;
		uint32_t FIX : 8; // constant alpha; only meaningful when C == ALPHA_C_FIX
		uint32_t _PAD2 : 24;
	};
};
static_assert(sizeof(GIFRegALPHA) == 8, "GIFReg must be 8 bytes");

union GIFRegTEST
{
	uint64_t U64;
	struct
	{
		uint32_t ATE : 1; // alpha test enable
		uint32_t ATST : 3; // GS_ATST: pass condition for alpha vs AREF
		uint32_t AREF : 8; // PS2 alpha (0..0x80) compared against
		uint32_t AFAIL : 2; // GS_AFAIL: what a failing pixel does
		uint32_t DATE : 1;
		uint32_t DATM : 1;
		uint32_t ZTE : 1;
		uint32_t ZTST : 2; // GS_ZTST
		uint32_t _PAD1 : 13;
		uint32_t _PAD2 : 32;
	};
};
static_assert(sizeof(GIFRegTEST) == 8, "TEST must be 8 bytes");

union GIFRegCLAMP
{
	uint64_t U64;
	struct
	{
		uint64_t WMS : 2; // GS_CLAMP
		uint64_t WMT : 2;
		uint64_t MINU : 10; // REGION_CLAMP bounds; MSK/FIX under REGION_REPEAT
		uint64_t MAXU : 10;
		uint64_t MINV : 10;
		uint64_t MAXV : 10;
		uint64_t _PAD1 : 20;
	};
};
static_assert(sizeof(GIFRegCLAMP) == 8, "CLAMP must be 8 bytes");

union GIFRegFRAME
{
	uint64_t U64;
	struct
	{
		uint64_t FBP : 9;
		uint64_t _PAD1 : 7;
		uint64_t FBW : 6;
		uint64_t _PAD2 : 2;
		uint64_t PSM : 6; // GS_PSM
		uint64_t _PAD3 : 2;
		uint64_t FBMSK : 32; // per-bit write mask; a set bit means do not write
	};
};
static_assert(sizeof(GIFRegFRAME) == 8, "FRAME must be 8 bytes");
#pragma pack(pop)

// The FRAME write masks the game pokes around its screen effects.
constexpr uint32_t FBMSK_NONE = 0x00000000;
constexpr uint32_t FBMSK_ALPHA_ONLY = 0x00FFFFFF; // RGB masked, alpha written
constexpr uint32_t FBMSK_COLOR_ONLY = 0xFF000000; // alpha masked, RGB written
