// Overlay guest structs on the emulated console's main RAM; ptr<T>/val<T> read and write it
// transparently. The RenderWare ABI structs on top live in renderware.h. Runtime-agnostic: this models
// the PS2 guest (the Emotion Engine address space), not any host runtime. The host points g_ram at
// the active runtime's RAM buffer via setRam().
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sky2::guest
{
	using addr_t = uint32_t; // a guest EE address

	extern uint8_t* g_ram; // 32 MB guest main RAM, set once by the core via setRam()
	constexpr addr_t RAM_MASK = 0x01FFFFFF;
	constexpr size_t RAM_SIZE = size_t(RAM_MASK) + 1;

	// Defined by the core; forwards to the renderer's error path. guest.h is the lowest-level overlay
	// (used before the sky2 subsystem exists), so it routes errors through this choke point instead
	// of calling sky2::reportError directly, which would be a layering cycle.
	void reportOverflow(const char* fmt, ...)
#ifdef __GNUC__
		__attribute__((format(printf, 1, 2)))
#endif
		;

	// Zeroed scratch returned on an out-of-range or pre-init access, so callers read zeros / write
	// somewhere harmless instead of faulting or touching unrelated host memory. Grows to fit.
	inline uint8_t* sentinel(size_t size)
	{
		static thread_local std::vector<uint8_t> buf;
		if (buf.size() < size)
		{
			buf.assign(size, 0);
		}
		return buf.data();
	}

	// Point the overlay at the guest RAM buffer. Requires exactly RAM_SIZE so RAM_MASK can never
	// overrun it. On mismatch, reports and leaves the overlay unset (every access then reports).
	inline void setRam(uint8_t* p, size_t size)
	{
		if (size != RAM_SIZE)
		{
			reportOverflow("guest::setRam: buffer size %zu != expected guest RAM size %zu", size, RAM_SIZE);
			g_ram = nullptr;
			return;
		}
		g_ram = p;
	}

	// The choke point for every guest-memory access: masks `a` into the 32 MB window and checks the
	// full [a, a+size) extent fits, so a near-top multi-byte access can't overrun. Reports and returns
	// a sentinel instead of faulting.
	inline void* host(addr_t a, size_t size)
	{
		if (!g_ram)
		{
			reportOverflow("guest:: access before g_ram is set (addr=0x%08X size=%zu)", a, size);
			return sentinel(size);
		}
		const uint32_t base = a & RAM_MASK;
		if (uint64_t(base) + uint64_t(size) > RAM_SIZE)
		{
			reportOverflow("guest:: access out of range (addr=0x%08X size=%zu)", a, size);
			return sentinel(size);
		}
		return g_ram + base;
	}

	// A guest scalar, transparently usable as its underlying type. Only ever a field of a struct
	// already overlaid via ptr<T>, whose bounds check covers the whole struct.
	template <class T>
	struct val
	{
		T v;
		operator T() const { return v; } // read
		T operator=(T x)
		{
			v = x;
			return x;
		} // write into guest RAM
	};
	using u8 = val<uint8_t>;
	using u16 = val<uint16_t>;
	using u32 = val<uint32_t>;
	using s8 = val<int8_t>;
	using s16 = val<int16_t>;
	using s32 = val<int32_t>;
	using f32 = val<float>;
	static_assert(sizeof(u32) == 4 && sizeof(f32) == 4, "val must be transparent");

	// A guest pointer to T (4 bytes). Dereferencing overlays T in guest RAM, so p->field and p[i]
	// read/write the actual guest memory. Construct guest::ptr<T>(addr), deref with * / -> / [].
	template <class T>
	struct ptr
	{
		uint32_t addr;
		constexpr explicit ptr(addr_t a = 0)
			: addr(a)
		{
		}
		T* get() const
		{
			if (addr == 0)
			{
				reportOverflow("guest::ptr<T>::get() dereferenced a NULL guest pointer");
				return reinterpret_cast<T*>(sentinel(sizeof(T)));
			}
			return reinterpret_cast<T*>(host(addr, sizeof(T)));
		}
		T* operator->() const { return get(); }
		T& operator*() const { return *get(); }
		T& operator[](int32_t i) const
		{
			// Compute the guest address first (64-bit to dodge signed-overflow UB), then bound-check
			// it, rather than indexing the resolved host pointer (which would walk host memory).
			const addr_t ga = static_cast<addr_t>(static_cast<int64_t>(addr) + static_cast<int64_t>(i) * static_cast<int64_t>(sizeof(T)));
			return *reinterpret_cast<T*>(host(ga, sizeof(T)));
		}
		explicit operator bool() const { return addr != 0; }
	};
	template <>
	struct ptr<void> // opaque guest pointer: address only, no deref
	{
		uint32_t addr;
		constexpr explicit ptr(addr_t a = 0)
			: addr(a)
		{
		}
		explicit operator bool() const { return addr != 0; }
	};
	static_assert(sizeof(ptr<int>) == 4 && sizeof(ptr<void>) == 4, "ptr must be 4 bytes");

	inline bool ready() { return g_ram != nullptr; }
} // namespace sky2::guest
