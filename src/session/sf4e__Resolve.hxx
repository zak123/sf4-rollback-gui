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

		// STUN-lite NAT probe. Every session server answers UDP
		// datagrams one port above its main port with the sender's
		// observed "ip:port"- which is the endpoint a peer must fire
		// GGPO traffic at. Clients used to report their local port,
		// which most NATs rewrite; that was the top cause of the
		// 45-second "could not reach the opponent" abort.
		struct NatProbe {
			uintptr_t sock = (uintptr_t)~0;
			uint16_t publicPort = 0;
			char publicAddr[48] = { 0 };
			bool ok = false;

			// Symmetric detection: the same socket probes a second
			// echo port. A NAT that maps the same local port to
			// DIFFERENT public ports per destination is symmetric-
			// peers can't reach the probed endpoint, and only a port
			// forward (or a relay) will connect it.
			bool symmetricKnown = false;
			bool symmetric = false;
			uint16_t publicPort2 = 0;
		};

		// Bind nLocalPort and ask the server's echo what public
		// endpoint the NAT maps it to. On success the socket STAYS
		// OPEN so keepalives can hold the mapping until GGPO takes the
		// port over; call Net_ProbeClose right before GGPO binds.
		bool Net_ProbeGgpoPort(const SteamNetworkingIPAddr& server, uint16_t nLocalPort, NatProbe& out);
		void Net_ProbeKeepalive(NatProbe& probe, const SteamNetworkingIPAddr& server);
		void Net_ProbeClose(NatProbe& probe);
	}
}
