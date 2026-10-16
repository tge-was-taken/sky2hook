#pragma once

#include "common.h"

#include <array>
#include <functional>
#include <vector>

namespace sky2
{
	// Where one UNPACK's elements landed in VU1 data memory, and what format they arrived in. The
	// cycle registers, not the element index, decide the destination, so it is recorded per element.
	struct VifStream
	{
		uint8_t vn = 0, vl = 0;
		bool usn = false;
		std::vector<uint16_t> dst;
	};

	// The UNPACKs between two microprogram kicks: VU1 runs once over these.
	struct VifBatch
	{
		std::vector<VifStream> streams;
		uint32_t itop = 0;
	};

	// VIF1's unpack path replayed over a DMA source chain in guest memory, writing into a modelled
	// VU1 data memory. Models the state that decides where a write lands and what it holds (cycle,
	// write mask, row/col, addition mode, double-buffer base), so an interleaved or masked packet
	// decodes as the hardware wrote it rather than as a packed array.
	class VifMachine
	{
	public:
		// onBatch fires at each kick, while VU1 memory still holds what that microprogram would
		// have seen: a later batch writing the same addresses is exactly what double buffering
		// exists to avoid, and reading afterwards would miss when it does not.
		void run(uint32_t chainAddr, uint32_t endAddr, const std::function<void(const VifBatch&)>& onBatch);

		uint32_t word(uint32_t qw, int field) const { return mem_[(qw & 0x3FF) * 4 + field]; }
		float f32(uint32_t qw, int field) const;
		int32_t i32(uint32_t qw, int field) const { return static_cast<int32_t>(word(qw, field)); }

	private:
		void runCodes(uint32_t w, uint32_t end, uint32_t dataOverride, bool& usedOverride);
		void unpack(uint32_t code, uint32_t dataAddr);
		void kick();

		std::array<uint32_t, 1024 * 4> mem_{};
		const std::function<void(const VifBatch&)>* onBatch_ = nullptr;
		VifBatch pending_;

		uint32_t cl_ = 1, wl_ = 1;
		uint32_t mask_ = 0, mode_ = 0;
		std::array<uint32_t, 4> row_{}, col_{};
		uint32_t base_ = 0, ofst_ = 0, tops_ = 0, itops_ = 0;
		bool dbf_ = false;
	};
} // namespace sky2
