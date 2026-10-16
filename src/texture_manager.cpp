#include "texture_manager.h"

#include <array>

#include "common.h"
#include "gl.h"
#include "object_manager.h" // objects->traceGsResidentRaster
#include "render_queue.h"
#include "renderer.h"
#include "rw_to_gl.h"
#include "texture_decoder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

// Header-only, the same way PCSX2's own GS texture cache pulls it in. FNV-1a is a serial multiply
// chain, so it ran at memory-latency speed rather than memory-bandwidth speed; XXH3 is vectorized.
// The values are only ever compared against other values from this function, so changing it is free.
#define XXH_INLINE_ALL 1
#include <xxhash.h>
#include <cstdlib>

namespace sky2
{
	static uint64_t hashGuestRange(uint32_t addr, uint32_t byteCount, uint64_t seed)
	{
		const uint32_t base = addr & guest::RAM_MASK;
		const uint32_t ramSize = guest::RAM_MASK + 1;
		if (base + byteCount > ramSize)
		{
			SKY2_ERROR(false, "attempt to read past end of EE ram: addr=%08X byteCount=%08X", addr, byteCount);
			byteCount = ramSize - base;
		}
		return XXH3_64bits_withSeed(guest::host(addr, byteCount), byteCount, seed);
	}

	static uint64_t getRwRasterHash(uint32_t pxAddr, uint32_t palAddr, int w, int h, int depth, bool clut16)
	{
		if (w <= 0 || h <= 0 || w > 2048 || h > 2048)
		{
			SKY2_ERROR(false, "invalid dims %dx%d depth=%d", w, h, depth);
			return 0; // never matches a cached signature, so the caller re-attempts the decode
		}
		uint64_t s = (14695981039346656037ull ^ (uint64_t(w) << 20) ^ (uint64_t(h) << 4) ^ uint64_t(depth));
		const uint32_t pixBytes = depth == 32 ? uint32_t(w) * h * 4 : depth == 8 ? uint32_t(w) * h :
		                                                                           uint32_t(w) * h / 2;
		s = hashGuestRange(pxAddr, pixBytes, s);
		if (palAddr)
		{
			s = hashGuestRange(palAddr, (depth == 8 ? 256u : 16u) * (clut16 ? 2u : 4u), s); // CLUT entries * entry size
		}
		return s;
	}

	static void dumpTexture(TextureId key, uint32_t srcPx, int w, int h, int depth, int cFmt,
		const std::vector<uint8_t>& rgba)
	{
		static std::unordered_set<TextureId> dumped;
		static int count = 0;
		if (count >= 600 || !dumped.insert(key).second) // the key is the content hash: one dump per content
		{
			return;
		}
		count++;
		char path[256];
		// Named by the content key, which is what the capture manifest prints as "tex", so a draw joins
		// to its texture by string match. The source address follows for finding the pixels in RAM.
		std::snprintf(path, sizeof(path), "experiments/dumps/tex_%016llX_px%08X_%dx%d_d%d_f%02X.ppm",
			static_cast<unsigned long long>(key), srcPx, w, h, depth, cFmt);
		if (FILE* f = std::fopen(path, "wb"))
		{
			std::fprintf(f, "P6\n%d %d\n255\n", w, h);
			for (int i = 0; i < w * h; i++)
			{
				std::fwrite(&rgba[static_cast<size_t>(i) * 4], 1, 3, f);
			}
			std::fclose(f);
		}
		// Alpha channel as a grayscale .pgm (raw decoded 0..0x80 PS2 alpha), so transparency issues
		// (an overlay that should be a cutout but decodes opaque) are visible next to the RGB.
		std::snprintf(path, sizeof(path), "experiments/dumps/tex_%016llX_px%08X_%dx%d_d%d_f%02X.pgm",
			static_cast<unsigned long long>(key), srcPx, w, h, depth, cFmt);
		if (FILE* f = std::fopen(path, "wb"))
		{
			std::fprintf(f, "P5\n%d %d\n255\n", w, h);
			for (int i = 0; i < w * h; i++)
			{
				std::fwrite(&rgba[static_cast<size_t>(i) * 4 + 3], 1, 1, f);
			}
			std::fclose(f);
		}
	}

