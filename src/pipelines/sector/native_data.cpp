#include "native_data.h"

#include "vif.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace sky2
{
	namespace
	{
		enum VifFormat
		{
			FMT_V2_32 = (1 << 2) | 0,
			FMT_V3_32 = (2 << 2) | 0,
			FMT_V4_32 = (3 << 2) | 0,
			FMT_V3_8 = (2 << 2) | 2,
			FMT_V4_8 = (3 << 2) | 2
		};

		const char* unpackName(uint8_t vn, uint8_t vl)
		{
			static const char* names[4][4] = {
				{"S_32", "S_16", "S_8", "S_?"},
				{"V2_32", "V2_16", "V2_8", "V2_?"},
				{"V3_32", "V3_16", "V3_8", "V3_?"},
				{"V4_32", "V4_16", "V4_8", "V4_5"}};
			return names[vn & 3][vl & 3];
		}

		// Sky2's packed skin weight word: low 10 bits are (boneIndex+1)*4, the rest is the blend weight
		// with its low mantissa bits overwritten; 0 means the influence slot is unused
		void decodeSkinWord(uint32_t bits, float& index, float& weight)
		{
			if (!bits)
			{
				index = 0.0f;
				weight = 0.0f;
				return;
			}
			index = static_cast<float>((bits & 0x3FF) / 4 - 1);
			const uint32_t weightBits = bits & 0xFFFFFC00;
			std::memcpy(&weight, &weightBits, sizeof(weight));
		}

		// One batch's attributes, read out of VU1 memory where the batch's UNPACKs put them. V4_32 is
		// ambiguous: Atomics carry position as V4_32, but skinned meshes carry a same-count V4_32 skin
		// weight/index stream alongside a real V3_32 position instead, so both readings are staged and
		// the caller picks once it knows whether a V3_32 showed up.
		struct BatchAttribs
		{
			std::vector<glm::vec3> pos, normal;
			std::vector<glm::vec2> uv;
			std::vector<glm::vec4> color;
			std::vector<glm::vec3> v4pos;
			std::vector<glm::vec4> v4boneIndex, v4boneWeight;
		};

		void readBatch(const VifMachine& vif, const VifBatch& batch, BatchAttribs& out)
		{
			for (const VifStream& s : batch.streams)
			{
				// ITOP is the vertex count VU1 loops over, so a stream of another length is not a
				// per-vertex attribute: the same chain also carries matrix and light uploads, which
				// have a format this would otherwise read as positions.
				if (batch.itop && s.dst.size() != batch.itop)
				{
					continue;
				}
				const int format = (s.vn << 2) | s.vl;
				for (uint16_t qw : s.dst)
				{
					switch (format)
					{
						case FMT_V3_32:
							out.pos.emplace_back(vif.f32(qw, 0), vif.f32(qw, 1), vif.f32(qw, 2));
							break;
						case FMT_V4_32:
						{
							out.v4pos.emplace_back(vif.f32(qw, 0), vif.f32(qw, 1), vif.f32(qw, 2));
							glm::vec4 idx{}, wt{};
							for (int j = 0; j < 4; j++)
							{
								decodeSkinWord(vif.word(qw, j), idx[j], wt[j]);
							}
							out.v4boneIndex.push_back(idx);
							out.v4boneWeight.push_back(wt);
							break;
						}
						case FMT_V2_32:
							out.uv.emplace_back(vif.f32(qw, 0), vif.f32(qw, 1));
							break;
						case FMT_V4_8:
							out.color.emplace_back(vif.i32(qw, 0) / 255.0f, vif.i32(qw, 1) / 255.0f,
								vif.i32(qw, 2) / 255.0f, vif.i32(qw, 3) / 255.0f);
							break;
						case FMT_V3_8:
							out.normal.emplace_back(
								vif.i32(qw, 0) / 127.0f, vif.i32(qw, 1) / 127.0f, vif.i32(qw, 2) / 127.0f);
							break;
						default: break; // not an attribute this pipeline reads
					}
				}
			}
		}

		// The chain for one mesh: the RwResEntry's payload starts with a small header whose last word
		// points at the first DMA tag. Returns 0 if the entry doesn't look like one.
		uint32_t meshChain(uint32_t nativeTableAddr, int mesh, uint32_t& chainEnd)
		{
			const uint32_t count = *guest::ptr<uint32_t>(nativeTableAddr);
			if (mesh < 0 || static_cast<uint32_t>(mesh) >= count || count > 4096)
			{
				SKY2_TRACE_ONCE(nativeTableAddr, "native table 0x%X: mesh %d outside count %u",
					nativeTableAddr, mesh, count);
				return 0;
			}
			const uint32_t res = *guest::ptr<uint32_t>(nativeTableAddr + 4 + mesh * 4);
			if (!res)
			{
				SKY2_TRACE_ONCE(nativeTableAddr, "native table 0x%X: mesh %d has no ResEntry",
					nativeTableAddr, mesh);
				return 0;
			}
			const uint32_t size = *guest::ptr<uint32_t>(res + 0x08); // RwResEntry::size
			const uint32_t chain = *guest::ptr<uint32_t>(res + 0x20);
			if (!chain || chain < res || chain >= res + size)
			{
				SKY2_TRACE_ONCE(res, "ResEntry 0x%X: chain 0x%X outside [0x%X,0x%X) (size %u)",
					res, chain, res, res + size, size);
				return 0;
			}
			chainEnd = res + size;
			return chain;
		}

		// Append src to dst, dropping src's leading overlap with dst's tail.
		template <class T>
		void appendAfterOverlap(std::vector<T>& dst, const std::vector<T>& src, size_t drop)
		{
			dst.insert(dst.end(), src.begin() + std::min(drop, src.size()), src.end());
		}
	} // namespace

	uint32_t sectorNativeTable(uint32_t sectorAddr)
	{
		const uint32_t off = sky2::addresses().worldSectorNativeDataOffset;
		return off ? *guest::ptr<uint32_t>(sectorAddr + off) : 0;
	}

	uint32_t geometryNativeTable(uint32_t geomAddr)
	{
		const uint32_t off = sky2::addresses().geometryNativeDataOffset;
		return off ? *guest::ptr<uint32_t>(geomAddr + off) : 0;
	}

	bool decodeNativeMesh(uint32_t nativeTableAddr, int mesh, NativeMesh& out)
	{
		uint32_t end = 0;
		const uint32_t chain = meshChain(nativeTableAddr, mesh, end);
		if (!chain)
		{
			return false;
		}
		VifMachine vif;
		vif.run(chain, end, [&](const VifBatch& batch) {
			BatchAttribs cur;
			readBatch(vif, batch, cur);
			// A real V3_32 position means the V4_32 stream was skin weight/index data instead.
			const bool v4IsSkin = !cur.pos.empty();
			if (!v4IsSkin)
			{
				cur.pos = std::move(cur.v4pos);
			}
			if (cur.pos.empty())
			{
				return;
			}
			// VU1 reads only the first ITOP vertices; the packet's UNPACKs can declare more, because
			// they are sized for a batch's worst case rather than for this instance.
			const size_t live = batch.itop ? std::min<size_t>(batch.itop, cur.pos.size()) : cur.pos.size();
			// Every batch after the first restates the two vertices the strip continues from.
			const size_t n = out.pos.size();
			const bool overlaps = n >= 2 && live >= 2 && cur.pos[0] == out.pos[n - 2] && cur.pos[1] == out.pos[n - 1];
			const size_t drop = overlaps ? 2 : 0;
			const auto take = [&](auto& dst, auto& src) {
				src.resize(std::min(live, src.size()));
				appendAfterOverlap(dst, src, drop);
			};
			take(out.pos, cur.pos);
			take(out.uv, cur.uv);
			take(out.color, cur.color);
			take(out.normal, cur.normal);
			if (v4IsSkin)
			{
				take(out.boneIndex, cur.v4boneIndex);
				take(out.boneWeight, cur.v4boneWeight);
			}
		});
		return !out.pos.empty();
	}

	NativePrim nativePrimFor(uint32_t meshHeaderFlags)
	{
		return (meshHeaderFlags & rpMESHHEADERPRIMMASK) == rpMESHHEADERPOINTLIST ? NativePrim::QuadList : NativePrim::Strip;
	}

	void appendNativePrims(std::vector<uint16_t>& outIdx, std::span<const Vertex3D> verts,
		uint32_t base, uint32_t count, NativePrim prim)
	{
		const auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
			if (verts[a].pos == verts[b].pos || verts[b].pos == verts[c].pos || verts[a].pos == verts[c].pos)
			{
				return;
			}
			outIdx.push_back(static_cast<uint16_t>(a));
			outIdx.push_back(static_cast<uint16_t>(b));
			outIdx.push_back(static_cast<uint16_t>(c));
		};
		if (prim == NativePrim::QuadList)
		{
			// The four corners come round the quad, so the split is the 0-2 diagonal. Reading them as a
			// strip instead takes the 1-3 diagonal for the second triangle, which double-covers one half
			// and leaves the other empty: the thin slivers P4's effect billboards used to draw.
			for (uint32_t i = 0; i + 3 < count; i += 4)
			{
				emit(base + i, base + i + 1, base + i + 2);
				emit(base + i, base + i + 2, base + i + 3);
			}
			return;
		}
		for (uint32_t i = 0; i + 2 < count; i++)
		{
			const uint32_t a = base + i, b = base + i + 1, c = base + i + 2;
			(i & 1) ? emit(a, c, b) : emit(a, b, c); // winding alternates each strip step
		}
	}

	void logNativeMesh(uint32_t nativeTableAddr, int mesh)
	{
		uint32_t end = 0;
		const uint32_t chain = meshChain(nativeTableAddr, mesh, end);
		if (!chain)
		{
			std::fprintf(stderr, "[vif] mesh %d: no native chain\n", mesh);
			return;
		}
		int batches = 0;
		std::string formats;
		VifMachine vif;
		vif.run(chain, end, [&](const VifBatch& batch) {
			if (batches == 0)
			{
				for (const VifStream& s : batch.streams)
				{
					formats += unpackName(s.vn, s.vl);
					formats += '/';
					formats += std::to_string(s.dst.size());
					formats += ' ';
				}
				formats += "itop=" + std::to_string(batch.itop);
			}
			batches++;
		});
		NativeMesh nm;
		decodeNativeMesh(nativeTableAddr, mesh, nm);
		std::fprintf(stderr, "[vif] mesh %d chain 0x%08X..0x%08X batches=%d strip=%zu uv=%zu col=%zu nrm=%zu [%s]\n",
			mesh, chain, end, batches, nm.pos.size(), nm.uv.size(), nm.color.size(), nm.normal.size(),
			formats.c_str());
		// The chain's first words, so a walker that loses sync can be read against the real layout
		// rather than guessed at from the formats it reported.
		std::fprintf(stderr, "[vif]   words:");
		for (uint32_t w = 0; w < 24 && chain + w * 4 + 4 <= end; w++)
		{
			std::fprintf(stderr, "%s%08X", w % 8 == 0 ? "\n[vif]    " : " ",
				*guest::ptr<uint32_t>(chain + w * 4));
		}
		std::fprintf(stderr, "\n");
	}
} // namespace sky2
