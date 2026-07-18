#include <random>
#include <string>
#include <time.h>
#include <utility>
#include <vector>
#include <windows.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>

#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Math.hxx"

#include "sf4e__LobbyRegistry.hxx"
#include "sf4e__SessionProtocol.hxx"
#include "sf4e__SessionServer.hxx"

using nlohmann::json;

namespace SessionProtocol = sf4e::SessionProtocol;
using Dimps::Math::FixedPoint;
using sf4e::Lobby;
using sf4e::LobbyMember;
using sf4e::SessionServer;


const int sf4e::SESSION_SERVER_MAX_MESSAGES_PER_POLL = 200;

// Chat rate limiting: each peer may send CHAT_RATE_BURST messages per
// rolling CHAT_RATE_WINDOW_MS. The burst count must match the size of
// Peer::chatStamps.
static const int CHAT_RATE_BURST = 5;
static const uint64_t CHAT_RATE_WINDOW_MS = 2000;

// How long a seat-handoff token stays valid. Generously covers the
// launch-plus-boot time of the game process it's issued for.
static const uint64_t HANDOFF_TTL_MS = 60 * 1000;

static std::string GenerateHandoffToken() {
	static std::mt19937_64 gen(std::random_device{}());
	char buf[33];
	snprintf(
		buf,
		sizeof(buf),
		"%016llx%016llx",
		(unsigned long long)gen(),
		(unsigned long long)gen()
	);
	return std::string(buf);
}

SessionServer* SessionServer::s_pCallbackInstance;

SessionServer::SessionServer(std::string identity, std::string sidecarHash) :
	_identity(identity),
	_sidecarHash(sidecarHash),
	_interface(SteamNetworkingSockets()),
	_listenSock(k_HSteamListenSocket_Invalid),
	registry(identity)
{
	_pollGroup = _interface->CreatePollGroup();
}

SessionServer::SessionServer(std::string identity, std::string sidecarHash, bool editionSelect, int roundCount, FixedPoint roundTime) :
	SessionServer(identity, sidecarHash)
{
	Lobby* defaultLobby = registry.Create(
		"main",
		editionSelect,
		roundCount,
		roundTime,
		MAX_SF4E_PROTOCOL_USERS,
		true
	);
	_defaultLobbyKey = defaultLobby->id.key;
}

SessionServer::~SessionServer()
{
	if (_listenSock != k_HSteamListenSocket_Invalid) {
		Close();
		_listenSock = k_HSteamListenSocket_Invalid;
	}
}

Lobby* SessionServer::GetDefaultLobby() {
	if (_defaultLobbyKey.empty()) {
		return nullptr;
	}
	return registry.FindByKey(_defaultLobbyKey);
}

void SessionServer::AddConnection(HSteamNetConnection newConn) {
	// XXX (adanducci): It is absolutely critical to note that
	// `SetConfigValue`'s interface to set callbacks is _not_ the
	// same as the one used by `CreateListenSocketIP`/`SteamNetworkingConfigValue_t`.
	//
	// Per the documentation for `SetConfigValue` and the header comment @
	// https://github.com/ValveSoftware/GameNetworkingSockets/blob/62b395172f157ca4f01eea3387d1131400f8d604/include/steam/isteamnetworkingutils.h#L296-L307 :
	//
	// NOTE: When setting pointers (e.g. callback functions), do not pass the function pointer
	// directly. Your argument should be a pointer to a function pointer.
	//
	// `CreateListenSocketIP`/`SteamNetworkingConfigValue_t` just takes the
	// function pointer directly. The failure mode if you pass the function
	// pointer directly is _extremely_ confusing- it just appears to be
	// a segfault in the GNS callback loop.
	void* callback = SteamNetConnectionStatusChangedCallback;
	SteamNetworkingUtils()->SetConfigValue(
		k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
		k_ESteamNetworkingConfig_Connection,
		newConn,
		k_ESteamNetworkingConfig_Ptr,
		&callback
	);
	_interface->SetConnectionPollGroup(newConn, _pollGroup);
}

int SessionServer::Listen(uint16 nPort) {
	SteamNetworkingIPAddr serverLocalAddr;
	serverLocalAddr.Clear();
	serverLocalAddr.m_port = nPort;
	SteamNetworkingConfigValue_t opt;
	opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)SteamNetConnectionStatusChangedCallback);
	_listenSock = _interface->CreateListenSocketIP(serverLocalAddr, 1, &opt);
	if (_listenSock == k_HSteamListenSocket_Invalid) {
		spdlog::error("Failed to listen on port {}", nPort);
		return -1;
	}
	spdlog::info("Server listening on port {}", nPort);
	return 0;
}

