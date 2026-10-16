#include "vif.h"

#include <algorithm>
#include <cstring>

namespace sky2
{
	namespace
	{
		enum VifCmd
		{
			VIF_NOP = 0x00,
			VIF_STCYCL = 0x01,
			VIF_OFFSET = 0x02,
			VIF_BASE = 0x03,
			VIF_ITOP = 0x04,
			VIF_STMOD = 0x05,
			VIF_MSKPATH3 = 0x06,
			VIF_MARK = 0x07,
			VIF_FLUSHE = 0x10,
			VIF_FLUSH = 0x11,
			VIF_FLUSHA = 0x13,
			VIF_MSCAL = 0x14,
			VIF_MSCALF = 0x15,
			VIF_MSCNT = 0x17,
			VIF_STMASK = 0x20,
			VIF_STROW = 0x30,
			VIF_STCOL = 0x31,
			VIF_MPG = 0x4A,
			VIF_DIRECT = 0x50,
			VIF_DIRECTHL = 0x51
		};

		// Source bytes per element, [(32 >> vl) * (vn + 1)] / 8, with V4_5 packing a whole vector
		// into 16 bits.
		uint32_t elementBytes(uint8_t vn, uint8_t vl)
		{
			if (vn == 3 && vl == 3)
			{
				return 2;
			}
			return ((32u >> vl) * (vn + 1u)) / 8u;
		}

