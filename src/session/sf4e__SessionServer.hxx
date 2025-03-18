#pragma once

#include <map>
#include <string>
#include <vector>

#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <nlohmann/json.hpp>

#include "../Dimps/Dimps__Math.hxx"
#include "sf4e__SessionProtocol.hxx"

namespace sf4e {
	extern const int SESSION_SERVER_MAX_MESSAGES_PER_POLL;

	class SessionServer
	{
	private:
		// Connection related data
		std::string _sidecarHash;
		HSteamListenSocket _listenSock;
		HSteamNetPollGroup _pollGroup;
		ISteamNetworkingSockets* _interface;

		// Connection callbacks and message utilities
		static SessionServer* s_pCallbackInstance;
		static void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo);
		void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);
		void BroadcastMessage(nlohmann::json& msg);
		void Respond(HSteamNetConnection client, nlohmann::json& msg);

		// Direct lobby data manipulation utilities
		SessionProtocol::JoinResult RegisterToWait(
			const HSteamNetConnection& conn,
			const uint16_t& port,
			const std::string& sidecarHash,
			const std::string& name,
			const SteamNetworkingIPAddr& peerAddr
		);
		void HandleResults(int loserSide);

	public:
		SessionServer(
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

		typedef struct SessionMember {
			SessionProtocol::MemberData data;
			HSteamNetConnection conn;
		} SessionMember;

		std::vector<SessionMember> clients;


		// Lobby data: Public for visibility into tests only.
		bool _dataDirty;
		SessionProtocol::LobbyData _lobbyData;
		SessionProtocol::MatchData _matchData;
	};
}