int SessionServer::Step()
{
	ISteamNetworkingMessage* pIncomingMsgs[SESSION_SERVER_MAX_MESSAGES_PER_POLL] = { 0 };
	int numMsgs = _interface->ReceiveMessagesOnPollGroup(_pollGroup, pIncomingMsgs, SESSION_SERVER_MAX_MESSAGES_PER_POLL);

	if (numMsgs < 0) {
		spdlog::error("Session server error checking for messages: {}", numMsgs);
		return -1;
	}

	for (int i = 0; i < numMsgs; i++) {
		ISteamNetworkingMessage* pIncomingMsg = pIncomingMsgs[i];
		if (!pIncomingMsg) {
			spdlog::error("Client: incoming message enumerated, but not data retrieved");
			return -1;
		}

		HSteamNetConnection conn = pIncomingMsg->m_conn;
		const char* start = (const char*)pIncomingMsg->m_pData;
		json msg;

		try {
			msg = json::parse(start, start + pIncomingMsg->m_cbSize);
		}
		catch (json::exception e) {
			spdlog::info("Server: got a non-json message");
			continue;
		}

		SessionProtocol::MessageType type;
		try {
			msg.at("type").get_to(type);
		}
		catch (json::exception e) {
			spdlog::info("Server: got a message without a type, or a type that was not a string");
			continue;
		}

		if (cidMap.find(conn) == cidMap.end()) {
			if (type == SessionProtocol::MT_SESSION_HELLO) {
				SessionProtocol::SessionHelloResp cidMsg;
				cidMsg.cid.host = _identity;
				cidMsg.cid.user = std::to_string(conn);
				cidMap[conn] = cidMsg.cid;
				json resp = cidMsg;
				Respond(conn, resp);
			}
			else {
				spdlog::warn("Server: got message type {} from a connection that hasn't said hello", (int)type);
			}
			continue;
		}

		SessionProtocol::ConnectionID cid = cidMap[conn];
		auto peerIter = peers.find(conn);

		if (type == SessionProtocol::MT_SESSION_JOINREQ) {
			SessionProtocol::SessionJoinRequest request;
			try {
				msg.get_to(request);
			}
			catch (json::exception e) {
				spdlog::info("Server: could not deserialize join request");
				RespondJoinReject(conn, SessionProtocol::JR_REQUEST_INVALID);
				continue;
			}
			spdlog::debug(
				"Server: join request from conn {} (registered: {}) for lobby \"{}\"",
				conn,
				peerIter != peers.end(),
				request.lobby.key
			);

			if (!request.handoff.empty()) {
				// Seat takeover: a game process presenting a one-shot
				// token takes over the seat held by the lobby app that
				// launched it. Tokens are consumed on first sight,
				// matched or not expired or not.
				if (!_sidecarHash.empty() && request.sidecarHash != _sidecarHash) {
					spdlog::info("Server: rejecting handoff for bad sidecar hash");
					RespondJoinReject(conn, SessionProtocol::JR_HASH_INVALID);
					continue;
				}

				Lobby* lobby = registry.FindByKey(request.lobby.key);
				int seat = -1;
				if (lobby) {
					uint64_t now = GetTickCount64();
					for (auto hIter = lobby->pendingHandoffs.begin(); hIter != lobby->pendingHandoffs.end(); hIter++) {
						if (hIter->token == request.handoff) {
							if (now - hIter->issuedAtMs <= HANDOFF_TTL_MS) {
								seat = hIter->seat;
							}
							lobby->pendingHandoffs.erase(hIter);
							break;
						}
					}
				}
				if (!lobby || seat < 0 || seat >= (int)lobby->members.size()) {
					spdlog::info("Server: rejecting invalid/expired handoff from conn {}", conn);
					RespondJoinReject(conn, SessionProtocol::JR_HANDOFF_INVALID);
					continue;
				}

				LobbyMember& member = lobby->members[seat];
				HSteamNetConnection oldConn = member.conn;

				char peerAddrStr[SteamNetworkingIPAddr::k_cchMaxString];
				SteamNetworkingIPAddr peerAddr = *(pIncomingMsg->m_identityPeer.GetIPAddr());
				if (peerAddr.IsLocalHost()) {
					peerAddrStr[0] = 0;
				}
				else {
					peerAddr.ToString(peerAddrStr, SteamNetworkingIPAddr::k_cchMaxString, false);
				}

				// The game connection registers under the seat's name.
				// Two peers deliberately share the name for the duration
				// of the match: the app keeps its registration for lounge
				// chat.
				Peer gamePeer;
				gamePeer.data = { cid, member.data.name, peerAddrStr, request.port, 0 };
				gamePeer.lobbyKey = lobby->id.key;
				peers[conn] = gamePeer;
				peerIter = peers.find(conn);

				// Swap the seat over with the game's own routing info-
				// GGPO needs the game process's address and port, not
				// the app's.
				member.conn = conn;
				member.data.connId = cid;
				member.data.ip = peerAddrStr;
				member.data.port = request.port;
				member.data.flags = 0;

				auto oldPeerIter = peers.find(oldConn);
				if (oldPeerIter != peers.end() && oldConn != conn) {
					oldPeerIter->second.lobbyKey = "";
					RespondNullLobbyUpdate(oldConn);
				}

				lobby->dirty = true;
				if (lobby->match.IsAllReady()) {
					lobby->pendingAllReadySends.push_back(conn);
				}
				spdlog::info(
					"Server: seat {} (\"{}\") of lobby {} handed off to conn {}",
					seat,
					member.data.name,
					lobby->id.key,
					conn
				);
				continue;
			}

			if (peerIter == peers.end()) {
				// First join request from this connection: register it.
				if (maxPeers != 0 && peers.size() >= maxPeers) {
					spdlog::info("Server: rejecting registration, server is full ({} peers)", peers.size());
					RespondJoinReject(conn, SessionProtocol::JR_SERVER_FULL);
					continue;
				}

				// The hash gate exists to keep mismatched game builds-
				// which would desync- out of matches. Connections that
				// don't run the game (the lobby app, browsers) carry no
				// sidecar and send an empty hash: admit them regardless
				// of the pin. Game builds always send their real hash,
				// and seats are still strictly gated at handoff.
				if (
					!_sidecarHash.empty() &&
					!request.sidecarHash.empty() &&
					request.sidecarHash != _sidecarHash
				) {
					spdlog::info("Server: rejecting registration for bad sidecar hash");
					RespondJoinReject(conn, SessionProtocol::JR_HASH_INVALID);
					continue;
				}

				if (request.username.empty()) {
					spdlog::info("Server: rejecting registration for empty username");
					RespondJoinReject(conn, SessionProtocol::JR_REQUEST_INVALID);
					continue;
				}

				bool nameTaken = false;
				for (auto iter = peers.begin(); iter != peers.end(); iter++) {
					if (iter->second.data.name == request.username) {
						nameTaken = true;
						break;
					}
				}
				if (nameTaken) {
					spdlog::info("Server: rejecting registration for taken name \"{}\"", request.username);
					RespondJoinReject(conn, SessionProtocol::JR_NAME_TAKEN);
					continue;
				}

				char peerAddrStr[SteamNetworkingIPAddr::k_cchMaxString];
				SteamNetworkingIPAddr peerAddr = *(pIncomingMsg->m_identityPeer.GetIPAddr());
				if (peerAddr.IsLocalHost()) {
					peerAddrStr[0] = 0;
				}
				else {
					peerAddr.ToString(peerAddrStr, SteamNetworkingIPAddr::k_cchMaxString, false);
				}

				Peer newPeer;
				newPeer.data = { cid, request.username, peerAddrStr, request.port, 0 };
				newPeer.lobbyKey = "";
				peers[conn] = newPeer;
				peerIter = peers.find(conn);
				spdlog::info("Server: registered \"{}\" as {}@{}", request.username, cid.user, cid.host);
			}

			// Resolve which lobby, if any, the sender should be seated in.
			// Note LobbyID's operator== treats every null-ish ID as equal.
			Lobby* target = nullptr;
			if (request.lobby == SessionProtocol::LobbyID::NULL_LOBBY_ID) {
				target = GetDefaultLobby();
			}
			else {
				target = registry.FindByKey(request.lobby.key);
				if (!target) {
					spdlog::info("Server: \"{}\" asked to join nonexistent lobby {}", peerIter->second.data.name, request.lobby.key);
					RespondJoinReject(conn, SessionProtocol::JR_NO_SUCH_LOBBY);
					continue;
				}
			}

			if (target) {
				if (!peerIter->second.lobbyKey.empty()) {
					RespondJoinReject(conn, SessionProtocol::JR_ALREADY_IN_LOBBY);
					continue;
				}
				if (target->IsFull()) {
					RespondJoinReject(conn, SessionProtocol::JR_LOBBY_FULL);
					continue;
				}
				SeatInLobby(conn, *target);
			}
			else {
				// No lobby to seat in- the sender idles in the lounge.
				// Acknowledge with a null-lobby update so the client knows
				// registration succeeded.
				RespondNullLobbyUpdate(conn);
			}
		}
		else if (peerIter == peers.end()) {
			spdlog::warn("Server: got message type {} from a connection that hasn't registered", (int)type);
		}
		else if (type == SessionProtocol::MT_LOBBY_CREATE) {
			SessionProtocol::LobbyCreate request;
			SessionProtocol::LobbyCreateResp resp;
			try {
				msg.get_to(request);
			}
			catch (json::exception e) {
				spdlog::info("Server: could not deserialize lobby create request");
				resp.result = SessionProtocol::JR_REQUEST_INVALID;
				json respMsg = resp;
				Respond(conn, respMsg);
				continue;
			}

			if (!peerIter->second.lobbyKey.empty()) {
				resp.result = SessionProtocol::JR_ALREADY_IN_LOBBY;
				json respMsg = resp;
				Respond(conn, respMsg);
				continue;
			}

			std::string displayName = request.name;
			if (displayName.empty()) {
				displayName = peerIter->second.data.name + "'s lobby";
			}

			// 1v1 scope: created lobbies hold exactly the two players.
			Lobby* lobby = registry.Create(
				displayName,
				request.editionSelect,
				request.roundCount,
				request.roundTime,
				2,
				false
			);
			SeatInLobby(conn, *lobby);
			spdlog::info("Server: \"{}\" created lobby {} (\"{}\")", peerIter->second.data.name, lobby->id.key, displayName);

			resp.result = SessionProtocol::JOIN_OK;
			resp.lobby = lobby->id;
			json respMsg = resp;
			Respond(conn, respMsg);
		}
		else if (type == SessionProtocol::MT_LOBBY_LIST) {
			SessionProtocol::LobbyListResp resp;
			for (auto iter = registry.lobbies.begin(); iter != registry.lobbies.end(); iter++) {
				Lobby& lobby = iter->second;
				SessionProtocol::LobbyListEntry entry;
				entry.id = lobby.id;
				entry.name = lobby.displayName;
				entry.playerCount = (int32_t)lobby.members.size();
				entry.capacity = (int32_t)lobby.capacity;
				for (auto memberIter = lobby.members.begin(); memberIter != lobby.members.end(); memberIter++) {
					entry.players.push_back(memberIter->data.name);
				}
				resp.lobbies.push_back(entry);
			}
			json respMsg = resp;
			Respond(conn, respMsg);
		}
		else if (type == SessionProtocol::MT_LOBBY_LEAVE) {
			if (peerIter->second.lobbyKey.empty()) {
				spdlog::info("Server: \"{}\" asked to leave but is not in a lobby", peerIter->second.data.name);
				continue;
			}
			RemoveFromLobby(conn);
			RespondNullLobbyUpdate(conn);
		}
		else if (type == SessionProtocol::MT_CHAT_SEND) {
			SessionProtocol::ChatSend request;
			try {
				msg.get_to(request);
			}
			catch (json::exception e) {
				spdlog::info("Server: could not deserialize chat message");
				continue;
			}

			Peer& peer = peerIter->second;

			uint64_t now = GetTickCount64();
			uint64_t oldest = peer.chatStamps[peer.chatStampIdx];
			if (oldest != 0 && (now - oldest) < CHAT_RATE_WINDOW_MS) {
				spdlog::debug("Server: rate limiting chat from \"{}\"", peer.data.name);
				continue;
			}
			peer.chatStamps[peer.chatStampIdx] = now;
			peer.chatStampIdx = (peer.chatStampIdx + 1) % CHAT_RATE_BURST;

			// Strip control characters and truncate before rebroadcast.
			std::string text;
			for (size_t ci = 0; ci < request.text.size() && text.size() < SessionProtocol::CHAT_TEXT_MAX; ci++) {
				unsigned char c = (unsigned char)request.text[ci];
				if (c >= 0x20) {
					text.push_back((char)c);
				}
			}
			if (text.empty()) {
				continue;
			}

			SessionProtocol::ChatEvent event;
			event.channel = request.channel;
			event.from = peer.data.name;
			event.text = text;
			event.ts = (int64_t)time(nullptr);
			json eventMsg = event;

			if (request.channel == SessionProtocol::CHAT_CHANNEL_LOUNGE) {
				BroadcastToPeers(eventMsg);
			}
			else if (!peer.lobbyKey.empty() && request.channel == peer.lobbyKey) {
				Lobby* lobby = registry.FindByKey(peer.lobbyKey);
				if (lobby) {
					BroadcastToLobby(*lobby, eventMsg);
				}
			}
			else {
				spdlog::debug("Server: dropping chat from \"{}\" to channel \"{}\" they're not in", peer.data.name, request.channel);
			}
		}
		else if (type == SessionProtocol::MT_FORWARD) {
			SessionProtocol::ForwardMessage fwdMsg;
			try {
				msg.get_to(fwdMsg);
			}
			catch (json::exception e) {
				spdlog::debug("Server: could not deserialize forwarding message");
				continue;
			}

			// If this is a connection ID managed by this server, we can apply
			// additional security- messages with this source address should
			// only be coming from the connection that the address is assigned
			// to.
			if (fwdMsg.src.host == _identity) {
				if (fwdMsg.src.user != std::to_string(conn)) {
					spdlog::debug("Server: dropping fraudulent packet; {} masqueraded as {}", conn, fwdMsg.src.user);
					continue;
				}
			}

			if (fwdMsg.dest.host != _identity) {
				spdlog::info("Server: cannot forward to nonlocal identity {}@{}, clustering not yet implemented", fwdMsg.dest.user, fwdMsg.dest.host);
				continue;
			}

			// Forwarding is scoped to members of the sender's lobby.
			Lobby* lobby = registry.FindByKey(peerIter->second.lobbyKey);
			if (!lobby) {
				spdlog::debug("Server: dropping forward from \"{}\": not in a lobby", peerIter->second.data.name);
				continue;
			}
			LobbyMember* dest = nullptr;
			for (auto memberIter = lobby->members.begin(); memberIter != lobby->members.end(); memberIter++) {
				if (memberIter->data.connId == fwdMsg.dest) {
					dest = &*memberIter;
					break;
				}
			}
			if (dest) {
				Respond(dest->conn, msg);
			}
			else {
				spdlog::debug("Server: Could not forward to {}@{}: not in sender's lobby", fwdMsg.dest.user, fwdMsg.dest.host);
			}
		}
		else if (type == SessionProtocol::MT_PREBATTLE_SETCHARA) {
			Lobby* lobby = registry.FindByKey(peerIter->second.lobbyKey);
			if (!lobby) {
				spdlog::info("Server: sender {} tried to set chara, but is not in a lobby", conn);
				continue;
			}
			int side = lobby->MemberIndex(conn);
			if (side < 0 || side > 1) {
				spdlog::info("Server: sender {} tried to set chara, but is not playing", conn);
				continue;
			}

			SessionProtocol::PreBattleSetChara request;
			try {
				msg.get_to(request);
			}
			catch (json::exception e) {
				spdlog::info("Server: could not deserialize SetConditionsRequest");
				continue;
			}
			lobby->match.chara[side] = request.chara;
			lobby->dirty = true;
		}
		else if (type == SessionProtocol::MT_PREBATTLE_SETENV) {
			Lobby* lobby = registry.FindByKey(peerIter->second.lobbyKey);
			if (!lobby || lobby->MemberIndex(conn) != 0) {
				spdlog::info("Server: sender {} tried to set env, but is not P1", conn);
				continue;
			}
			SessionProtocol::PreBattleSetEnv request;
			try {
				msg.get_to(request);
			}
			catch (json::exception e) {
				spdlog::info("Server: could not deserialize SetConditionsRequest");
				continue;
			}

			lobby->match.rngSeed = request.rngSeed;
			lobby->dirty = true;
		}
		else if (type == SessionProtocol::MT_PREBATTLE_SETSTAGE) {
			Lobby* lobby = registry.FindByKey(peerIter->second.lobbyKey);
			if (!lobby || lobby->MemberIndex(conn) != 0) {
				spdlog::info("Server: sender {} tried to set stage, but is not P1", conn);
				continue;
			}
			SessionProtocol::PreBattleSetStage request;
			try {
				msg.get_to(request);
			}
			catch (json::exception e) {
				spdlog::info("Server: could not deserialize SetConditionsRequest");
				continue;
			}

			lobby->match.stageID = request.stageID;
			lobby->dirty = true;
		}
		else if (type == SessionProtocol::MT_BATTLE_LOADED) {
			Lobby* lobby = registry.FindByKey(peerIter->second.lobbyKey);
			if (!lobby) {
				spdlog::info("Server: sender {} sent battle loaded, but is not in a lobby", conn);
				continue;
			}
			LobbyMember* member = lobby->FindMember(conn);
			member->data.flags |= SessionProtocol::MF_BATTLE_LOADED;

			bool allLoaded = true;
			for (auto memberIter = lobby->members.begin(); memberIter != lobby->members.end(); memberIter++) {
				allLoaded = allLoaded && (memberIter->data.flags & SessionProtocol::MF_BATTLE_LOADED);
			}
			lobby->sendBattleSynced = allLoaded;
			lobby->dirty = true;
		}
		else if (type == SessionProtocol::MT_LOBBY_READY) {
			Lobby* lobby = registry.FindByKey(peerIter->second.lobbyKey);
			if (!lobby) {
				spdlog::info("Server: sender {} tried to ready, but is not in a lobby", conn);
				continue;
			}
			int side = lobby->MemberIndex(conn);
			if (side < 0 || side > 1) {
				spdlog::info("Server: sender {} tried to ready, but is not playing", conn);
				continue;
			}

			SessionProtocol::LobbyReady request;
			try {
				msg.get_to(request);
			}
			catch (json::exception e) {
				spdlog::info("Server: could not deserialize ReportResultsRequest");
				continue;
			}
			lobby->match.readyMessageNum[side] = pIncomingMsg->GetMessageNumber();
			lobby->sendAllReady = lobby->sendAllReady || lobby->match.IsAllReady();
			lobby->dirty = true;
		}
		else if (type == SessionProtocol::MT_BATTLE_ENDED) {
			Lobby* lobby = registry.FindByKey(peerIter->second.lobbyKey);
			if (!lobby) {
				spdlog::info("Server: sender {} sent battle ended, but is not in a lobby", conn);
				continue;
			}

			// Reset the ready/loaded cycle so the seated players can
			// ready up for a rematch. Character and stage picks stay as
			// convenient defaults; results reporting is optional and
			// only used for queue rotation. Both games send this, and
			// the reset is idempotent.
			lobby->match.readyMessageNum[0] = -1;
			lobby->match.readyMessageNum[1] = -1;
			for (auto memberIter = lobby->members.begin(); memberIter != lobby->members.end(); memberIter++) {
				memberIter->data.flags &= (~SessionProtocol::MF_BATTLE_LOADED);
			}
			lobby->ClearHandoffs();
			lobby->dirty = true;
		}
		else if (type == SessionProtocol::MT_LOBBY_REPORTRESULTS) {
			Lobby* lobby = registry.FindByKey(peerIter->second.lobbyKey);
			if (!lobby) {
				spdlog::info("Server: sender {} reported results, but is not in a lobby", conn);
				continue;
			}
			SessionProtocol::LobbyReportResults request;
			try {
				msg.get_to(request);
			}
			catch (json::exception e) {
				spdlog::info("Server: could not deserialize ReportResultsRequest");
				continue;
			}

			HandleResults(*lobby, request.loserSide);
			lobby->dirty = true;
		}
		else if (type == SessionProtocol::MT_BATTLE_SNAPSHOT) {
			// Forward the snapshot to every other member of the sender's
			// lobby.
			Lobby* lobby = registry.FindByKey(peerIter->second.lobbyKey);
			if (!lobby) {
				continue;
			}
			for (auto memberIter = lobby->members.begin(); memberIter != lobby->members.end(); memberIter++) {
				if (memberIter->conn != conn) {
					_interface->SendMessageToConnection(
						memberIter->conn, (const char*)pIncomingMsg->m_pData, pIncomingMsg->m_cbSize,
						k_nSteamNetworkingSend_Reliable, nullptr
					);
				}
			}
		}
		else {
			spdlog::warn("Server: got unrecognized message type: {}", (int)type);
		}
	}

	for (int i = 0; i < numMsgs; i++) {
		ISteamNetworkingMessage* pIncomingMsg = pIncomingMsgs[i];
		if (pIncomingMsg) {
			pIncomingMsg->Release();
		}
	}

	for (auto iter = registry.lobbies.begin(); iter != registry.lobbies.end(); iter++) {
		Lobby& lobby = iter->second;
		if (lobby.dirty) {
			SessionProtocol::SessionDataUpdate updateMsg;
			updateMsg.lobbyData = lobby.ToLobbyData();
			updateMsg.matchData = lobby.match;
			json update = updateMsg;
			BroadcastToLobby(lobby, update);
			lobby.dirty = false;
		}

		if (lobby.sendAllReady) {
			json allReady = SessionProtocol::LobbyAllReady();
			BroadcastToLobby(lobby, allReady);
			lobby.sendAllReady = false;

			// Issue each seat a one-shot handoff token so the apps can
			// hand their seats to the game processes they launch.
			lobby.ClearHandoffs();
			for (int seat = 0; seat < 2 && seat < (int)lobby.members.size(); seat++) {
				Lobby::PendingHandoff pending;
				pending.token = GenerateHandoffToken();
				pending.seat = seat;
				pending.issuedAtMs = GetTickCount64();
				lobby.pendingHandoffs.push_back(pending);

				SessionProtocol::MatchHandoff handoffMsg;
				handoffMsg.token = pending.token;
				handoffMsg.lobby = lobby.id;
				json m = handoffMsg;
				Respond(lobby.members[seat].conn, m);
			}
		}

		if (!lobby.pendingAllReadySends.empty()) {
			for (auto connIter = lobby.pendingAllReadySends.begin(); connIter != lobby.pendingAllReadySends.end(); connIter++) {
				json allReady = SessionProtocol::LobbyAllReady();
				Respond(*connIter, allReady);
			}
			lobby.pendingAllReadySends.clear();
		}

		if (lobby.sendBattleSynced) {
			json synced = SessionProtocol::BattleSynced();
			BroadcastToLobby(lobby, synced);
			lobby.sendBattleSynced = false;
		}
	}

	return 0;
}

