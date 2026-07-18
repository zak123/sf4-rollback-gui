#pragma once

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>

namespace sf4e {
	namespace Net {
		// Parse "host:port" into a connectable address. Literal IPs
		// (IPv4, or bracketed IPv6) parse directly; anything else is
		// resolved through DNS, taking the first IPv4 result. Returns
		// false when the string carries no usable port, or resolution
		// fails.
		bool ResolveHostPort(const char* input, SteamNetworkingIPAddr& out);
	}
}
