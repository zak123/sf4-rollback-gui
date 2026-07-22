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
			SCE_JOIN_REJECTED_HANDOFF_INVALID,
			SCE_JOIN_REJECTED_SERVER_FULL,
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
			void (*OnChat)(const SessionProtocol::ChatEvent& event, SessionClient* const client, const Callbacks& callbacks);
			void (*OnMatchHandoff)(const SessionProtocol::MatchHandoff& handoff, SessionClient* const client, const Callbacks& callbacks);
			void (*OnPresence)(SessionClient* const client, const Callbacks& callbacks);
			void (*OnChallengeEvent)(const SessionProtocol::ChallengeEvent& event, SessionClient* const client, const Callbacks& callbacks);
			void (*OnChallengeResult)(const SessionProtocol::ChallengeResultMsg& result, SessionClient* const client, const Callbacks& callbacks);
			void (*OnMatchmakeEvent)(const SessionProtocol::MatchmakeEvent& event, SessionClient* const client, const Callbacks& callbacks);
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
		std::vector<SessionProtocol::PresenceEntry> _presence;
		int32_t _lookingCount = 0;
		int64_t _outstandingReadyRequestNumber = -1;
		bool _snapshotsEnabled;

		// Set before Connect() to make the automatic post-hello join
		// request target a specific lobby, optionally presenting a seat
		// handoff token. Left as defaults, the join request carries the
		// null lobby, preserving the older register-into-default flow.
		SessionProtocol::LobbyID _autoJoinLobby = { "", "" };
		std::string _autoJoinHandoff;

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

		EResult Chat_Send(const std::string& channel, const std::string& text);

		EResult Presence_RequestList();
		EResult Challenge_Send(const std::string& target);
		EResult Challenge_Answer(const std::string& from, bool accept);
		EResult Matchmake_Set(bool enabled);

		EResult PreBattle_SetEnv(uint32_t rngSeed);
		EResult PreBattle_SetChara(const Dimps::GameEvents::VsMode::ConfirmedCharaConditions& chara);
		EResult PreBattle_SetStage(int32_t stageID);

		EResult Battle_Loaded();
		EResult Battle_Ended();

		EResult Forward(const SessionProtocol::ConnectionID& dest, const nlohmann::json& msg);

		// Public for testing
		EResult Send(nlohmann::json& msg, int64_t* outMessageNum);

		// Round-trip ping to the session server in ms, or -1 while
		// disconnected.
		int GetPingMs();

		// The server's self-reported version from hello_resp. Empty until
		// the handshake lands, and still empty on servers that predate
		// the field (which itself means "older than this build").
		std::string _serverVersion;
		
		// Connection related data - public for testing
		std::string _sidecarHash;
		uint16_t _ggpoPort;

		// The port reported to the server for this client's seat.
		// Defaults to the local GGPO port; a successful NAT probe
		// replaces it with the public port the NAT maps that socket
		// to, which is what the opponent must actually fire at.
		uint16_t _reportedGgpoPort;

		// NAT flags reported in join requests (SessionProtocol::NatFlag
		// bits). The game sets NF_NEEDS_RELAY when its probe found a
		// symmetric NAT or got no reply.
		uint32_t _natFlags = 0;
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