void SessionServer::Respond(HSteamNetConnection client, nlohmann::json& msg) {
	std::string buf = msg.dump();
	_interface->SendMessageToConnection(
		client, buf.c_str(), (uint32)buf.length(),
		k_nSteamNetworkingSend_Reliable, nullptr
	);
}

void SessionServer::RespondNullLobbyUpdate(HSteamNetConnection client) {
	SessionProtocol::SessionDataUpdate ack;
	ack.lobbyData = SessionProtocol::LobbyData::NULL_LOBBY;
	json msg = ack;
	Respond(client, msg);
}

void SessionServer::RespondJoinReject(HSteamNetConnection client, SessionProtocol::JoinResult result) {
	SessionProtocol::SessionJoinReject reject;
	reject.result = result;
	json msg = reject;
	Respond(client, msg);
}

void SessionServer::BroadcastToLobby(Lobby& lobby, nlohmann::json& msg) {
	// XXX (adanducci) replace SendMessageToConnection with SendMessages for
	// peformance gains, but ensuring low-copy with it is annoyingly difficult.
	std::string buf = msg.dump();
	for (auto iter = lobby.members.begin(); iter != lobby.members.end(); iter++) {
		_interface->SendMessageToConnection(
			iter->conn, buf.c_str(), (uint32)buf.length(),
			k_nSteamNetworkingSend_Reliable, nullptr
		);
	}
}

