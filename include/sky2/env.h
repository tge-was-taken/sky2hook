// The library's debug switches come from the environment, so a run can be shaped without a rebuild.
// Each name is looked up as SKY2_<NAME> first, then under the host's own prefix if it set one, so a
// host that already documents its own flag names keeps them working.
#pragma once

namespace sky2
{
	void setEnvPrefix(const char* prefix); // e.g. "P3FES_"; nullptr clears it

	const char* env(const char* name); // nullptr when unset
	bool envSet(const char* name);
} // namespace sky2
