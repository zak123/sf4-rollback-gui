#pragma once

#include <stdint.h>
#include <string>
#include <utility>


#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <nlohmann/json.hpp>

// Only the wire-portable game structs, not the RE headers: the session
// layer also builds on Linux for the dedicated server.
#include "../Dimps/Dimps__Wire.hxx"

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
			Dimps::GameEvents::Wire::ConfirmedCharaConditions chara[2];
			int64_t stageID;
			uint32_t rngSeed;

			// "ip:port" of the UDP relay both games must route GGPO
			// through, set by the server when a seat's NAT can't be
			// traversed directly. Empty means direct P2P (the fast
			// path). The shared relay token is the lobby ID, which both
			// games already hold.
			std::string relayEndpoint;
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

			MT_PRESENCE_LIST,
			MT_PRESENCE_LIST_RESP,
			MT_CHALLENGE_SEND,
			MT_CHALLENGE_EVENT,
			MT_CHALLENGE_ANSWER,
			MT_CHALLENGE_RESULT,
			MT_MATCHMAKE,
			MT_MATCHMAKE_EVENT,
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

			{MT_PRESENCE_LIST, "presence_list"},
			{MT_PRESENCE_LIST_RESP, "presence_list_resp"},
			{MT_CHALLENGE_SEND, "challenge_send"},
			{MT_CHALLENGE_EVENT, "challenge_event"},
			{MT_CHALLENGE_ANSWER, "challenge_answer"},
			{MT_CHALLENGE_RESULT, "challenge_result"},
			{MT_MATCHMAKE, "matchmake"},
			{MT_MATCHMAKE_EVENT, "matchmake_event"},
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

			// The server build's version, so clients can warn the player
			// when they're on a stale release. Empty when talking to a
			// server that predates the field.
			std::string version;
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

			// NAT status this seat's game observed via the probe. Bit 0
			// (NF_NEEDS_RELAY) = a direct connection is unlikely
			// (symmetric NAT, or the probe got no reply); the server
			// routes the match through the relay when either seat sets
			// it.
			uint32_t natFlags = 0;
		};

		enum NatFlag {
			NF_NEEDS_RELAY = 1,
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
			Dimps::GameEvents::Wire::ConfirmedCharaConditions chara;
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

		// One status per display name; a player's app and game
		// connections are folded together server-side.
		enum PresenceStatus {
			PS_LOUNGE = 0,
			PS_LOOKING = 1,
			PS_IN_LOBBY = 2,
			PS_IN_MATCH = 3,
		};

		NLOHMANN_JSON_SERIALIZE_ENUM(PresenceStatus, {
			{PS_LOUNGE, "lounge"},
			{PS_LOOKING, "looking"},
			{PS_IN_LOBBY, "in_lobby"},
			{PS_IN_MATCH, "in_match"},
		})

		struct PresenceEntry {
			std::string name;
			PresenceStatus status = PS_LOUNGE;
		};

		struct PresenceListRequest {
			MessageType type = MT_PRESENCE_LIST;
		};

		struct PresenceListResp {
			MessageType type = MT_PRESENCE_LIST_RESP;
			std::vector<PresenceEntry> players;
			int32_t lookingCount = 0;
		};

		// Private challenges: sender asks for a specific opponent; on
		// acceptance the server seats both in an unlisted lobby.
		struct ChallengeSend {
			MessageType type = MT_CHALLENGE_SEND;
			std::string target;
		};

		struct ChallengeEvent {
			MessageType type = MT_CHALLENGE_EVENT;
			std::string from;
		};

		struct ChallengeAnswer {
			MessageType type = MT_CHALLENGE_ANSWER;
			std::string from;
			bool accept = false;
		};

		enum ChallengeResult {
			CR_ACCEPTED = 0,
			CR_DECLINED = 1,
			CR_EXPIRED = 2,
			CR_BUSY = 3,
			CR_OFFLINE = 4,
		};

		NLOHMANN_JSON_SERIALIZE_ENUM(ChallengeResult, {
			{CR_ACCEPTED, "accepted"},
			{CR_DECLINED, "declined"},
			{CR_EXPIRED, "expired"},
			{CR_BUSY, "busy"},
			{CR_OFFLINE, "offline"},
		})

		struct ChallengeResultMsg {
			MessageType type = MT_CHALLENGE_RESULT;
			std::string target;
			ChallengeResult result = CR_DECLINED;
		};

		// Quickmatch: flagged lounge players are paired oldest-first
		// into unlisted lobbies.
		struct Matchmake {
			MessageType type = MT_MATCHMAKE;
			bool enabled = false;
		};

		struct MatchmakeEvent {
			MessageType type = MT_MATCHMAKE_EVENT;
			std::string opponent;
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
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MatchData, readyMessageNum, chara, stageID, rngSeed, relayEndpoint);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionHelloMsg, type);

		// SessionHelloResp is serialized by hand: `version` is optional
		// on the wire so hellos from servers predating it still parse.
		inline void to_json(nlohmann::json& j, const SessionHelloResp& r) {
			j = nlohmann::json{
				{"type", r.type},
				{"cid", r.cid},
				{"version", r.version},
			};
		}

		inline void from_json(const nlohmann::json& j, SessionHelloResp& r) {
			j.at("type").get_to(r.type);
			j.at("cid").get_to(r.cid);
			r.version = j.value("version", std::string());
		}

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
				{"natFlags", r.natFlags},
			};
		}

		inline void from_json(const nlohmann::json& j, SessionJoinRequest& r) {
			j.at("type").get_to(r.type);
			j.at("sidecarHash").get_to(r.sidecarHash);
			j.at("username").get_to(r.username);
			j.at("port").get_to(r.port);
			r.lobby = j.value("lobby", LobbyID{ "", "" });
			r.handoff = j.value("handoff", std::string());
			r.natFlags = j.value("natFlags", (uint32_t)0);
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
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PresenceEntry, name, status);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PresenceListRequest, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PresenceListResp, type, players, lookingCount);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChallengeSend, type, target);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChallengeEvent, type, from);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChallengeAnswer, type, from, accept);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChallengeResultMsg, type, target, result);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Matchmake, type, enabled);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MatchmakeEvent, type, opponent);
	}
}