void SessionServer::BroadcastToPeers(nlohmann::json& msg) {
	std::string buf = msg.dump();
	for (auto iter = peers.begin(); iter != peers.end(); iter++) {
		_interface->SendMessageToConnection(
			iter->first, buf.c_str(), (uint32)buf.length(),
			k_nSteamNetworkingSend_Reliable, nullptr
		);
	}
}

void SessionServer::SeatInLobby(HSteamNetConnection conn, Lobby& lobby) {
	Peer& peer = peers[conn];
	LobbyMember member;
	member.data = peer.data;
	member.conn = conn;
	lobby.members.push_back(member);
	peer.lobbyKey = lobby.id.key;
	lobby.dirty = true;
}

void SessionServer::RemoveFromLobby(HSteamNetConnection conn) {
	auto peerIter = peers.find(conn);
	if (peerIter == peers.end() || peerIter->second.lobbyKey.empty()) {
		return;
	}

	std::string key = peerIter->second.lobbyKey;
	peerIter->second.lobbyKey = "";

	Lobby* lobby = registry.FindByKey(key);
	if (!lobby) {
		return;
	}

	for (auto memberIter = lobby->members.begin(); memberIter != lobby->members.end(); memberIter++) {
		if (memberIter->conn == conn) {
			lobby->members.erase(memberIter);
			break;
		}
	}

	if (lobby->members.empty() && !lobby->persistent) {
		registry.Remove(key);
		return;
	}

	// A member leaving invalidates any half-agreed match setup, along
	// with any handoff tokens issued for it.
	lobby->match.Clear();
	lobby->ClearHandoffs();
	lobby->dirty = true;
}

