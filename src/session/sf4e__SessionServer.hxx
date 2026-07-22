#pragma once

#include <map>
#include <string>
#include <vector>

#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <nlohmann/json.hpp>

#include "../Dimps/Dimps__Wire.hxx"
#include "sf4e__LobbyRegistry.hxx"
#include "sf4e__SessionProtocol.hxx"

namespace sf4e {
	extern const int SESSION_SERVER_MAX_MESSAGES_PER_POLL;

	class SessionServer
	{
	private:
		// A binary blob usable by any host for routing messages to this
		// server. This is most likely an IP address and port, a hostname
		// and port, or in extreme cases an overlay network's concept of
		// addressing (ex. an index in a service discovery protocol).
		// Identities of clients connected to the server are prefixed with
		// this identity. This allows all servers in a cluster to forward
		// messages to any user connected to any server in the cluster-
		// just send it to the prefixed identity, and that host will take
		// care of the rest.
		std::string _identity;

		// Connection related data
		std::string _sidecarHash;
		HSteamListenSocket _listenSock;
		HSteamNetPollGroup _pollGroup;
		ISteamNetworkingSockets* _interface;

		// Raw UDP echoes one and two ports above the listen port:
		// clients probe both from their GGPO socket- the first learns
		// the public endpoint their NAT maps that port to, and a
		// differing answer from the second exposes a symmetric NAT
		// (per-destination mappings). ~0 when unbound.
		uintptr_t _probeSockets[2] = { (uintptr_t)~0, (uintptr_t)~0 };

		// UDP relay, three ports above the listen port. Games whose NAT
		// can't be traversed directly register here from their GGPO
		// socket with the lobby ID as a token, then point GGPO at this
		// socket; it cross-forwards datagrams between the two
		// registered endpoints of each match. ~0 when unbound.
		uintptr_t _relaySocket = (uintptr_t)~0;
		uint16_t _relayPort = 0;

		// One relay pairing per match token (the lobby ID string). Each
		// side's public endpoint is learned from its registration
		// datagram; packets from one endpoint forward to the other.
		struct RelayPair {
			uint32_t addr[2] = { 0, 0 };   // network-order IPv4
			uint16_t port[2] = { 0, 0 };   // network-order
			bool present[2] = { false, false };
			uint64_t lastSeenMs = 0;
		};
		std::map<std::string, RelayPair> _relayPairs;
		void StepRelay();

		// The registry key of the default lobby, if this server was
		// constructed with one. Empty in dedicated mode. Tracked
		// explicitly- a user-created lobby can coincidentally hold any
		// key, so key values alone can't identify the default lobby.
		std::string _defaultLobbyKey;

		// Connection callbacks and message utilities
		static SessionServer* s_pCallbackInstance;
		static void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo);
		void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);
		void BroadcastToLobby(Lobby& lobby, nlohmann::json& msg);
		void BroadcastToPeers(nlohmann::json& msg);
		void Respond(HSteamNetConnection client, nlohmann::json& msg);
		void RespondNullLobbyUpdate(HSteamNetConnection client);
		void RespondJoinReject(HSteamNetConnection client, SessionProtocol::JoinResult result);

		// Direct lobby data manipulation utilities
		void SeatInLobby(HSteamNetConnection conn, Lobby& lobby);
		void RemoveFromLobby(HSteamNetConnection conn);
		void HandleResults(Lobby& lobby, int loserSide);
		Lobby* CreatePairLobby(const std::string& displayName, HSteamNetConnection a, HSteamNetConnection b);
		void RespondChallengeResult(HSteamNetConnection conn, const std::string& target, SessionProtocol::ChallengeResult result);
		void StepMatchmaking();

	public:
		// Hosts no lobbies at startup: every lobby is created by a
		// client, and clients joining without naming a lobby stay in
		// the lounge. This is the dedicated-server mode.
		SessionServer(
			std::string identity,
			std::string sidecarHash
		);

		// Hosts a persistent default lobby with the given settings, and
		// clients joining without naming a lobby are seated there. This
		// matches the behavior in-game hosts and existing clients expect.
		SessionServer(
			std::string identity,
			std::string sidecarHash,
			bool editionSelect,
			int roundCount,
			Dimps::Math::FixedPoint roundTime
		);
		~SessionServer();

		void AddConnection(HSteamNetConnection newConn);
		int Listen(uint16 nPort);
		int Step();
		int Close();
		void PrepareForCallbacks();
		void ResetBattleSync();

		Lobby* GetDefaultLobby();

		// A user that has said hello and successfully registered a name
		// with a join request. Peers may or may not be seated in a lobby;
		// `lobbyKey` is empty for peers idling in the lounge.
		typedef struct Peer {
			SessionProtocol::MemberData data;
			std::string lobbyKey;

			// NAT status this peer's game reported (SessionProtocol
			// NatFlag bits). Drives whether a match it seats in routes
			// through the relay.
			uint32_t natFlags = 0;

			// Chat rate limiting: a ring of the timestamps of this
			// peer's most recent sends. A send is dropped when the
			// slot it would overwrite is younger than the rate window.
			uint64_t chatStamps[5] = { 0 };
			int chatStampIdx = 0;

			// Quickmatch queue membership; lookingSinceMs orders the
			// FIFO pairing.
			bool lookingForMatch = false;
			uint64_t lookingSinceMs = 0;

			// This peer's outstanding outgoing challenge, if any.
			std::string challengeTarget;
			uint64_t challengeSentAtMs = 0;
		} Peer;

		// The most registered peers this server admits; zero means
		// unlimited. Seat handoffs are exempt- their games pair with
		// already-admitted peers.
		size_t maxPeers = 0;

		// Match settings applied to server-created (challenge and
		// quickmatch) lobbies. The default-lobby constructor mirrors its
		// settings here; Lobbyd sets them from its flags.
		bool matchEditionSelect = true;
		int matchRoundCount = 3;
		Dimps::Math::FixedPoint matchRoundTime = { 0, 99 };

		// How long an unanswered challenge lives. Public so tests can
		// shrink it.
		uint64_t challengeTtlMs = 30 * 1000;

		// Public for visibility into tests only.
		std::map<HSteamNetConnection, SessionProtocol::ConnectionID> cidMap;
		std::map<HSteamNetConnection, Peer> peers;
		LobbyRegistry registry;
	};
}
