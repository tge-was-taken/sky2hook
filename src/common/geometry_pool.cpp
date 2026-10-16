#include "geometry_pool.h"

#include "sky2/config.h"

#include <algorithm>
#include <limits>

namespace sky2
{
	namespace
	{
		void poison(std::vector<Vertex3D>& v)
		{
			Vertex3D bad{};
			bad.pos = glm::vec3(std::numeric_limits<float>::quiet_NaN());
			std::fill(v.begin(), v.end(), bad);
		}
		void poison(std::vector<uint16_t>& v) { std::fill(v.begin(), v.end(), uint16_t{0xFFFF}); }
	} // namespace

	template <class T>
	typename BufferPool<T>::Slot* BufferPool<T>::slotAt(uint32_t index) const
	{
		const uint32_t chunk = index / CHUNK;
		if (chunk >= MAX_CHUNKS)
		{
			return nullptr;
		}
		Chunk* c = table[chunk].load(std::memory_order_acquire);
		return c ? &c->slots[index % CHUNK] : nullptr;
	}

	template <class T>
	uint32_t BufferPool<T>::claimSlot()
	{
		if (!freeSlots.empty())
		{
			const uint32_t slot = freeSlots.back();
			freeSlots.pop_back();
			return slot;
		}
		const uint32_t slot = published.load(std::memory_order_relaxed);
		if (slot >= GEOM_MAX_SLOTS)
		{
			SKY2_ERROR(false, "geometry pool exhausted at %u slots", slot);
			return NO_SLOT; // never slot 0: that would hand out a live buffer belonging to something else
		}
		if (slot % CHUNK == 0) // first slot of a new chunk: build it, then publish the pointer
		{
			owned.push_back(std::make_unique<Chunk>());
			table[slot / CHUNK].store(owned.back().get(), std::memory_order_release);
		}
		published.store(slot + 1, std::memory_order_release);
		return slot;
	}

	template <class T>
	std::vector<T> BufferPool<T>::scratch()
	{
		if (spare.empty())
		{
			scratchMisses++;
			return {};
		}
		scratchHits++;
		std::vector<T> v = std::move(spare.back());
		spare.pop_back();
		v.clear(); // capacity survives; callers that need N initialized elements assign(), not resize()
		return v;
	}

	template <class T>
	GeomId BufferPool<T>::create(std::vector<T>&& data, bool perFrame)
	{
		const uint32_t index = claimSlot();
		Slot* s = index == NO_SLOT ? nullptr : slotAt(index);
		if (!s)
		{
			return GEOM_ID_NONE;
		}
		s->data = std::move(data);
		s->lastUsedFrame.store(currentFrame, std::memory_order_relaxed);
		s->alive.store(true, std::memory_order_release);
		live++;
		if (perFrame)
		{
			perFrameSlots.push_back(index);
		}
		return geomId(index, s->generation.load(std::memory_order_relaxed));
	}

	template <class T>
	void BufferPool<T>::free(GeomId id)
	{
		if (!alive(id))
		{
			return;
		}
		const uint32_t index = geomSlot(id);
		Slot* s = slotAt(index);
		s->alive.store(false, std::memory_order_release);
		// Bump now so every stale handle stops matching immediately, but hold the slot: a frame the
		// render thread still holds may reference it, and its contents must stay readable until the
		// fence says otherwise.
		s->generation.fetch_add(1, std::memory_order_release);
		pendingFree.push_back(index);
		live--;
	}

	template <class T>
	void BufferPool<T>::touch(GeomId id)
	{
		// GEOM_ID_NONE has slot 0, so without the null test a failed create() would keep refreshing a
		// live slot belonging to some other geometry, which reclaim() then never recycles. view() and
		// alive() both guard the same way.
		if (Slot* s = id ? slotAt(geomSlot(id)) : nullptr)
		{
			s->lastUsedFrame.store(currentFrame, std::memory_order_release);
		}
	}

	template <class T>
	void BufferPool<T>::beginFrame(uint64_t frameIndex, uint64_t retiredIndex)
	{
		currentFrame = frameIndex;

		// Per-frame geometry belongs to the frame that built it, so the fence passing that frame is
		// the whole condition. A slot freed explicitly in the meantime is already on pendingFree.
		const auto retire = [&](uint32_t index) {
			Slot* s = slotAt(index);
			if (!s->alive.load(std::memory_order_acquire))
			{
				return true; // freed by hand; pendingFree owns it now
			}
			if (s->lastUsedFrame.load(std::memory_order_acquire) > retiredIndex)
			{
				return false;
			}
			s->alive.store(false, std::memory_order_release);
			s->generation.fetch_add(1, std::memory_order_release);
			pendingFree.push_back(index);
			live--;
			return true;
		};
		perFrameSlots.erase(std::remove_if(perFrameSlots.begin(), perFrameSlots.end(), retire),
			perFrameSlots.end());

		const auto reclaim = [&](uint32_t index) {
			Slot* s = slotAt(index);
			if (s->lastUsedFrame.load(std::memory_order_acquire) > retiredIndex)
			{
				return false; // a frame the render thread can still draw references it
			}
			// Poisoning keeps the slot populated with obviously wrong values instead of clearing it, so
			// a read through a stale handle draws visible garbage rather than the previous tenant's
			// perfectly plausible geometry. Replay and savestate diffs cannot catch that on their own.
			if (sky2::config().poolPoison)
			{
				poison(s->data); // leave it populated and wrong; do not recycle the buffer either
			}
			else
			{
				s->data.clear();
				spare.push_back(std::move(s->data)); // hand the allocation back to scratch()
			}
			freeSlots.push_back(index);
			return true;
		};
		pendingFree.erase(std::remove_if(pendingFree.begin(), pendingFree.end(), reclaim),
			pendingFree.end());
	}

	template <class T>
	std::span<const T> BufferPool<T>::view(GeomId id) const
	{
		const Slot* s = id ? slotAt(geomSlot(id)) : nullptr;
		return s ? std::span<const T>(s->data) : std::span<const T>();
	}

	template <class T>
	bool BufferPool<T>::alive(GeomId id) const
	{
		const Slot* s = id ? slotAt(geomSlot(id)) : nullptr;
		return s && s->alive.load(std::memory_order_acquire) &&
			   s->generation.load(std::memory_order_acquire) == geomGen(id);
	}

	template <class T>
	size_t BufferPool<T>::bytes() const
	{
		size_t total = 0;
		for (uint32_t i = 0, n = published.load(std::memory_order_acquire); i < n; i++)
		{
			total += slotAt(i)->data.capacity() * sizeof(T);
		}
		for (const std::vector<T>& v : spare)
		{
			total += v.capacity() * sizeof(T);
		}
		return total;
	}

	template class BufferPool<Vertex3D>;
	template class BufferPool<uint16_t>;
} // namespace sky2
