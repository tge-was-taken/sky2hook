// sky2 texture manager: what the EE thread knows about each raster's pixels, the GL textures the
// render thread holds for them, and the two queues between. One instance, owned by the Device.
#pragma once

#include "common.h" // TextureId, PendingTex, GL types

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sky2
{
	class ObjectManager;

	class TextureManager
	{
	public:
		ObjectManager* objects = nullptr; // injected by Device (raster classification, movie frame, tracing)

		// ---- EE (build) thread ----
		// Decode a raster / texture to a cache key (uploading its pixels if the content changed), or
		// nullopt when there is nothing sampleable (GS-resident with no EE pixels, unhandled format).
		std::optional<TextureId> convertRaster(uint32_t rasterAddr, unsigned filterAddressing);
		std::optional<TextureId> convertTexture(uint32_t textureAddr);
		void invalidate(uint32_t rasterAddr); // pixels or CLUT changed: forget what we knew
		void beginFrame(); // EE thread: age the content cache and sweep it back under budget
		void enqueueDestroy(uint32_t rasterAddr, uint32_t pxAddr); // free the GL on the render thread

		// ---- render thread ----
		void uploadPending(); // drain the upload queue into the GL texture cache
		GLuint lookup(TextureId key) const; // decoded GL texture for a key, or 0
		uint32_t sourceAddress(TextureId key) const; // guest address the content came from, 0 if unknown
		// EE thread: does this texture have texels below PS2 opaque (0x80)? False for anything not in
		// the content cache (render targets, feedback taps, streaming frames).
		bool hasTranslucentTexels(TextureId key) const;
		// A sampler object for one packed filterAddressing, created on first use. Sampling has to live
		// apart from the texture because content keying shares one texture between rasters that sample
		// it differently; 17-29% of reuses measured wanted different filtering.
		// `magnified` promotes a point filter to bilinear (upscale_filter): the game asks for NEAREST on
		// art authored for 640x448, which only reads as blocky once the render is larger. Part of the
		// cache key, so a resize cannot hand back a sampler built for the other case.
		GLuint samplerFor(unsigned filterAddressing, bool magnified);
		void putTexture(TextureId key, GLuint tex, size_t bytes = 0); // publish an RT / feedback texture
		std::vector<std::pair<uint32_t, uint32_t>> takePendingDestroy(); // (raster, px) pairs, cleared
		void evictTexture(uint32_t pxAddr); // delete + drop the decoded texture keyed by pixel address
		void eraseRenderTargetTex(uint32_t rasterAddr); // drop the RT-tagged entry (GL freed elsewhere)
		void destroyGl(); // delete every GL texture and clear the cache (teardown)
		size_t textureCount() const { return texCache.size(); }
		// Bytes of decoded texture we uploaded (mip levels included at 4/3). Excludes textures the
		// renderer publishes (RT color, feedback copies): those are owned and counted by their FBO.
		size_t uploadedBytes() const { return texBytes; }
		// The render-target keys we hold, for the "RT bound but never published" diagnostic.
		std::vector<TextureId> renderTargetKeys() const;

		// What the GL texture cache is holding, split by what kind of key owns it. A growing cache is
		// only diagnosable if you can see which kind is growing: decoded content is swept by age,
		// render targets and feedback are freed with their FBO, streaming keys overwrite in place, so
		// each kind growing without bound means a different bug.
		struct CacheBreakdown
		{
			size_t content = 0, renderTarget = 0, feedback = 0, streaming = 0;
			size_t contentBytes = 0, streamingBytes = 0;
		};
		CacheBreakdown breakdown() const; // render thread

		// Written by whichever thread owns the event, read by the overlay: atomic rather than plain,
		// since a torn diagnostic is still a data race.
		struct Stats
		{
			std::atomic<uint64_t> decodes{0}; // content we had not decoded before, so a real decode ran
			std::atomic<uint64_t> evictions{0};
			// Cumulative nanoseconds, differenced per frame by the overlay. Split three ways because a
			// load spike has three candidates with different fixes: hashing runs on every bind, decoding
			// only on a content miss, uploading only on the render thread.
			std::atomic<uint64_t> hashNs{0}, decodeNs{0}, uploadNs{0};
			std::atomic<uint64_t> hashCalls{0}, hashBytes{0}, hashMemoHits{0};
			std::atomic<uint64_t> contentHits{0}; // decode+upload skipped: this content is already resident
			std::atomic<uint64_t> contentLive{0}, contentSwept{0};
			std::atomic<uint64_t> streamingAddrs{0}; // addresses whose pixels change too often to cache
		};
		Stats stats;

	private:
		void enqueueUpload(TextureId key, uint32_t srcPx, int w, int h, const unsigned char* rgba, unsigned filterAddressing);
		void dropEntry(TextureId key, bool deleteGl); // erase one entry and its byte contribution
		void sweepContent(); // drop content unused for a while, once past the byte budget
		std::vector<TextureId> pendingEvict; // content the sweep retired; the render thread frees the GL

		// ---- EE thread: everything known about one raster's pixels, in one place ----
		// `hashedSig` is the content hash we last computed, `uploadedSig` the one we last handed to the
		// render thread; they differ only between a decode failing and the next attempt.
		//
		// The entry's lifetime is the RW object's, not the frame's: pixels and CLUT change only through
		// RasterUnlock, RasterUnlockPalette and RasterDestroy, all of which we hook and all of which
		// drop it. Retiring per frame instead re-hashed every bound texture once a frame for nothing.
		// The bet that follows: a raster mutated without any of those three is never re-examined.
		struct Decoded
		{
			uint32_t palAddr = 0; // same pixels with a swapped CLUT is a different texture
			uint64_t hashedSig = 0;
			uint64_t uploadedSig = 0;
			// How many times the pixels at this address have changed under us. Past a threshold the
			// address is streaming (see STREAMING_TEX_BIT) and stops being keyed by content, since
			// every frame of an FMV would otherwise be a permanent entry nothing samples again.
			uint32_t contentChanges = 0;
		};
		std::unordered_map<uint32_t, Decoded> decoded; // keyed by pixel address

		// Decoded content, keyed by its hash rather than by the address the pixels sat at. This is the
		// point of the 64-bit key: the game re-materializes the same texture at new addresses
		// constantly (measured: 70-78% of decodes), and every one of those is a hit here, skipping the
		// decode and the upload both.
		//
		// Content therefore outlives the rasters that reference it, so a raster being destroyed no
		// longer frees anything: release is by age and byte budget instead.
		struct Content
		{
			uint64_t lastUsedFrame = 0;
			uint32_t bytes = 0;
			// Does this content actually contain translucent texels? RW's blend predicate has a texture
			// term, and the useful question is this one, not "does the format carry an alpha channel"
			// (every PS2 format does). Free to answer here: the decode already walks every pixel.
			bool translucent = false;
		};
		std::unordered_map<TextureId, Content> content;
		uint64_t frameIndex = 1;
		size_t contentBytes = 0;

		// ---- render thread: the GL texture per key, with the size it contributes ----
		struct GlTexture
		{
			GLuint id = 0;
			size_t bytes = 0;
			uint32_t srcPx = 0; // the address this content was decoded from, for the capture manifest
		};
		std::unordered_map<TextureId, GlTexture> texCache; // decoded textures and RT-tagged ids
		std::unordered_map<unsigned, GLuint> samplers; // render thread, keyed by packed filterAddressing
		size_t texBytes = 0;

		// ---- between the two, under `mutex` ----
		std::vector<PendingTex> pendingUpload;
		std::vector<std::pair<uint32_t, uint32_t>> pendingDestroy; // (raster, px)
		std::mutex mutex; // guards the two queues only
	};
} // namespace sky2
