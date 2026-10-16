#include "material/pipeline_dump.h"

#include "common.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace sky2
{
	namespace
	{
		constexpr uint32_t EE_LOW = 0x00080000; // below this is kernel/scratch, never pipeline data
		constexpr uint32_t EE_HIGH = 0x02000000;
		constexpr uint32_t NODE_STRIDE = 40; // RxPipelineNode
		constexpr uint32_t SCAN_QWORDS = 512; // per candidate block; bounds a bad pointer's cost
		constexpr uint8_t VIF_MPG = 0x4A;
		constexpr uint32_t MAX_REGS = 64;
		constexpr uint32_t GIF_FLG_PACKED = 0;
		constexpr uint32_t GIF_REGS_AD = 0xE;
		constexpr uint32_t VU1_INSTRS = 2048;

		bool plausible(uint32_t addr) { return addr >= EE_LOW && addr < EE_HIGH && (addr & 3) == 0; }

		// GIF A+D writes a register as [data:64][address:64], so a qword whose upper half is a small
		// register id is a register write and its lower half is the value. That is how the PS2 driver
		// programs the GS from inside a pipeline, which is why these never reach RpSkyRenderStateSet.
		const char* gsRegName(uint64_t addr)
		{
			switch (addr)
			{
				case 0x00: return "PRIM";
				case 0x01: return "RGBAQ";
				case 0x06: return "TEX0_1";
				case 0x08: return "CLAMP_1";
				case 0x14: return "TEX1_1";
				case 0x1A: return "PRMODECONT";
				case 0x3B: return "TEXA";
				case 0x40: return "SCISSOR_1";
				case 0x42: return "ALPHA_1";
				case 0x43: return "ALPHA_2";
				case 0x45: return "DTHE";
				case 0x46: return "COLCLAMP";
				case 0x47: return "TEST_1";
				case 0x48: return "TEST_2";
				case 0x49: return "PABE";
				case 0x4A: return "FBA_1";
				case 0x4C: return "FRAME_1";
				case 0x4E: return "ZBUF_1";
				default: return nullptr;
			}
		}

		constexpr uint64_t GS_TEST_1 = 0x47;
		constexpr uint64_t GS_TEST_2 = 0x48;
		constexpr uint64_t GS_ALPHA_1 = 0x42;
		constexpr uint64_t GS_ALPHA_2 = 0x43;

		void describeReg(std::string& out, uint64_t reg, const char* name, uint64_t v)
		{
			char line[192];
			if (reg == GS_TEST_1 || reg == GS_TEST_2)
			{
				GIFRegTEST t{};
				t.U64 = v;
				std::snprintf(line, sizeof(line),
					"    %-11s 0x%016llX  ATE=%u ATST=%u AREF=%u AFAIL=%u DATE=%u DATM=%u ZTE=%u ZTST=%u\n",
					name, static_cast<unsigned long long>(v), t.ATE, t.ATST, t.AREF, t.AFAIL, t.DATE,
					t.DATM, t.ZTE, t.ZTST);
			}
			else if (reg == GS_ALPHA_1 || reg == GS_ALPHA_2)
			{
				GIFRegALPHA a{};
				a.U64 = v;
				std::snprintf(line, sizeof(line), "    %-11s 0x%016llX  A=%u B=%u C=%u D=%u FIX=%u\n",
					name, static_cast<unsigned long long>(v), a.A, a.B, a.C, a.D, a.FIX);
			}
			else
			{
				std::snprintf(line, sizeof(line), "    %-11s 0x%016llX\n", name,
					static_cast<unsigned long long>(v));
			}
			out += line;
		}

		uint64_t fnv1a(uint64_t h, uint32_t word)
		{
			for (int b = 0; b < 4; b++)
			{
				h ^= (word >> (b * 8)) & 0xFFu;
				h *= 0x100000001B3ull;
			}
			return h;
		}

		// A block reachable from the pipeline: its GIF register writes and any VU microprogram it
		// uploads. Both are found by scanning rather than by a struct offset, because the node's
		// private layout differs per node definition and there are five of them.
		struct Scan
		{
			std::string regs;
			uint32_t regCount = 0;
			std::vector<uint32_t> vu; // microinstruction words, in upload order
			uint32_t vuAt = 0;
		};

		uint64_t read64(uint32_t at)
		{
			return (static_cast<uint64_t>(*guest::ptr<uint32_t>(at + 4)) << 32) |
			       *guest::ptr<uint32_t>(at);
		}

		void scanBlock(uint32_t base, Scan& s)
		{
			if (!plausible(base))
			{
				return;
			}
			for (uint32_t q = 0; q < SCAN_QWORDS; q++)
			{
				const uint32_t at = base + q * 16;
				if (!plausible(at + 12))
				{
					return;
				}
				const uint64_t lo = read64(at);
				const uint64_t hi = read64(at + 8);
				// A GIFtag introducing register writes: PACKED, one register per loop, and that
				// register is A+D, which takes its destination from each entry's upper half. Matching
				// the tag rather than the entries is what keeps ordinary memory from reading as a
				// packet, since an entry alone is just a qword with a small number in it.
				const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
				const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
				const uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
				if (flg == GIF_FLG_PACKED && nreg == 1 && (hi & 0xFu) == GIF_REGS_AD && nloop > 0 &&
					nloop <= MAX_REGS)
				{
					for (uint32_t k = 0; k < nloop && s.regCount < MAX_REGS; k++)
					{
						const uint32_t e = at + 16 + k * 16;
						if (!plausible(e + 12))
						{
							break;
						}
						const uint64_t reg = read64(e + 8);
						if (const char* name = gsRegName(reg))
						{
							describeReg(s.regs, reg, name, read64(e));
							s.regCount++;
						}
					}
				}
				// A VIF MPG code names the microinstructions that follow it: NUM double-words at
				// micro-memory ADDR, NUM of 0 meaning 256. VU1 holds 2048, so a larger address is
				// some other word that happens to start with 0x4A.
				for (const uint32_t w : {static_cast<uint32_t>(lo), static_cast<uint32_t>(lo >> 32),
						 static_cast<uint32_t>(hi), static_cast<uint32_t>(hi >> 32)})
				{
					if ((w >> 24) != VIF_MPG || (w & 0xFFFFu) >= VU1_INSTRS || !s.vu.empty())
					{
						continue;
					}
					const uint32_t num = ((w >> 16) & 0xFFu) ? ((w >> 16) & 0xFFu) : 256u;
					s.vuAt = at;
					for (uint32_t i = 0; i < num * 2 && plausible(at + 16 + i * 4); i++)
					{
						s.vu.push_back(*guest::ptr<uint32_t>(at + 16 + i * 4));
					}
				}
			}
		}

		std::unordered_map<uint32_t, PipeDump> g_cache;
	} // namespace

	bool pipeDumpEnabled()
	{
		static const bool on = sky2::env("PIPE_DUMP") != nullptr;
		return on;
	}

	const PipeDump& dumpPipeline(uint32_t pipelinePtr)
	{
		if (const auto have = g_cache.find(pipelinePtr); have != g_cache.end())
		{
			return have->second;
		}
		PipeDump d;
		d.pipeline = pipelinePtr;
		if (!pipelinePtr || !guest::ready())
		{
			return g_cache.emplace(pipelinePtr, std::move(d)).first->second;
		}
		d.pipeId = *guest::ptr<uint32_t>(pipelinePtr + 0x30);
		const uint32_t numNodes = *guest::ptr<uint32_t>(pipelinePtr + 0x04);
		const uint32_t nodes = *guest::ptr<uint32_t>(pipelinePtr + 0x08);
		Scan scan;
		std::string body;
		if (nodes && numNodes > 0 && numNodes <= 64)
		{
			for (uint32_t n = 0; n < numNodes; n++)
			{
				const uint32_t node = nodes + n * NODE_STRIDE;
				const uint32_t def = *guest::ptr<uint32_t>(node);
				const uint32_t name = def ? *guest::ptr<uint32_t>(def) : 0;
				std::string nodeName;
				for (uint32_t c = 0; name && c < 48; c++)
				{
					const char ch = static_cast<char>(*guest::ptr<uint8_t>(name + c));
					if (!ch)
					{
						break;
					}
					nodeName += ch;
				}
				d.nodes += d.nodes.empty() ? "" : " -> ";
				d.nodes += nodeName.empty() ? "?" : nodeName;
				char head[128];
				std::snprintf(head, sizeof(head), "  node %u @0x%08X def=0x%08X %s\n", n, node, def,
					nodeName.c_str());
				body += head;
				// Every word of the node is a candidate block base. A wrong guess scans bytes that
				// match nothing, which costs a bounded read and reports nothing.
				for (uint32_t w = 0; w < NODE_STRIDE / 4; w++)
				{
					scanBlock(*guest::ptr<uint32_t>(node + w * 4), scan);
				}
			}
		}
		scanBlock(pipelinePtr, scan);
		d.gsRegs = scan.regs;
		d.vuCode = scan.vuAt;
		d.vuInstrs = static_cast<uint32_t>(scan.vu.size() / 2);
		for (const uint32_t w : scan.vu)
		{
			d.vuHash = fnv1a(d.vuHash ? d.vuHash : 0xCBF29CE484222325ull, w);
		}
		char path[192];
		std::snprintf(path, sizeof(path), "experiments/dumps/pipe_%08X.txt", pipelinePtr);
		if (FILE* f = std::fopen(path, "w"))
		{
			std::fprintf(f, "pipeline 0x%08X pipeId 0x%X\n%s\n%s", pipelinePtr, d.pipeId,
				d.nodes.c_str(), body.c_str());
			if (!d.gsRegs.empty())
			{
				std::fprintf(f, "  GIF A+D register writes:\n%s", d.gsRegs.c_str());
			}
			std::fprintf(f, "  VU microprogram @0x%08X: %u instructions, hash 0x%016llX\n", d.vuCode,
				d.vuInstrs, static_cast<unsigned long long>(d.vuHash));
			for (size_t i = 0; i + 1 < scan.vu.size(); i += 2)
			{
				std::fprintf(f, "    %04zu  %08X %08X\n", i / 2, scan.vu[i + 1], scan.vu[i]);
			}
			std::fclose(f);
			d.dumpFile = path;
		}
		return g_cache.emplace(pipelinePtr, std::move(d)).first->second;
	}
} // namespace sky2
