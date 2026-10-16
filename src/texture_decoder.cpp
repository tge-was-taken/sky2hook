// PS2 raster pixel decoders: swizzled paletted (PSMT8/PSMT4), direct 32-bit (PSMCT32), and
// Sofdec MPEG frames in EE RAM -> linear RGBA8888. Raw PS2 alpha (0..0x80) is kept; the shader
// scales it. Pure functions over guest memory; no renderer state.
#include "texture_decoder.h"

#include "sky2/guest.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace sky2
{
	void TextureDecoder::unswizzlePSMT8(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src, int w, int h)
	{
		dst.resize(static_cast<size_t>(w) * h);
		for (int y = 0; y < h; y++)
		{
			for (int x = 0; x < w; x++)
			{
				const int blockLoc = (y & (~0xf)) * w + (x & (~0xf)) * 2;
				const int swapSel = (((y + 2) >> 2) & 0x1) * 4;
				const int posY = (((y & (~3)) >> 1) + (y & 1)) & 0x7;
				const int colLoc = posY * w * 2 + ((x + swapSel) & 0x7) * 4;
				const int byteSel = ((y >> 1) & 1) + ((x >> 2) & 2);
				const size_t idx = static_cast<size_t>(blockLoc + colLoc + byteSel);
				dst[static_cast<size_t>(y) * w + x] = idx < src.size() ? src[idx] : 0;
			}
		}
	}

	// Read `count` CLUT entries as RGBA8888. An 8888 CLUT is copied straight through; a 1555 CLUT holds
	// 16-bit RGBA5551 entries, which the GS expands by shifting each 5-bit channel up 3 and mapping the
	// 1-bit alpha to the TEXA pair (the usual TA0=0, TA1=0x80, matching the raw PS2 alpha kept here).
	void TextureDecoder::readClut(uint32_t palAddr, int count, bool clut16, uint8_t (*out)[4])
	{
		if (clut16)
		{
			const auto pal = guest::ptr<uint16_t>(palAddr);
			for (int i = 0; i < count; i++)
			{
				const uint16_t e = pal[i];
				out[i][0] = uint8_t((e & 0x1F) << 3);
				out[i][1] = uint8_t(((e >> 5) & 0x1F) << 3);
				out[i][2] = uint8_t(((e >> 10) & 0x1F) << 3);
				out[i][3] = (e & 0x8000) ? 0x80 : 0x00;
			}
			return;
		}
		const auto pal = guest::ptr<uint32_t>(palAddr);
		for (int i = 0; i < count; i++)
		{
			const uint32_t e = pal[i];
			out[i][0] = e & 0xFF;
			out[i][1] = (e >> 8) & 0xFF;
			out[i][2] = (e >> 16) & 0xFF;
			out[i][3] = (e >> 24) & 0xFF; // raw PS2 alpha (0..0x80); scaled in the shader
		}
	}

	bool TextureDecoder::decodePSMT8(uint32_t pxAddr, uint32_t palAddr, int w, int h, std::vector<uint8_t>& out)
	{
		if (!pxAddr || !palAddr || w <= 0 || h <= 0 || w > 1024 || h > 1024)
		{
			return false;
		}
		std::vector<uint8_t> swizzled(static_cast<size_t>(w) * h);
		const auto pxBytes = guest::ptr<uint8_t>(pxAddr);
		for (size_t i = 0; i < swizzled.size(); i++)
		{
			swizzled[i] = pxBytes[static_cast<int32_t>(i)];
		}
		std::vector<uint8_t> indices;
		unswizzlePSMT8(indices, swizzled, w, h);
		uint8_t clut[256][4];
		readClut(palAddr, 256, false, clut);
		for (int i = 0; i < 256; i++)
		{
			if ((i & 0x18) == 0x10)
			{
				for (int k = 0; k < 4; k++)
				{
					std::swap(clut[i][k], clut[i ^ 0x18][k]);
				}
			}
		}
		out.resize(static_cast<size_t>(w) * h * 4);
		for (size_t p = 0; p < indices.size(); p++)
		{
			const uint8_t ci = indices[p];
			out[p * 4 + 0] = clut[ci][0];
			out[p * 4 + 1] = clut[ci][1];
			out[p * 4 + 2] = clut[ci][2];
			out[p * 4 + 3] = clut[ci][3];
		}
		return true;
	}

	bool TextureDecoder::decodePSMCT32(uint32_t pxAddr, int w, int h, std::vector<uint8_t>& out)
	{
		if (!pxAddr || w <= 0 || h <= 0 || w > 2048 || h > 2048)
		{
			return false;
		}
		out.resize(static_cast<size_t>(w) * h * 4);
		const auto pixels = guest::ptr<uint32_t>(pxAddr);
		for (size_t p = 0; p < static_cast<size_t>(w) * h; p++)
		{
			const uint32_t e = pixels[static_cast<int32_t>(p)];
			out[p * 4 + 0] = e & 0xFF; // R
			out[p * 4 + 1] = (e >> 8) & 0xFF; // G
			out[p * 4 + 2] = (e >> 16) & 0xFF; // B
			out[p * 4 + 3] = (e >> 24) & 0xFF; // A
		}
		return true;
	}

	int TextureDecoder::nextPow2(int v)
	{
		int p = 1;
		while (p < v)
			p <<= 1;
		return p;
	}

	// Detile a Sofdec/sceMpegGetPicture frame (16x16 macroblocks, column-major block order) into
	// linear RGBA8888. Output is sized to the GS allocation (next power of two) with the movie in
	// the top-left, since the game's Im2D UVs are normalized to that allocation.
	bool TextureDecoder::decodeMpegFrame(uint32_t bufAddr, int w, int h, int& outW, int& outH, std::vector<uint8_t>& out)
	{
		if (!bufAddr || w <= 0 || h <= 0 || (w % 16) || (h % 16) || w > 2048 || h > 2048)
			return false;
		outW = nextPow2(w);
		outH = nextPow2(h);
		out.assign(static_cast<size_t>(outW) * outH * 4, 0);
		const int blocksY = h / 16;
		const auto buf = guest::ptr<uint32_t>(bufAddr);
		for (int y = 0; y < h; y++)
		{
			const int blockRow = y / 16, by = y % 16;
			for (int x = 0; x < w; x++)
			{
				const int blockCol = x / 16, bx = x % 16;
				const uint32_t n = static_cast<uint32_t>(blockCol) * blocksY + blockRow;
				const uint32_t e = buf[static_cast<int32_t>(n * 256u + static_cast<uint32_t>(by) * 16u + bx)];
				const size_t d = (static_cast<size_t>(y) * outW + x) * 4;
				out[d + 0] = e & 0xFF;
				out[d + 1] = (e >> 8) & 0xFF;
				out[d + 2] = (e >> 16) & 0xFF;
				out[d + 3] = (e >> 24) & 0xFF;
			}
		}
		return true;
	}

	bool TextureDecoder::decodePSMT4(uint32_t pxAddr, uint32_t palAddr, bool clut16, int w, int h,
		std::vector<uint8_t>& out)
	{
		if (!pxAddr || !palAddr || w <= 0 || h <= 0 || w > 1024 || h > 1024)
		{
			return false;
		}
		// PS2 4-bit indices are swizzled with the SAME pattern as 8-bit: expand the packed nibbles to
		// one index per byte in stream order (low nibble first), then run the 8-bit unswizzle. This
		// is the RW/PS2 raster convention (Amicitia RWRasterDataStructNode.ReadIndices UnSwizzle8s
		// PSMT4 too); the old per-pixel PSMT4 swizzle produced a flat image for these textures.
		const size_t nbytes = static_cast<size_t>(w) * h / 2;
		std::vector<uint8_t> swizzled(static_cast<size_t>(w) * h);
		const auto pxBytes = guest::ptr<uint8_t>(pxAddr);
		for (size_t i = 0; i < nbytes; i++)
		{
			const uint8_t b = pxBytes[static_cast<int32_t>(i)];
			swizzled[i * 2 + 0] = b & 0x0F;
			swizzled[i * 2 + 1] = b >> 4;
		}
		std::vector<uint8_t> indices;
		unswizzlePSMT8(indices, swizzled, w, h);
		// 16-entry CLUT is linear (no 8x2 tiling, unlike the 256-entry PSMT8 CLUT).
		uint8_t clut[16][4];
		readClut(palAddr, 16, clut16, clut);
		out.resize(static_cast<size_t>(w) * h * 4);
		for (size_t p = 0; p < indices.size(); p++)
		{
			const uint8_t ci = indices[p] & 0x0F;
			out[p * 4 + 0] = clut[ci][0];
			out[p * 4 + 1] = clut[ci][1];
			out[p * 4 + 2] = clut[ci][2];
			out[p * 4 + 3] = clut[ci][3];
		}
		return true;
	}
} // namespace sky2