int SessionServer::Close()
{
	spdlog::info("Closing connections...");
	// Close all connections.  We use "linger mode" to ask SteamNetworkingSockets
	// to flush this out and close gracefully.
	for (auto iter = cidMap.begin(); iter != cidMap.end(); iter++) {
		_interface->SetConnectionPollGroup(iter->first, k_HSteamNetPollGroup_Invalid);
		_interface->CloseConnection(iter->first, 0, "Server Shutdown", true);
	}
	cidMap.clear();
	peers.clear();
	registry.lobbies.clear();
	_interface->DestroyPollGroup(_pollGroup);
	_pollGroup = k_HSteamNetPollGroup_Invalid;
	if (_listenSock != k_HSteamListenSocket_Invalid) {
		_interface->CloseListenSocket(_listenSock);
		_listenSock = k_HSteamListenSocket_Invalid;
	}
	return 0;
}

void SessionServer::PrepareForCallbacks()
{
	s_pCallbackInstance = this;
}

void SessionServer::ResetBattleSync()
{
	for (auto iter = registry.lobbies.begin(); iter != registry.lobbies.end(); iter++) {
		Lobby& lobby = iter->second;
		for (auto memberIter = lobby.members.begin(); memberIter != lobby.members.end(); memberIter++) {
			memberIter->data.flags &= (~SessionProtocol::MF_BATTLE_LOADED);
		}
	}
}

