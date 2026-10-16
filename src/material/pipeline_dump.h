// RxPipeline introspection (P3FES_PIPE_DUMP): the VU1 microprogram and the GS registers a pipeline
// uploads, neither of which is reachable from the RW API.
#pragma once

#include <cstdint>
#include <string>

namespace sky2
{
	// What one RxPipeline uploads, read once per pipeline address and cached.
	struct PipeDump
	{
		uint32_t pipeline = 0;
		uint32_t pipeId = 0; // PDS pipe id (RxPipeline +0x30), 0 when it has none
		std::string nodes; // node definition names, in order
		uint32_t vuCode = 0; // guest address of the microprogram's DMA chain
		uint32_t vuInstrs = 0; // microinstructions found in it
		uint64_t vuHash = 0; // content hash: two draws share a program or they do not
		std::string dumpFile; // where the dump went, empty when there was none
		std::string gsRegs; // GS registers found in the node data, decoded
	};

	bool pipeDumpEnabled();
	// Walk a pipeline and cache the result. Cheap after the first call for a given address.
	const PipeDump& dumpPipeline(uint32_t pipelinePtr);
} // namespace sky2
