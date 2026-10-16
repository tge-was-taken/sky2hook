// PS2 raster pixel decoders. Each reads guest RAM at pxAddr (+ palAddr for paletted formats) and
// writes linear RGBA8888 into `out` (raw PS2 alpha 0..0x80, scaled later in the shader). Returns
// false on a null address or out-of-range dimensions.
#pragma once

#include <cstdint>
#include <vector>

namespace sky2
{
	// Stateless PS2 pixel decoders, grouped as static methods.
	class TextureDecoder
	{
	public:
		static bool decodePSMT8(uint32_t pxAddr, uint32_t palAddr, int w, int h, std::vector<uint8_t>& out);
		// clut16: the CLUT holds 16-bit 1555 entries instead of 32-bit 8888 ones.
		static bool decodePSMT4(uint32_t pxAddr, uint32_t palAddr, bool clut16, int w, int h,
			std::vector<uint8_t>& out);
		static bool decodePSMCT32(uint32_t pxAddr, int w, int h, std::vector<uint8_t>& out);
		// Detile a Sofdec/sceMpegGetPicture frame (16x16 macroblocks) into linear RGBA at the GS pow2
		// allocation (outW/outH), with the movie in the top-left so the game's normalized UVs line up.
		static bool decodeMpegFrame(uint32_t bufAddr, int w, int h, int& outW, int& outH, std::vector<uint8_t>& out);

	private:
		static void readClut(uint32_t palAddr, int count, bool clut16, uint8_t (*out)[4]);
		static void unswizzlePSMT8(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src, int w, int h);
		static int nextPow2(int v);
	};
} // namespace sky2