	std::optional<TextureId> TextureManager::convertRaster(uint32_t rasterAddr, unsigned filterAddressing)
	{
		if (!rasterAddr)
		{
			return std::nullopt;
		}
		// The frame buffers the game samples for feedback live at fixed EE globals (the displayed one and
		// the one being drawn into, each a stable pointer). Register from there rather
		// than from the RpSkyGet*BufferRaster hooks, which fire once and are then cached by the game, so
		// they never re-fire on a savestate reload. Guarded, so this is a read plus a set lookup per call.
		const std::pair<uint32_t, ObjectManager::BufferRole> globals[] = {
			{sky2::addresses().skyDisplayRaster, ObjectManager::BufferRole::Display},
			{sky2::addresses().skyDrawRaster, ObjectManager::BufferRole::Draw}};
		for (const auto& [global, role] : globals)
		{
			if (!global)
			{
				continue;
			}
			if (const uint32_t db = *guest::ptr<uint32_t>(global);
				db && !objects->frameBufferRasters.count(db))
			{
				objects->registerFrameBufferRaster(db, role, "frame-buffer global");
			}
		}
		// The game bound a frame buffer as a texture for a screen effect (framebuffer feedback): sample
		// what we rendered rather than the (GS-resident) PS2 pixels. Which of the pair it bound is the
		// whole answer. sky2 keeps skyDisplayRaster and skyDrawRaster as two permanently distinct
		// rasters and only swaps which GS block each points at on a flip (basky.c's
		// _rpSkyFrameBufferRastersUpdate), so the display raster always means the frame already
		// presented and the draw raster the frame so far.
		if (objects->frameBufferRasters.count(rasterAddr))
		{
			if (objects->frameBufferRole(rasterAddr) == ObjectManager::BufferRole::Display)
			{
				objects->prevFrameWanted.store(true); // the render thread starts keeping the copy
				return DISPLAY_BUFFER_FEEDBACK_TEX_ID;
			}
			return DRAW_BUFFER_FEEDBACK_TEX_ID;
		}
		// A raster the game renders into and now samples (portrait/effect RT): sample its FBO color
		// attachment rather than decoding PS2 pixels (there may be none). The render thread resolves
		// this tagged id to the target's GL texture (see finishOffscreenTarget).
		if (objects->isRenderTargetRaster(rasterAddr))
		{
			static const bool sLogRt = sky2::env("LOG_RT") != nullptr;
			if (sLogRt)
			{
				const auto rr = guest::ptr<RwRaster>(rasterAddr);
				SKY2_TRACE_ONCE(rasterAddr, "RT-classified raster 0x%X: cpPixels=0x%X %dx%d cType=0x%X cFmt=0x%X",
					rasterAddr, uint32_t(rr->cpPixels.addr), int32_t(rr->width), int32_t(rr->height),
					uint8_t(rr->cType), uint8_t(rr->cFormat));
			}
			return rasterAddr | RENDER_TARGET_TEX_BIT;
		}
		const auto raster = guest::ptr<RwRaster>(rasterAddr);
		const int depth = int32_t(raster->depth);
		const uint8_t cType = uint8_t(raster->cType);
		const uint8_t cFmt = uint8_t(raster->cFormat);
		const uint32_t pxAddr = raster->cpPixels.addr;
		const uint32_t palAddr = raster->palette.addr;
		const int tw = int32_t(raster->width);
		const int th = int32_t(raster->height);
		// Mipmap flags (0x80 MIPMAP, 0x10 AUTOMIPMAP) don't change the base-level decode (we only
		// upload level 0), so mask them off before matching the pixel+CLUT format.
		const uint8_t pixFmt = cFmt & 0x6F;
		const bool psmt8 = depth == 8 && pixFmt == ps2FMT_PAL8_8888;
		const bool clut16 = pixFmt == ps2FMT_PAL4_1555; // 16-bit CLUT entries instead of 32-bit
		const bool psmt4 = depth == 4 && (pixFmt == ps2FMT_PAL4_8888 || clut16);
		const bool psmct32 = depth == 32 && pixFmt == ps2FMT_8888; // direct 32-bit (FMV, 32-bit art)

		// FMV / GS-resident 32-bit texture: pixels live in GS VRAM (cpPixels=0), but the Sofdec
		// decoder left the frame in EE RAM. HACK: assumes ONE active movie, routed to objects->movieBuf
		// under a fixed 'MOVI' key; a cleaner fix would map each movie raster to its own buffer.
		if (psmct32 && pxAddr == 0 && objects->movieBuf != 0 && tw > 0 && th > 0)
		{
			const TextureId movieKey = 0x4D4F5649; // 'MOVI'
			const auto cached = decoded.find(movieKey);
			if (cached != decoded.end() && cached->second.uploadedSig == objects->movieFrameId)
				return movieKey; // this movie frame is already uploaded
			std::vector<uint8_t> movieRGBA;
			int mtW = 0, mtH = 0;
			if (TextureDecoder::decodeMpegFrame(objects->movieBuf, tw, th, mtW, mtH, movieRGBA))
			{
				enqueueUpload(movieKey, objects->movieBuf, mtW, mtH, movieRGBA.data(), filterAddressing);
				decoded[movieKey].uploadedSig = objects->movieFrameId;
				return movieKey;
			}
		}
		// A camera texture holds what was rendered into it, so its EE allocation is uninitialized unless
		// the CPU filled it (RasterUnlock). Decoding that allocation yields noise, so route it to the
		// render-target path: sample the target's FBO, or log the miss if the game filled it through a
		// GS copy we don't intercept.
		if ((cType & rwRASTERTYPEMASK) == rwRASTERTYPECAMERATEXTURE && !objects->cpuWrittenRasters.count(rasterAddr))
		{
			SKY2_TRACE_ONCE(rasterAddr, "camera-texture raster 0x%X sampled with no camera pass of ours: "
										 "its EE pixels are not the image (%dx%d cFormat=0x%02X)",
				rasterAddr, tw, th, cFmt);
			return rasterAddr | RENDER_TARGET_TEX_BIT;
		}
		// Any other GS-resident texture (cpPixels=0) is a gap: we have no EE pixels to read. Trace its
		// origin once per raster (not an error/abort: it is a known gap, not a code bug).
		if (!pxAddr)
		{
			objects->traceGsResidentRaster(rasterAddr, depth, cType, cFmt, tw, th);
			return std::nullopt;
		}
		const bool handled = psmt8 || psmt4 || psmct32;
		if (!handled)
		{
			SKY2_ERROR(false, "unhandled texture format depth=%d cType=0x%02X cFormat=0x%02X pal=%d",
				depth, cType, cFmt, palAddr != 0);
			return std::nullopt;
		}
		{
			const uint32_t key = (uint32_t(cType) << 16) | (uint32_t(cFmt) << 8) | uint32_t(depth & 0xFF);
			SKY2_TRACE_ONCE(key, "texture format depth=%d cType=0x%02X cFormat=0x%02X (decoded)",
				depth, cType, cFmt);
		}
		if ((psmt8 || psmt4) && !palAddr)
		{
			SKY2_ERROR(false, "paletted texture has no CLUT: depth=%d cType=0x%02X cFormat=0x%02X",
				depth, cType, cFmt);
			return std::nullopt;
		}
		// Re-decode only when the content changed (signature miss); reuse otherwise.
		// Past this many changes at one address, the pixels are a stream, not a texture.
		constexpr uint32_t STREAM_AFTER_CHANGES = 4;
		const uint32_t hashPal = (psmt8 || psmt4) ? palAddr : 0;
		Decoded& entry = decoded[pxAddr];
		uint64_t sig = 0;
		if (entry.hashedSig && entry.palAddr == hashPal)
		{
			sig = entry.hashedSig; // nothing has told us these pixels changed since we hashed them
			bumpStat(stats.hashMemoHits);
		}
		else
		{
			const uint64_t prevSig = entry.hashedSig;
			{
				ScopedNs timer{stats.hashNs};
				sig = getRwRasterHash(pxAddr, hashPal, tw, th, depth, clut16);
			}
			if (prevSig && prevSig != sig && ++entry.contentChanges == STREAM_AFTER_CHANGES)
			{
				bumpStat(stats.streamingAddrs); // this address just stopped being worth caching
			}
			entry.palAddr = hashPal;
			entry.hashedSig = sig;
			const uint32_t pixBytes = depth == 32 ? uint32_t(tw) * th * 4 :
			                          depth == 8  ? uint32_t(tw) * th :
			                                        uint32_t(tw) * th / 2;
			bumpStat(stats.hashBytes, pixBytes);
			bumpStat(stats.hashCalls);
		}
		// The key is the content, not the address. Reserve the tag bits so a hash can never look like a
		// render-target, feedback or streaming id. An address whose pixels keep changing is the
		// exception: content keying would mint an entry per frame, each dead the moment it is drawn,
		// and the age sweep cannot retire them fast enough to hold the budget.
		const bool streaming = entry.contentChanges >= STREAM_AFTER_CHANGES;
		const TextureId key = streaming ? (STREAMING_TEX_BIT | pxAddr) : (sig & ~TEX_KEY_TAG_MASK);
		if (streaming)
		{
			if (entry.uploadedSig == sig)
			{
				bumpStat(stats.contentHits);
				return key; // the frame we already uploaded into this address's texture
			}
		}
		else if (const auto have = content.find(key); have != content.end())
		{
			have->second.lastUsedFrame = frameIndex;
			entry.uploadedSig = sig;
			bumpStat(stats.contentHits);
			return key; // already resident, wherever it was decoded from: no decode, no upload
		}
		std::vector<uint8_t> texRGBA;
		bool ok = false;
		{
			ScopedNs timer{stats.decodeNs};
			ok = psmct32 ? TextureDecoder::decodePSMCT32(pxAddr, tw, th, texRGBA) :
			     psmt8   ? TextureDecoder::decodePSMT8(pxAddr, palAddr, tw, th, texRGBA) :
			               TextureDecoder::decodePSMT4(pxAddr, palAddr, clut16, tw, th, texRGBA);
		}
		if (!ok)
		{
			SKY2_ERROR(false, "texture decode failed: depth=%d cType=0x%02X cFormat=0x%02X %dx%d",
				depth, cType, cFmt, tw, th);
			return std::nullopt;
		}
		bumpStat(stats.decodes);
		if (sky2::config().texLog && (psmt4 || psmt8))
		{
			// The palette as it sits in guest RAM, next to the spread of indices that read it. A flat
			// decode is either flat art or a palette read that landed on the wrong bytes, and only
			// these two together tell them apart.
			const int entries = psmt4 ? 16 : 256;
			char clutText[8 * 16 + 1] = {};
			int at = 0;
			for (int e = 0; e < 16; e++)
			{
				const uint32_t word = *guest::ptr<uint32_t>(palAddr + uint32_t(e) * (clut16 ? 2u : 4u));
				at += std::snprintf(clutText + at, sizeof(clutText) - size_t(at), "%08X ", word);
			}
			std::array<int, 256> used{};
			for (size_t px = 0; px * 4 < texRGBA.size(); px++)
			{
				used[size_t(texRGBA[px * 4])] = 1; // red channel stands in for "some entry landed here"
			}
			SKY2_TRACE_ONCE(key, "tex %016llX %dx%d d%d f%02X px=0x%08X pal=0x%08X clut[%d] first16: %s",
				static_cast<unsigned long long>(key), tw, th, depth, cFmt, pxAddr, palAddr, entries,
				clutText);
		}
		enqueueUpload(key, pxAddr, tw, th, texRGBA.data(), filterAddressing);
		// A streaming key overwrites one GL texture, so it never joins the content cache and never
		// enters the byte budget: there is nothing to keep and nothing to sweep.
		if (!streaming)
		{
			const uint32_t bytes = static_cast<uint32_t>(size_t(tw) * size_t(th) * 4 * 4 / 3); // + mips
			constexpr uint8_t PS2_OPAQUE = 0x80;
			bool translucent = false;
			for (size_t i = 3; i < texRGBA.size() && !translucent; i += 4)
			{
				translucent = texRGBA[i] < PS2_OPAQUE;
			}
			content[key] = {frameIndex, bytes, translucent};
			contentBytes += bytes;
		}
		entry.uploadedSig = sig;
		if (sky2::config().texLog)
		{
			dumpTexture(key, pxAddr, tw, th, depth, cFmt, texRGBA);
		}
		return key;
	}

