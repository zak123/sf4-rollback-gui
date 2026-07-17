#pragma once

#include <string>
#include <utility>


#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <nlohmann/json.hpp>

#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Math.hxx"

#define MAX_SF4E_PROTOCOL_USERS 4

namespace sf4e {
	namespace SessionProtocol {
		typedef Dimps::Math::FixedPoint FixedPoint; 

		// Connection IDs are ephemeral and reusable- they can be used to
		// distinguish clients from each other, but should not be used as
		// any kind of stable identifier. "user" in this context is _not_
		// a typical user account- it only correspond to the `username`
		// portion of a URL. Care should be taken that any references to
		// connection IDs (including those inside client code) are released
		// or deleted when the connection is terminated, to prevent
		// referring to duplicate IDs.
		struct ConnectionID {
			std::string host;
			std::string user;

			bool operator==(const ConnectionID&);
		};

		// Similar to connection IDs, lobby IDs are ephemeral and reusable.
		// All lobby IDs containing an empty string as the host or key are
		// equivalent and unreachable, and the canonical null lobby, which
		// contains no members, is represented as {"", ""}.
		struct LobbyID {
			std::string host;
			std::string key;

			bool operator==(const LobbyID& rhs);
			static const LobbyID NULL_LOBBY_ID;
		};

		enum MemberFlags {
			MF_BATTLE_LOADED = 1,
		};

		struct MemberData {
			ConnectionID connId;

			// A user-provided display name.
			std::string name;

			// The IP the client has connected from. While not guaranteed to
			// be reachable, other P2P libraries (ex. GGPO) can try to leverage
			// this to connect directly.
			std::string ip;
			uint16_t port;
			uint64_t flags;
		};

		struct LobbyData {
			LobbyID id;

			// A user-provided display name for the lobby, ex. for
			// lobby-browser listings.
			std::string name;

			bool editionSelect;
			int roundCount;
			FixedPoint roundTime;
			std::vector<MemberData> members;

			static const LobbyData NULL_LOBBY;
		};

		struct MatchData {
			MatchData();
			void Clear();
			bool IsAllReady();

			int64_t readyMessageNum[2];
			Dimps::GameEvents::VsMode::ConfirmedCharaConditions chara[2];
			int64_t stageID;
			DWORD rngSeed;
		};

		enum MessageType {
			MT_SESSION_HELLO,
			MT_SESSION_HELLO_RESP,

			MT_SESSION_DATAUPDATE,
			MT_SESSION_JOINREQ,
			MT_SESSION_JOINREJ,

			MT_LOBBY_READY,
			MT_LOBBY_ALLREADY,
			MT_LOBBY_REPORTRESULTS,

			MT_PREBATTLE_SETENV,
			MT_PREBATTLE_SETCHARA,
			MT_PREBATTLE_SETSTAGE,

			MT_BATTLE_LOADED,
			MT_BATTLE_SYNCED,
			MT_BATTLE_SNAPSHOT,

			MT_FORWARD,

			MT_LOBBY_CREATE,
			MT_LOBBY_CREATE_RESP,
			MT_LOBBY_LIST,
			MT_LOBBY_LIST_RESP,
			MT_LOBBY_LEAVE,

			MT_CHAT_SEND,
			MT_CHAT_EVENT,

			MT_MATCH_HANDOFF,

			MT_BATTLE_ENDED,
		};

		NLOHMANN_JSON_SERIALIZE_ENUM(MessageType, {
			{MT_SESSION_HELLO, "hello"},
			{MT_SESSION_HELLO_RESP, "hello_resp"},
			{MT_SESSION_DATAUPDATE, "data_update"},
			{MT_SESSION_JOINREJ, "join_rej"},
			{MT_SESSION_JOINREQ, "join_req"},

			{MT_LOBBY_READY, "lobby_ready"},
			{MT_LOBBY_ALLREADY, "lobby_allready"},
			{MT_LOBBY_REPORTRESULTS, "lobby_reportresults"},

			{MT_PREBATTLE_SETENV, "prebattle_setenv"},
			{MT_PREBATTLE_SETCHARA, "prebattle_setchara"},
			{MT_PREBATTLE_SETSTAGE, "prebattle_setstage"},

			{MT_BATTLE_LOADED, "battle_loaded"},
			{MT_BATTLE_SYNCED, "battle_synced"},
			{MT_BATTLE_SNAPSHOT, "battle_snapshot"},

			{MT_FORWARD, "forward"},

			{MT_LOBBY_CREATE, "lobby_create"},
			{MT_LOBBY_CREATE_RESP, "lobby_create_resp"},
			{MT_LOBBY_LIST, "lobby_list"},
			{MT_LOBBY_LIST_RESP, "lobby_list_resp"},
			{MT_LOBBY_LEAVE, "lobby_leave"},

			{MT_CHAT_SEND, "chat_send"},
			{MT_CHAT_EVENT, "chat_event"},

			{MT_MATCH_HANDOFF, "match_handoff"},

			{MT_BATTLE_ENDED, "battle_ended"},
		})

