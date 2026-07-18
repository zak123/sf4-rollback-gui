#include <stdlib.h>
#include <string>
#include <string.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>

#include "sf4e__Resolve.hxx"

bool sf4e::Net::ResolveHostPort(const char* input, SteamNetworkingIPAddr& out) {
	out.Clear();

	// Literal addresses parse directly.
	if (out.ParseString(input)) {
		return out.m_port != 0;
	}

	// hostname:port- hostnames contain no colons, so split on the last
	// one. (Unbracketed IPv6 with a port isn't representable either
	// way; ParseString above covers the bracketed form.)
	const char* colon = strrchr(input, ':');
	if (!colon || colon == input) {
		return false;
	}
	std::string host(input, colon - input);
	int port = atoi(colon + 1);
	if (port <= 0 || port > 65535) {
		return false;
	}

	// Resolution requires an initialized Winsock; every caller runs
	// after GameNetworkingSockets_Init, which handles WSAStartup.
	addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo* results = nullptr;
	if (getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0 || !results) {
		return false;
	}
	uint32 ipHostOrder = ntohl(((sockaddr_in*)results->ai_addr)->sin_addr.s_addr);
	freeaddrinfo(results);

	out.SetIPv4(ipHostOrder, (uint16)port);
	return true;
}