	void TextureManager::invalidate(uint32_t rasterAddr)
	{
		if (!rasterAddr)
			return;
		const uint32_t pxAddr = guest::ptr<RwRaster>(rasterAddr)->cpPixels.addr;
		decoded.erase(pxAddr); // re-hash and re-decode on the next bind
	}

	std::optional<TextureId> TextureManager::convertTexture(uint32_t textureAddr)
	{
		if (!textureAddr)
		{
			return std::nullopt;
		}
		const auto texture = guest::ptr<RwTexture>(textureAddr);
		const auto raster = texture->raster;
		if (!raster)
		{
			SKY2_ERROR(false, "RwTexture 0x%X has no raster", textureAddr);
			return std::nullopt;
		}
		return convertRaster(raster.addr, uint32_t(texture->filterAddressing));
	}

	void TextureManager::enqueueUpload(TextureId key, uint32_t srcPx, int w, int h, const unsigned char* rgba, unsigned filterAddressing)
	{
		// A negative w/h would underflow the size_t below into a huge allocation; a null rgba is a
		// null-pointer read. Both mean the caller (a texture decode) already produced bad data.
		if (!rgba || w <= 0 || h <= 0)
		{
			SKY2_ERROR(false, "invalid args (key=0x%016llX w=%d h=%d rgba=%p)",
				static_cast<unsigned long long>(key), w, h, static_cast<const void*>(rgba));
			return;
		}
		PendingTex t;
		t.key = key;
		t.srcPx = srcPx;
		t.w = w;
		t.h = h;
		t.rgba.assign(rgba, rgba + static_cast<size_t>(w) * h * 4);
		t.wrapS = rwAddressToGL((filterAddressing >> 8) & 0xF);
		t.wrapT = rwAddressToGL((filterAddressing >> 12) & 0xF);
		rwFilterToGL(filterAddressing & 0xFF, t.minFilter, t.magFilter);
		std::lock_guard<std::mutex> lk(mutex);
		pendingUpload.push_back(std::move(t));
	}