		uint32_t component(uint32_t addr, int k, uint8_t vl, bool usn)
		{
			switch (vl)
			{
				case 0: return *guest::ptr<uint32_t>(addr + k * 4);
				case 1:
				{
					const uint16_t v = *guest::ptr<uint16_t>(addr + k * 2);
					return usn ? v : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v)));
				}
				default:
				{
					const uint8_t v = *guest::ptr<uint8_t>(addr + k);
					return usn ? v : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v)));
				}
			}
		}

		// One source element widened to a quadword the way VIF does it: S broadcasts, V2 repeats the
		// pair, and V3 reads a fourth component from the element after it (hardware does, and the
		// following unpack normally overwrites it).
		void readElement(uint32_t addr, uint8_t vn, uint8_t vl, bool usn, uint32_t out[4])
		{
			if (vn == 3 && vl == 3)
			{
				const uint32_t d = *guest::ptr<uint16_t>(addr);
				out[0] = (d & 0x001F) << 3;
				out[1] = (d & 0x03E0) >> 2;
				out[2] = (d & 0x7C00) >> 7;
				out[3] = (d & 0x8000) >> 8;
				return;
			}
			switch (vn)
			{
				case 0:
					out[0] = out[1] = out[2] = out[3] = component(addr, 0, vl, usn);
					break;
				case 1:
					out[0] = out[2] = component(addr, 0, vl, usn);
					out[1] = out[3] = component(addr, 1, vl, usn);
					break;
				default:
					for (int k = 0; k < 4; k++)
					{
						out[k] = component(addr, k, vl, usn);
					}
					break;
			}
		}
	} // namespace

	float VifMachine::f32(uint32_t qw, int field) const
	{
		const uint32_t bits = word(qw, field);
		float f;
		std::memcpy(&f, &bits, sizeof(f));
		return f;
	}

	void VifMachine::kick()
	{
		pending_.itop = itops_;
		(*onBatch_)(pending_);
		pending_ = VifBatch{};
		tops_ = dbf_ ? base_ : base_ + ofst_;
		dbf_ = !dbf_;
	}

	void VifMachine::unpack(uint32_t code, uint32_t dataAddr)
	{
		const uint32_t cmd = (code >> 24) & 0x7F;
		const uint32_t num = ((code >> 16) & 0xFF) ? ((code >> 16) & 0xFF) : 256;
		VifStream s;
		s.vn = static_cast<uint8_t>((cmd >> 2) & 3);
		s.vl = static_cast<uint8_t>(cmd & 3);
		s.usn = ((code >> 14) & 1) != 0;
		const bool doMask = (cmd & 0x10) != 0;
		const bool flg = ((code >> 15) & 1) != 0;
		const uint32_t esz = elementBytes(s.vn, s.vl);

		uint32_t addr = ((code & 0x3FF) + (flg ? tops_ : 0)) & 0x3FF;
		// wl > cl is a filling write, which repeats a source element across the extra destination
		// quadwords; otherwise writes advance in groups of wl and skip cl - wl between them.
		const bool fill = wl_ > cl_;
		const uint32_t skip = fill ? 0 : cl_ - wl_;
		uint32_t cycle = 0, src = dataAddr;

		s.dst.reserve(num);
		for (uint32_t left = num; left; left--)
		{
			uint32_t v[4];
			readElement(src, s.vn, s.vl, s.usn, v);
			uint32_t* dest = &mem_[(addr & 0x3FF) * 4];
			const uint32_t maskCycle = std::min(cycle, 3u);
			for (int f = 0; f < 4; f++)
			{
				const int sel = doMask ? static_cast<int>((mask_ >> (maskCycle * 8 + f * 2)) & 3) : 0;
				// V4_5 ignores the addition mode.
				const uint32_t mode = (s.vn == 3 && s.vl == 3) ? 0u : mode_;
				switch (sel)
				{
					case 0:
						switch (mode)
						{
							case 1: dest[f] = v[f] + row_[f]; break;
							case 2: dest[f] = row_[f] = row_[f] + v[f]; break;
							case 3: dest[f] = row_[f] = v[f]; break;
							default: dest[f] = v[f]; break;
						}
						break;
					case 1: dest[f] = row_[f]; break;
					case 2: dest[f] = col_[maskCycle]; break;
					default: break; // write protect
				}
			}
			s.dst.push_back(static_cast<uint16_t>(addr & 0x3FF));
			addr++;
			cycle++;
			if (fill)
			{
				if (cycle <= cl_)
				{
					src += esz;
				}
				else if (cycle == wl_)
				{
					cycle = 0;
				}
			}
			else
			{
				src += esz;
				if (cycle >= wl_)
				{
					addr += skip;
					cycle = 0;
				}
			}
		}
		pending_.streams.push_back(std::move(s));
	}

	// One span of VIFcodes. dataOverride names a REF tag's payload, which its own TTE-packed UNPACK
	// reads instead of the bytes that follow the code (a tag's TTE span has no room for it).
	void VifMachine::runCodes(uint32_t w, uint32_t end, uint32_t dataOverride, bool& usedOverride)
	{
		while (w + 4 <= end)
		{
			const uint32_t code = *guest::ptr<uint32_t>(w);
			w += 4;
			const uint32_t cmd = (code >> 24) & 0x7F, num = (code >> 16) & 0xFF, imm = code & 0xFFFF;
			if ((cmd & 0x60) == 0x60)
			{
				if (dataOverride)
				{
					unpack(code, dataOverride);
					usedOverride = true;
					continue;
				}
				unpack(code, w);
				const uint32_t n = num ? num : 256;
				const uint32_t esz = elementBytes((cmd >> 2) & 3, cmd & 3);
				const uint32_t elems = wl_ > cl_ ? cl_ * (n / wl_) + std::min(n % wl_, cl_) : n;
				w += ((elems * esz + 3) / 4) * 4;
				continue;
			}
			switch (cmd)
			{
				case VIF_STCYCL:
					cl_ = imm & 0xFF;
					wl_ = (imm >> 8) & 0xFF;
					cl_ = cl_ ? cl_ : 256;
					wl_ = wl_ ? wl_ : 256;
					break;
				case VIF_OFFSET: ofst_ = imm & 0x3FF; break;
				case VIF_BASE: base_ = imm & 0x3FF; break;
				case VIF_ITOP: itops_ = imm & 0x3FF; break;
				case VIF_STMOD: mode_ = imm & 3; break;
				case VIF_STMASK:
					mask_ = *guest::ptr<uint32_t>(w);
					w += 4;
					break;
				case VIF_STROW:
					for (int i = 0; i < 4; i++)
					{
						row_[i] = *guest::ptr<uint32_t>(w + i * 4);
					}
					w += 16;
					break;
				case VIF_STCOL:
					for (int i = 0; i < 4; i++)
					{
						col_[i] = *guest::ptr<uint32_t>(w + i * 4);
					}
					w += 16;
					break;
				case VIF_MPG: w += (num ? num : 256) * 8; break;
				case VIF_DIRECT:
				case VIF_DIRECTHL: w += (imm ? imm : 65536) * 16; break;
				case VIF_MSCAL:
				case VIF_MSCALF:
				case VIF_MSCNT: kick(); break;
				default: break; // NOP and the remaining state-setting codes carry no inline data
			}
		}
	}

	// Walk the DMA source chain. The tag's ID says where its data is: CNT and friends keep it inline
	// after the tag, but a REF points at it and the chain continues at the tag's own end.
	void VifMachine::run(uint32_t addr, uint32_t endAddr, const std::function<void(const VifBatch&)>& onBatch)
	{
		onBatch_ = &onBatch;
		for (int tag = 0; addr + 16 <= endAddr && tag < 4096; tag++)
		{
			const uint32_t low = *guest::ptr<uint32_t>(addr);
			const uint32_t qwc = low & 0xFFFF;
			const uint32_t id = (low >> 28) & 0x7;
			const uint32_t ref = *guest::ptr<uint32_t>(addr + 4) & 0x7FFFFFFF;
			const bool byRef = id == 0 || id == 3 || id == 4; // refe / ref / refs
			const uint32_t dataBegin = byRef ? ref : addr + 16;
			const uint32_t dataEnd = dataBegin + qwc * 16;
			if (dataEnd < dataBegin || (!byRef && dataEnd > endAddr))
			{
				return;
			}
			bool tteTookData = false;
			runCodes(addr + 8, addr + 16, (byRef && qwc) ? dataBegin : 0, tteTookData);
			if (qwc && !tteTookData)
			{
				bool unused = false;
				runCodes(dataBegin, dataEnd, 0, unused);
			}
			// refe/ret/end stop the chain; ref continues past its own tag, cnt past its data, and
			// next/call take the pointer.
			if (id == 0 || id == 6 || id == 7)
			{
				return;
			}
			addr = byRef ? addr + 16 : (id == 2 || id == 5 ? ref : dataEnd);
			if (!addr)
			{
				return;
			}
		}
	}
} // namespace sky2
