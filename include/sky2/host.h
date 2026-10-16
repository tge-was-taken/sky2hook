// Services the library cannot provide itself: the guest executable's symbol table, and the
// emulator-side diagnostics a draw can tag. Every method has a do-nothing default, so a host
// implements only what it has and the library runs with no host at all (offline replay).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sky2
{
	class Host
	{
	public:
		virtual ~Host() = default;

		virtual std::string resolveSymbol(uint32_t /*pc*/) { return {}; }
		virtual bool inCodeRange(uint32_t /*pc*/) { return false; }
		virtual void loadSymbols() {}

		// The emulator's own GS trace rows for the last finished frame, appended to `path`. Returns
		// how many rows were written.
		virtual size_t writeGsRows(const char* /*path*/) { return 0; }

		// Name the guest work a hooked render callback is about to do, so an emulator-side trace can
		// attribute what the guest then submits (a VU1 upload, a GS batch) to this draw.
		virtual void pushDraw(const char* /*kind*/, uint32_t /*a*/, uint32_t /*b*/) {}
		virtual void popDraw() {}
	};

	void setHost(Host& host);
	Host& host();
} // namespace sky2