	// EE thread. What we knew about these pixels goes here, where that map is owned; the render thread
	// only frees GL objects.
	void TextureManager::enqueueDestroy(uint32_t rasterAddr, uint32_t pxAddr)
	{
		decoded.erase(pxAddr);
		std::lock_guard<std::mutex> lk(mutex);
		pendingDestroy.emplace_back(rasterAddr, pxAddr);
	}

	std::vector<std::pair<uint32_t, uint32_t>> TextureManager::takePendingDestroy()
	{
		std::vector<std::pair<uint32_t, uint32_t>> dead;
		std::lock_guard<std::mutex> lk(mutex);
		dead.swap(pendingDestroy);
		return dead;
	}

	// The byte total follows the entry, so dropping one place drops both.
	void TextureManager::dropEntry(TextureId key, bool deleteGl)
	{
		const auto it = texCache.find(key);
		if (it == texCache.end())
		{
			return;
		}
		if (deleteGl && it->second.id)
		{
			glDeleteTextures(1, &it->second.id);
		}
		texBytes -= it->second.bytes;
		texCache.erase(it);
	}

	// Render thread. A destroyed raster only stops one address referring to its pixels, and the pixels
	// themselves may still be referenced from elsewhere (the common case, measured), so releasing
	// content is the sweep's job. A streaming texture is the exception: it belongs to this address
	// alone and nothing else will ever sample it.
	void TextureManager::evictTexture(uint32_t pxAddr)
	{
		dropEntry(STREAMING_TEX_BIT | pxAddr, true);
	}

