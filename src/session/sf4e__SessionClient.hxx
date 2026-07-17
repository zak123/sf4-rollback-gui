#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>

#include "sf4e__SessionProtocol.hxx"

namespace sf4e {
	extern const int SESSION_CLIENT_MAX_MESSAGES_PER_POLL;

	class SessionClient
	{

	public:
		static bool bVerboseLogging;

		enum ErrorType {
			SCE_UNKNOWN,
			SCE_JOIN_REJECTED_HASH_INVALID,
			SCE_JOIN_REJECTED_LOBBY_FULL,
			SCE_JOIN_REJECTED_NAME_TAKEN,
			SCE_JOIN_REJECTED_REQUEST_INVALID,
			SCE_JOIN_REJECTED_NOT_REGISTERED,
			SCE_JOIN_REJECTED_ALREADY_IN_LOBBY,
			SCE_JOIN_REJECTED_NO_SUCH_LOBBY,
		};

		// Callback members past OnBattleSynced may be left null by users
		// that don't care about them; the client checks before calling.
		struct Callbacks {
			void* data;
			void (*OnError)(ErrorType errorType, SessionClient* const client, const Callbacks& callbacks);
			void (*OnReady)(SessionClient* const client, const Callbacks& callbacks);
			void (*OnBattleSynced)(SessionClient* const client, const Callbacks& callbacks);
			void (*OnLobbyCreated)(SessionProtocol::JoinResult result, SessionClient* const client, const Callbacks& callbacks);
			void (*OnLobbyList)(SessionClient* const client, const Callbacks& callbacks);
		};

		SessionClient(
			const Callbacks& callbacks,
			std::string sidecarHash,
			uint16_t ggpoPort,
			std::string& name
		);
		~SessionClient();

		int Connect(HSteamNetConnection newConn);
		int Connect(const SteamNetworkingIPAddr& serverAddr);
		void Disconnect();
		int Step();
		void PrepareForCallbacks();

		// Lobby data
		std::string _name;
		SessionProtocol::LobbyData _lobbyData;
		SessionProtocol::MatchData _matchData;
		std::vector<SessionProtocol::LobbyListEntry> _lobbyListing;
		int64_t _outstandingReadyRequestNumber = -1;
		bool _snapshotsEnabled;

		EResult Lobby_Create(
			const std::string& name,
			bool editionSelect,
			int32_t roundCount,
			Dimps::Math::FixedPoint roundTime
		);
		EResult Lobby_RequestList();
		EResult Lobby_Join(const SessionProtocol::LobbyID& id);
		EResult Lobby_Leave();
		EResult Lobby_Ready();
		EResult Lobby_ReportResults(int loserSide);

		EResult PreBattle_SetEnv(uint32_t rngSeed);
		EResult PreBattle_SetChara(const Dimps::GameEvents::VsMode::ConfirmedCharaConditions& chara);
		EResult PreBattle_SetStage(int32_t stageID);

		EResult Battle_Loaded();

		EResult Forward(const SessionProtocol::ConnectionID& dest, const nlohmann::json& msg);

		// Public for testing
		EResult Send(nlohmann::json& msg, int64_t* outMessageNum);
		
		// Connection related data - public for testing
		std::string _sidecarHash;
		uint16_t _ggpoPort;
		SteamNetworkingIPAddr _serverAddr;
		std::map<int, SessionProtocol::StateSnapshot> pendingRemoteSnapshots;
		SessionProtocol::ConnectionID _cid;
	private:

		// Connection related data
		Callbacks _callbacks;
		bool _connected = false;
		HSteamNetConnection _conn;
		ISteamNetworkingSockets* _interface;


		void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

		static SessionClient* s_pCallbackInstance;
		static void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo);
	};
}
