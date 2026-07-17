#pragma once

#include <map>
#include <string>
#include <vector>

#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <nlohmann/json.hpp>

#include "../Dimps/Dimps__Math.hxx"
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

			// Chat rate limiting: a ring of the timestamps of this
			// peer's most recent sends. A send is dropped when the
			// slot it would overwrite is younger than the rate window.
			uint64_t chatStamps[5] = { 0 };
			int chatStampIdx = 0;
		} Peer;

		// The most registered peers this server admits; zero means
		// unlimited. Seat handoffs are exempt- their games pair with
		// already-admitted peers.
		size_t maxPeers = 0;

		// Public for visibility into tests only.
		std::map<HSteamNetConnection, SessionProtocol::ConnectionID> cidMap;
		std::map<HSteamNetConnection, Peer> peers;
		LobbyRegistry registry;
	};
}