void SessionServer::OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
	switch (pInfo->m_info.m_eState)
	{
	case k_ESteamNetworkingConnectionState_ClosedByPeer:
	case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
	{
		// Ignore if they were not previously connected.  (If they disconnected
		// before we accepted the connection.)
		if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connected) {
			// Select appropriate log messages
			const char* pszDebugLogAction;
			if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
			{
				pszDebugLogAction = "problem detected locally";
			}
			else
			{
				// Note that here we could check the reason code to see if
				// it was a "usual" connection or an "unusual" one.
				pszDebugLogAction = "closed by peer";
			}

			spdlog::info("Connection {} {}, reason {}: {}",
				pInfo->m_info.m_szConnectionDescription,
				pszDebugLogAction,
				pInfo->m_info.m_eEndReason,
				pInfo->m_info.m_szEndDebug
			);

			RemoveFromLobby(pInfo->m_hConn);
			peers.erase(pInfo->m_hConn);
			cidMap.erase(pInfo->m_hConn);

			// Clean up the connection.  This is important!
			// The connection is "closed" in the network sense, but
			// it has not been destroyed.  We must close it on our end, too
			// to finish up.  The reason information do not matter in this case,
			// and we cannot linger because it's already closed on the other end,
			// so we just pass 0's.
			_interface->SetConnectionPollGroup(pInfo->m_hConn, k_HSteamNetPollGroup_Invalid);
			_interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
		}

		break;
	}

	case k_ESteamNetworkingConnectionState_Connecting:
	{
		// Try to accept the connection.
		if (_interface->AcceptConnection(pInfo->m_hConn) != k_EResultOK)
		{
			// This could fail.  If the remote host tried to connect, but then
			// disconnected, the connection may already be half closed.  Just
			// destroy whatever we have on our side.
			_interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
			spdlog::error("Can't accept connection.  (It was already closed?)");
			break;
		}

		_interface->SetConnectionPollGroup(pInfo->m_hConn, _pollGroup);
	}

	default:
		break;
	}
}

void SessionServer::SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
	s_pCallbackInstance->OnSteamNetConnectionStatusChanged(pInfo);
}

void SessionServer::HandleResults(Lobby& lobby, int loserIndex) {
	if (loserIndex < 0 || loserIndex >= (int)lobby.members.size()) {
		spdlog::info("Server: ignoring results report with out-of-range loser {}", loserIndex);
		return;
	}
	auto loser = lobby.members.begin() + loserIndex;
	lobby.members.push_back(*loser);
	lobby.members.erase(loser);
	lobby.match.Clear();
	lobby.ClearHandoffs();
}
