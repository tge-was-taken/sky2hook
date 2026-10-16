#include "sky2/addresses.h"
#include "sky2/config.h"
#include "sky2/env.h"
#include "sky2/guest.h"
#include "sky2/host.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace sky2::guest
{
	uint8_t* g_ram = nullptr; // the host points this at the guest's main RAM via setRam()
}

namespace sky2
{
	namespace
	{
		Config s_config;
		Addresses s_addresses;
		Host s_defaultHost;
		Host* s_host = &s_defaultHost;
		std::string s_envPrefix;
		std::unordered_map<uint64_t, uint32_t> s_pluginOffsets; // (baseSize << 32 | pluginID) -> offset

		uint64_t pluginKey(uint32_t baseSize, uint32_t pluginID)
		{
			return (static_cast<uint64_t>(baseSize) << 32) | pluginID;
		}
	} // namespace

	void setEnvPrefix(const char* prefix) { s_envPrefix = prefix ? prefix : ""; }

	const char* env(const char* name)
	{
		std::string key = "SKY2_";
		key += name;
		if (const char* v = std::getenv(key.c_str()))
			return v;
		if (s_envPrefix.empty())
			return nullptr;
		key = s_envPrefix + name;
		return std::getenv(key.c_str());
	}

	bool envSet(const char* name) { return env(name) != nullptr; }

	void setConfig(const Config& c) { s_config = c; }
	const Config& config() { return s_config; }

	void setAddresses(const Addresses& a) { s_addresses = a; }
	const Addresses& addresses() { return s_addresses; }

	void setHost(Host& host) { s_host = &host; }
	Host& host() { return *s_host; }

	uint32_t pluginOffset(uint32_t baseSize, uint32_t pluginID, uint32_t offsetGlobal)
	{
		const auto it = s_pluginOffsets.find(pluginKey(baseSize, pluginID));
		const uint32_t fallback = offsetGlobal && guest::ready() ? *guest::ptr<uint32_t>(offsetGlobal) : 0;
		if (it != s_pluginOffsets.end())
		{
			// A normal boot has both values, so it can check the fallback the replay and savestate paths
			// depend on. Those boot past plugin registration and use the fallback exclusively, where a
			// wrong global address is silent: the plugin data is simply never found.
			if (offsetGlobal && fallback != it->second)
			{
				static std::unordered_map<uint64_t, bool> warned;
				if (!warned[pluginKey(baseSize, pluginID)])
				{
					warned[pluginKey(baseSize, pluginID)] = true;
					std::fprintf(stderr,
						"[sky2][critical] plugin fallback wrong: base=0x%X id=0x%X discovered=0x%X but "
						"*0x%08X=0x%X. Replay/savestate runs will miss this plugin's data.\n",
						baseSize, pluginID, it->second, offsetGlobal, fallback);
				}
			}
			return it->second;
		}
		return fallback;
	}

	void recordPluginOffset(uint32_t baseSize, uint32_t pluginID, uint32_t offset)
	{
		s_pluginOffsets[pluginKey(baseSize, pluginID)] = offset;
	}
} // namespace sky2
