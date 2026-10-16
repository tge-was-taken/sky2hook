#pragma once

#include "common.h" // Vertex3D

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace sky2
{
	// Growth-stable storage for one kind of geometry buffer: the build thread creates and frees, the
	// render thread reads what the frames it holds reference. Slots sit in fixed chunks that are never
	// moved, since a vector or deque subscript reads a base pointer that growth rewrites under a
	// concurrent reader. Metadata is atomic; contents are immutable between create and reclaim.
	template <class T>
	class BufferPool
	{
	public:
		// Build thread. Fill a scratch() buffer and hand it to create(): create move-assigns, so a
		// freshly constructed vector would drop the pooled allocation and malloc again every frame.
		std::vector<T> scratch();
		// `perFrame` geometry is reclaimed automatically once the fence passes the frame that built it;
		// anything else lives until free().
		GeomId create(std::vector<T>&& data, bool perFrame);
		void free(GeomId id);
		void touch(GeomId id); // stamp the current frame; call on every reference, including cache hits
		void beginFrame(uint64_t frameIndex, uint64_t retiredIndex);

		// The buffer behind a handle the caller knows is live, i.e. one reached from a frame it holds.
		// Does not test the generation: a slot referenced by such a frame cannot have been reclaimed,
		// and an explicit free() before the fence leaves the contents intact for exactly this reason.
		std::span<const T> view(GeomId id) const;

		// Whether the handle still names what it was created for. This is the staleness test for
		// caches that outlive a frame, notably the render thread's GPU residency map.
		bool alive(GeomId id) const;

		size_t liveCount() const { return live; }
		// Buffers filled from a recycled allocation vs freshly malloc'd. The second settling near zero
		// on a repeating scene is what says the pooling works.
		size_t reused() const { return scratchHits; }
		size_t allocated() const { return scratchMisses; }
		size_t bytes() const; // capacity held, including reclaimed slots kept for reuse

	private:
		static constexpr uint32_t CHUNK = 256;
		static constexpr uint32_t MAX_CHUNKS = GEOM_MAX_SLOTS / CHUNK;

		struct Slot
		{
			std::vector<T> data;
			std::atomic<uint64_t> generation{1}; // bumped on free, so old handles stop matching
			std::atomic<uint64_t> lastUsedFrame{0};
			std::atomic<bool> alive{false};
		};
		struct Chunk
		{
			Slot slots[CHUNK];
		};

		static constexpr uint32_t NO_SLOT = ~0u;
		Slot* slotAt(uint32_t index) const;
		uint32_t claimSlot(); // a free slot, a newly published one, or NO_SLOT when exhausted

		// Ownership is build-thread only; lookup goes through the atomic table, published after the
		// chunk is constructed so the render thread cannot observe a half-built one.
		std::vector<std::unique_ptr<Chunk>> owned;
		std::atomic<Chunk*> table[MAX_CHUNKS] = {};
		std::atomic<uint32_t> published{0};

		std::vector<uint32_t> freeSlots; // reclaimed, ready to reuse
		std::vector<uint32_t> pendingFree; // freed, waiting for the fence
		std::vector<uint32_t> perFrameSlots; // reclaimed automatically once the fence passes
		std::vector<std::vector<T>> spare; // buffers off reclaimed slots, waiting to be filled again
		uint64_t currentFrame = 0;
		size_t live = 0;
		size_t scratchHits = 0, scratchMisses = 0;
	};

	extern template class BufferPool<Vertex3D>;
	extern template class BufferPool<uint16_t>;

	// What geometry costs, split the way the texture accounting is: parsing guest vertex and index
	// arrays on the EE thread, then uploading the result on the render thread. The two have different
	// fixes, so a spike has to say which one it is. Atomic because the overlay reads them from the
	// thread that does not write them.
	struct GeomStats
	{
		// One parser's work. `misses` ran it; `hits` were served by a cache and cost nothing.
		struct Parse
		{
			std::atomic<uint64_t> ns{0}, misses{0}, hits{0}, verts{0}, indices{0};
			// Of the misses, how many re-parsed to the same bytes, so the cached buffers were kept.
			std::atomic<uint64_t> unchanged{0};
		};
		Parse atomics; // RpAtomic geometry, cached by geometry address
		Parse sectors; // RpWorldSector geometry, cached by sector address

		// Im3D builds its geometry fresh every frame, so it has no cache to hit. identityModel is why
		// it cannot get one: RwIm3DVertex calls its position object-space, but every batch measured
		// carries an identity transform, so the game writes world space and the bytes change whenever
		// the object moves.
		std::atomic<uint64_t> im3dBatches{0}, im3dBytes{0}, im3dIdentityModel{0};

		std::atomic<uint64_t> uploadNs{0}, vertUploads{0}, idxUploads{0}, uploadBytes{0};
		// Creates against uploads separates new geometry from re-uploads into a buffer we already had,
		// which is the difference between an area load and the transient path churning.
		std::atomic<uint64_t> bufferCreates{0};
		std::atomic<uint64_t> evictions{0}; // resident buffers freed after going unbound
	};

	// The frame's geometry, split by buffer kind. One instance, owned by the Device.
	struct GeometryPool
	{
		BufferPool<Vertex3D> verts;
		BufferPool<uint16_t> indices;
		GeomStats stats;

		void beginFrame(uint64_t frameIndex, uint64_t retiredIndex)
		{
			verts.beginFrame(frameIndex, retiredIndex);
			indices.beginFrame(frameIndex, retiredIndex);
		}

		// Record a parse that actually ran, from whatever it produced. Both cached types name their
		// vertex count `vcount` and their per-material index lists `subs`.
		template <class Parsed>
		void noteParse(GeomStats::Parse& p, const Parsed& parsed)
		{
			bumpStat(p.misses);
			bumpStat(p.verts, parsed.vcount);
			for (const auto& sub : parsed.subs)
			{
				bumpStat(p.indices, indices.view(sub.indices).size());
			}
		}
	};
} // namespace sky2