	// EE thread. Content is only worth keeping while something still draws it, but "still drawn" is not
	// observable, so age stands in for it: past the budget, drop whatever has gone longest unused.
	void TextureManager::beginFrame()
	{
		frameIndex++;
		stats.contentLive.store(content.size(), std::memory_order_relaxed);
		sweepContent();
	}

	// Drop least-recently-used content until back under budget. A minimum age cannot bound this: an FMV
	// mints a frame's worth every frame, so nothing is ever old enough and the cache runs away.
	void TextureManager::sweepContent()
	{
		const size_t budgetBytes = static_cast<size_t>(std::max(1, sky2::config().texCacheMb)) << 20;
		const size_t lowWaterBytes = budgetBytes * 3 / 4; // sweep past the budget so this is rare
		constexpr uint64_t IN_FLIGHT_FRAMES = 4; // a frame the render thread still holds may bind it
		if (contentBytes <= budgetBytes)
		{
			return;
		}
		std::vector<std::pair<uint64_t, TextureId>> byAge; // (lastUsedFrame, key)
		byAge.reserve(content.size());
		for (const auto& [key, c] : content)
		{
			if (frameIndex - c.lastUsedFrame >= IN_FLIGHT_FRAMES)
			{
				byAge.emplace_back(c.lastUsedFrame, key);
			}
		}
		std::sort(byAge.begin(), byAge.end());
		std::vector<TextureId> dropped;
		for (const auto& [lastUsedFrame, key] : byAge)
		{
			if (contentBytes <= lowWaterBytes)
			{
				break;
			}
			contentBytes -= content[key].bytes;
			content.erase(key);
			dropped.push_back(key);
		}
		if (!dropped.empty())
		{
			std::lock_guard<std::mutex> lk(mutex);
			pendingEvict.insert(pendingEvict.end(), dropped.begin(), dropped.end());
			stats.contentSwept.store(
				stats.contentSwept.load(std::memory_order_relaxed) + dropped.size(), std::memory_order_relaxed);
		}
	}

