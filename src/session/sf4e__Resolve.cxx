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

static const char PROBE_MAGIC[] = "SF4EPROBE";

bool sf4e::Net::Net_ProbeGgpoPort(const SteamNetworkingIPAddr& server, uint16_t nLocalPort, NatProbe& out) {
	out.ok = false;
	out.sock = (uintptr_t)INVALID_SOCKET;
	uint32 serverIp = server.GetIPv4();
	if (!serverIp) {
		return false;
	}

	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET) {
		return false;
	}
	BOOL reuse = TRUE;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
	sockaddr_in local = { 0 };
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = INADDR_ANY;
	local.sin_port = htons(nLocalPort);
	if (bind(s, (sockaddr*)&local, sizeof(local)) != 0) {
		closesocket(s);
		return false;
	}
	DWORD timeoutMs = 400;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

	sockaddr_in dest = { 0 };
	dest.sin_family = AF_INET;
	dest.sin_addr.s_addr = htonl(serverIp);
	dest.sin_port = htons(server.m_port + 1);

	char reply[64] = { 0 };
	for (int attempt = 0; attempt < 3 && !out.ok; attempt++) {
		sendto(s, PROBE_MAGIC, sizeof(PROBE_MAGIC) - 1, 0, (sockaddr*)&dest, sizeof(dest));
		sockaddr_in from = { 0 };
		int fromLen = sizeof(from);
		int got = recvfrom(s, reply, sizeof(reply) - 1, 0, (sockaddr*)&from, &fromLen);
		if (got <= (int)sizeof(PROBE_MAGIC) - 1) {
			continue;
		}
		reply[got] = 0;
		unsigned int a, b, c, d, p;
		if (sscanf_s(reply, "SF4EPROBE %u.%u.%u.%u:%u", &a, &b, &c, &d, &p) == 5 && p > 0 && p < 65536) {
			out.publicPort = (uint16_t)p;
			snprintf(out.publicAddr, sizeof(out.publicAddr), "%u.%u.%u.%u:%u", a, b, c, d, p);
			out.ok = true;
		}
	}
	if (!out.ok) {
		closesocket(s);
		return false;
	}

	// Second destination, same socket: a differing observed port means
	// the NAT maps per destination (symmetric)- the endpoint above is
	// then only valid toward this server, not toward a peer. No reply
	// (older server, unopened firewall port) leaves it unknown.
	dest.sin_port = htons(server.m_port + 2);
	for (int attempt = 0; attempt < 2 && !out.symmetricKnown; attempt++) {
		sendto(s, PROBE_MAGIC, sizeof(PROBE_MAGIC) - 1, 0, (sockaddr*)&dest, sizeof(dest));
		sockaddr_in from = { 0 };
		int fromLen = sizeof(from);
		int got = recvfrom(s, reply, sizeof(reply) - 1, 0, (sockaddr*)&from, &fromLen);
		if (got <= (int)sizeof(PROBE_MAGIC) - 1) {
			continue;
		}
		reply[got] = 0;
		unsigned int a, b, c, d, p;
		if (sscanf_s(reply, "SF4EPROBE %u.%u.%u.%u:%u", &a, &b, &c, &d, &p) == 5 && p > 0 && p < 65536) {
			out.publicPort2 = (uint16_t)p;
			out.symmetricKnown = true;
			out.symmetric = out.publicPort2 != out.publicPort;
		}
	}

	// Keepalive phase: non-blocking so per-frame ticks never stall.
	u_long nonblock = 1;
	ioctlsocket(s, FIONBIO, &nonblock);
	out.sock = (uintptr_t)s;
	return true;
}

void sf4e::Net::Net_ProbeKeepalive(NatProbe& probe, const SteamNetworkingIPAddr& server) {
	if (!probe.ok || (SOCKET)probe.sock == INVALID_SOCKET) {
		return;
	}
	// Drain stale echoes, then refresh the NAT mapping.
	char sink[64];
	while (recv((SOCKET)probe.sock, sink, sizeof(sink), 0) > 0) {
	}
	sockaddr_in dest = { 0 };
	dest.sin_family = AF_INET;
	dest.sin_addr.s_addr = htonl(server.GetIPv4());
	dest.sin_port = htons(server.m_port + 1);
	sendto((SOCKET)probe.sock, PROBE_MAGIC, sizeof(PROBE_MAGIC) - 1, 0, (sockaddr*)&dest, sizeof(dest));
}

void sf4e::Net::Net_ProbeClose(NatProbe& probe) {
	if ((SOCKET)probe.sock != INVALID_SOCKET) {
		closesocket((SOCKET)probe.sock);
		probe.sock = (uintptr_t)INVALID_SOCKET;
	}
	probe.ok = false;
}

bool sf4e::Net::Net_RelayRegister(const char* relayEndpoint, uint16_t nLocalPort, const char* token, int seat) {
	SteamNetworkingIPAddr relay;
	relay.Clear();
	if (!relay.ParseString(relayEndpoint) || relay.m_port == 0) {
		return false;
	}
	uint32 relayIp = relay.GetIPv4();
	if (!relayIp) {
		return false;
	}

	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET) {
		return false;
	}
	BOOL reuse = TRUE;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
	sockaddr_in local = { 0 };
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = INADDR_ANY;
	local.sin_port = htons(nLocalPort);
	if (bind(s, (sockaddr*)&local, sizeof(local)) != 0) {
		closesocket(s);
		return false;
	}

	sockaddr_in dest = { 0 };
	dest.sin_family = AF_INET;
	dest.sin_addr.s_addr = htonl(relayIp);
	dest.sin_port = htons(relay.m_port);

	char msg[160];
	int len = snprintf(msg, sizeof(msg), "SF4ERELAY %s %d", token, seat);

	// A few sends so a dropped registration still lands before GGPO
	// takes the port; the relay keeps re-learning the endpoint from
	// live traffic anyway.
	for (int i = 0; i < 4; i++) {
		sendto(s, msg, len, 0, (sockaddr*)&dest, sizeof(dest));
	}

	closesocket(s);
	return true;
}