		enum JoinResult {
			JOIN_OK = 0,
			JR_REQUEST_INVALID = 1,
			JR_LOBBY_FULL = 2,
			JR_NAME_TAKEN = 3,
			JR_HASH_INVALID = 4,
			JR_NOT_REGISTERED = 5,
			JR_ALREADY_IN_LOBBY = 6,
			JR_NO_SUCH_LOBBY = 7,
			JR_HANDOFF_INVALID = 8,
			JR_SERVER_FULL = 9,
		};

		NLOHMANN_JSON_SERIALIZE_ENUM(JoinResult, {
			{JOIN_OK, "ok"},
			{JR_REQUEST_INVALID, "request_invalid"},
			{JR_LOBBY_FULL, "lobby_full"},
			{JR_NAME_TAKEN, "name_taken"},
			{JR_HASH_INVALID, "hash_invalid"},
			{JR_NOT_REGISTERED, "not_registered"},
			{JR_ALREADY_IN_LOBBY, "already_in_lobby"},
			{JR_NO_SUCH_LOBBY, "no_such_lobby"},
			{JR_HANDOFF_INVALID, "handoff_invalid"},
			{JR_SERVER_FULL, "server_full"}
		})

		struct SessionHelloMsg {
			MessageType type = MT_SESSION_HELLO;
			ConnectionID cid;
		};

		struct SessionHelloResp {
			MessageType type = MT_SESSION_HELLO_RESP;
			ConnectionID cid;
		};

		struct SessionDataUpdate {
			MessageType type = MT_SESSION_DATAUPDATE;
			LobbyData lobbyData;
			MatchData matchData;
		};

		struct SessionJoinReject {
			MessageType type = MT_SESSION_JOINREJ;
			JoinResult result;
		};

		struct SessionJoinRequest {
			MessageType type = MT_SESSION_JOINREQ;
			std::string sidecarHash;
			std::string username;
			uint16_t port;

			// Which lobby to take a seat in. The null lobby ID means
			// "server default": servers hosting a default lobby (ex. an
			// in-game host) seat the sender there, and dedicated servers
			// leave the sender registered-but-unseated, in the lounge.
			LobbyID lobby = { "", "" };

			// One-shot token authorizing this connection to take over a
			// seat already held under the same name, ex. a game process
			// taking over from the lobby app that launched it. Unused
			// until match handoff lands; carried now so the message shape
			// is stable.
			std::string handoff;
		};

		struct LobbyCreate {
			MessageType type = MT_LOBBY_CREATE;
			std::string name;
			bool editionSelect = true;
			int32_t roundCount = 3;
			FixedPoint roundTime = { 0, 99 };
		};

		struct LobbyCreateResp {
			MessageType type = MT_LOBBY_CREATE_RESP;
			JoinResult result = JOIN_OK;
			LobbyID lobby = { "", "" };
		};

		struct LobbyListEntry {
			LobbyID id = { "", "" };
			std::string name;
			int32_t playerCount = 0;
			int32_t capacity = 0;
			std::vector<std::string> players;
		};

		struct LobbyListRequest {
			MessageType type = MT_LOBBY_LIST;
		};

		struct LobbyListResp {
			MessageType type = MT_LOBBY_LIST_RESP;
			std::vector<LobbyListEntry> lobbies;
		};

		struct LobbyLeave {
			MessageType type = MT_LOBBY_LEAVE;
		};

		// The reserved chat channel every registered peer belongs to.
		// Any other channel value names a lobby key, and reaches only
		// that lobby's members.
		extern const char* const CHAT_CHANNEL_LOUNGE;

		// The most bytes of chat text a message may carry; the server
		// truncates anything longer.
		const size_t CHAT_TEXT_MAX = 256;

		struct ChatSend {
			MessageType type = MT_CHAT_SEND;
			std::string channel;
			std::string text;
		};

		struct ChatEvent {
			MessageType type = MT_CHAT_EVENT;
			std::string channel;
			std::string from;
			std::string text;
			int64_t ts = 0;
		};

		// Sent to each seated connection when its lobby goes all-ready.
		// The receiver (a lobby app) passes the token to the game process
		// it launches; the game presents the token in a join request to
		// take over the seat.
		struct MatchHandoff {
			MessageType type = MT_MATCH_HANDOFF;
			std::string token;
			LobbyID lobby = { "", "" };
		};