	// The GL texture was freed with its FBO, so only the bookkeeping goes here.
	void TextureManager::eraseRenderTargetTex(uint32_t rasterAddr)
	{
		dropEntry(rasterAddr | RENDER_TARGET_TEX_BIT, false);
	}

	void TextureManager::uploadPending()
	{
		// P3FES_LOG_TEXCHURN, render side: whether the cache converges or only ever grows. Eviction
		// happens on raster destroy, so a scene that never destroys a raster never releases anything.
		static const bool sLogChurn = sky2::env("LOG_TEXCHURN") != nullptr;
		if (sLogChurn)
		{
			static int calls = 0;
			if (++calls % 300 == 0)
			{
				const auto ms = [](const std::atomic<uint64_t>& c) {
					return c.load(std::memory_order_relaxed) / 1.0e6;
				};
				const CacheBreakdown b = breakdown();
				std::fprintf(stderr,
					"[sky2][texcache] GL textures: %zu content (%.1f MB) %zu streaming (%.1f MB) "
					"%zu RT %zu feedback\n",
					b.content, double(b.contentBytes) / (1024.0 * 1024.0), b.streaming,
					double(b.streamingBytes) / (1024.0 * 1024.0), b.renderTarget, b.feedback);
				std::fprintf(stderr,
					"[sky2][texcache] live=%zu bytes=%.1f MB evicted=%llu | total hash=%.0f ms "
					"decode=%.0f ms upload=%.0f ms over %llu hashes (%.0f MB) + %llu memo hits | content %llu resident, %llu hits\n",
					texCache.size(), uploadedBytes() / (1024.0 * 1024.0),
					static_cast<unsigned long long>(stats.evictions.load(std::memory_order_relaxed)),
					ms(stats.hashNs), ms(stats.decodeNs), ms(stats.uploadNs),
					static_cast<unsigned long long>(stats.hashCalls.load(std::memory_order_relaxed)),
					double(stats.hashBytes.load(std::memory_order_relaxed)) / (1024.0 * 1024.0),
					static_cast<unsigned long long>(stats.hashMemoHits.load(std::memory_order_relaxed)),
					static_cast<unsigned long long>(content.size()),
					static_cast<unsigned long long>(stats.contentHits.load(std::memory_order_relaxed)));
			}
		}
		std::vector<PendingTex> pending;
		{
			std::lock_guard<std::mutex> lk(mutex);
			pending.swap(pendingUpload);
		}
		// Content the sweep retired: free the GL here, where the context lives.
		{
			std::vector<TextureId> evict;
			{
				std::lock_guard<std::mutex> lk(mutex);
				evict.swap(pendingEvict);
			}
			for (const TextureId key : evict)
			{
				stats.evictions.store(stats.evictions.load(std::memory_order_relaxed) + 1,
					std::memory_order_relaxed);
				dropEntry(key, true);
			}
		}
		if (pending.empty())
		{
			return; // nothing to time, and the common case by far
		}
		ScopedNs uploadTimer{stats.uploadNs}; // the uploads themselves, not the bookkeeping above
		for (const PendingTex& t : pending)
		{
			// Reuse the GL texture if this key was re-queued (RasterUnlock invalidated it), so
			// re-decoded pixels overwrite the old ones instead of leaking a texture.
			GlTexture& entry = texCache[t.key];
			entry.srcPx = t.srcPx;
			const bool isNew = entry.id == 0;
			if (isNew)
			{
				glGenTextures(1, &entry.id);
			}
			const GLuint tex = entry.id;
			glBindTexture(GL_TEXTURE_2D, tex);
			if (isNew)
			{
				char label[40];
				std::snprintf(label, sizeof(label), "tex 0x%016llX", static_cast<unsigned long long>(t.key));
				gl::labelObject(GL_TEXTURE, tex, label);
			}
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.rgba.data());
			glGenerateMipmap(GL_TEXTURE_2D);
			// Re-decoded at a new size: swap this key's contribution rather than adding to it.
			const size_t bytes = size_t(t.w) * size_t(t.h) * 4 * 4 / 3; // + mip chain
			texBytes += bytes - entry.bytes;
			entry.bytes = bytes;
		}
	}

	GLuint TextureManager::samplerFor(unsigned filterAddressing, bool magnified)
	{
		const unsigned key = filterAddressing | (magnified ? 0x10000u : 0u);
		const auto it = samplers.find(key);
		if (it != samplers.end())
		{
			return it->second;
		}
		GLint minFilter = GL_LINEAR_MIPMAP_LINEAR, magFilter = GL_LINEAR;
		rwFilterToGL(filterAddressing & 0xFFu, minFilter, magFilter);
		if (magnified && sky2::config().upscaleFilter)
		{
			// Only the magnification filter: minification already has the texels it needs.
			magFilter = GL_LINEAR;
		}
		GLuint sampler = 0;
		glGenSamplers(1, &sampler);
		glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, minFilter);
		glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, magFilter);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, rwAddressToGL((filterAddressing >> 8) & 0xFu));
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, rwAddressToGL((filterAddressing >> 12) & 0xFu));
		samplers.emplace(key, sampler);
		return sampler;
	}

	GLuint TextureManager::lookup(TextureId key) const
	{
		const auto it = texCache.find(key);
		return it != texCache.end() ? it->second.id : 0;
	}

	uint32_t TextureManager::sourceAddress(TextureId key) const
	{
		const auto it = texCache.find(key);
		return it != texCache.end() ? it->second.srcPx : 0;
	}

	bool TextureManager::hasTranslucentTexels(TextureId key) const
	{
		const auto it = content.find(key);
		return it != content.end() && it->second.translucent;
	}

	// An RT colour attachment or feedback copy: the FBO owns the GL texture, so `bytes` stays 0 and it
	// contributes nothing to uploadedBytes().
	void TextureManager::putTexture(TextureId key, GLuint tex, size_t bytes)
	{
		GlTexture& entry = texCache[key];
		texBytes += bytes - entry.bytes;
		entry.id = tex;
		entry.bytes = bytes;
	}

	TextureManager::CacheBreakdown TextureManager::breakdown() const
	{
		CacheBreakdown b;
		for (const auto& [key, entry] : texCache)
		{
			if (isRenderTargetTexId(key))
			{
				b.renderTarget++;
			}
			else if (isRendererOwnedTexId(key))
			{
				b.feedback++;
			}
			else if (isStreamingTexId(key))
			{
				b.streaming++;
				b.streamingBytes += entry.bytes;
			}
			else
			{
				b.content++;
				b.contentBytes += entry.bytes;
			}
		}
		return b;
	}

	std::vector<TextureId> TextureManager::renderTargetKeys() const
	{
		std::vector<TextureId> keys;
		for (const auto& [key, entry] : texCache)
			if (key & RENDER_TARGET_TEX_BIT)
				keys.push_back(key);
		return keys;
	}

	void TextureManager::destroyGl()
	{
		for (auto& [key, entry] : texCache)
			if (entry.id)
				glDeleteTextures(1, &entry.id);
		texCache.clear();
		for (auto& [fa, sampler] : samplers)
			glDeleteSamplers(1, &sampler);
		samplers.clear();
		texBytes = 0;
	}
} // namespace sky2