		struct LobbyReady {
			MessageType type = MT_LOBBY_READY;
		};

		struct LobbyAllReady {
			MessageType type = MT_LOBBY_ALLREADY;
		};

		struct LobbyReportResults {
			MessageType type = MT_LOBBY_REPORTRESULTS;
			int32_t loserSide;
		};

		struct PreBattleSetEnv {
			MessageType type = MT_PREBATTLE_SETENV;
			uint32_t rngSeed;
		};

		struct PreBattleSetChara {
			MessageType type = MT_PREBATTLE_SETCHARA;
			Dimps::GameEvents::VsMode::ConfirmedCharaConditions chara;
		};

		struct PreBattleSetStage {
			MessageType type = MT_PREBATTLE_SETSTAGE;
			int32_t stageID;
		};

		struct BattleLoaded {
			MessageType type = MT_BATTLE_LOADED;
		};

		struct BattleSynced {
			MessageType type = MT_BATTLE_SYNCED;
		};

		// Sent by a game when its battle leaves the foreground. The
		// server resets the ready/loaded cycle so the seated players
		// can ready up for a rematch- results reporting is optional and
		// only matters for queue rotation.
		struct BattleEnded {
			MessageType type = MT_BATTLE_ENDED;
		};

		struct StateSnapshot {
			struct CharaStateSnapshot {
				int status;
				float rootPos[4];
				int side;
				FixedPoint vit;
				FixedPoint vitmax;
				FixedPoint revenge;
				FixedPoint revengemax;
				FixedPoint recoverable;
				FixedPoint recoverablemax;
				FixedPoint super;
				FixedPoint supermax;
				FixedPoint sctimeamt;
				FixedPoint sctimemax;
				FixedPoint uctime;
				FixedPoint uctimemax;
				FixedPoint damage;
				FixedPoint combodamage;
			};

			int frameIdx;
			CharaStateSnapshot chara[2];
		};

		struct BattleSnapshot {
			MessageType type = MT_BATTLE_SNAPSHOT;
			StateSnapshot snapshot;
		};

		struct ForwardMessage {
			MessageType type = MT_FORWARD;
			ConnectionID src;
			ConnectionID dest;
			nlohmann::json msg;
		};

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ConnectionID, host, user);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyID, host, key);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MemberData, connId, name, ip, port);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyData, id, name, editionSelect, roundCount, roundTime, members);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MatchData, readyMessageNum, chara, stageID, rngSeed);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionHelloMsg, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionHelloResp, type, cid);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionDataUpdate, type, lobbyData, matchData);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionJoinReject, type, result);

		// SessionJoinRequest is serialized by hand: `lobby` and `handoff`
		// are optional on the wire so join requests from builds predating
		// them still parse.
		inline void to_json(nlohmann::json& j, const SessionJoinRequest& r) {
			j = nlohmann::json{
				{"type", r.type},
				{"sidecarHash", r.sidecarHash},
				{"username", r.username},
				{"port", r.port},
				{"lobby", r.lobby},
				{"handoff", r.handoff},
			};
		}

		inline void from_json(const nlohmann::json& j, SessionJoinRequest& r) {
			j.at("type").get_to(r.type);
			j.at("sidecarHash").get_to(r.sidecarHash);
			j.at("username").get_to(r.username);
			j.at("port").get_to(r.port);
			r.lobby = j.value("lobby", LobbyID{ "", "" });
			r.handoff = j.value("handoff", std::string());
		}

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyCreate, type, name, editionSelect, roundCount, roundTime);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyCreateResp, type, result, lobby);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyListEntry, id, name, playerCount, capacity, players);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyListRequest, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyListResp, type, lobbies);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyLeave, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChatSend, type, channel, text);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChatEvent, type, channel, from, text, ts);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MatchHandoff, type, token, lobby);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyReady, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyAllReady, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyReportResults, type, loserSide);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PreBattleSetChara, type, chara);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PreBattleSetEnv, type, rngSeed);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PreBattleSetStage, type, stageID);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ForwardMessage, type, src, dest, msg);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StateSnapshot::CharaStateSnapshot, status, rootPos, side, vit, vitmax, revenge, revengemax, recoverable, recoverablemax, super, supermax, sctimeamt, sctimemax, uctime, uctimemax, damage, combodamage);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StateSnapshot, frameIdx, chara);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BattleSnapshot, type, snapshot);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BattleLoaded, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BattleSynced, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BattleEnded, type);
	}
}