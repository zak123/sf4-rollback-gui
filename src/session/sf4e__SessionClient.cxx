#include <string>
#include <utility>

#include <windows.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <ggponet.h>

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"

#include "../sf4e/sf4e__Game__Battle__System.hxx"

#include "sf4e__SessionClient.hxx"
#include "sf4e__SessionProtocol.hxx"

using nlohmann::json;

namespace SessionProtocol = sf4e::SessionProtocol;
using rSystem = Dimps::Game::Battle::System;
using fSystem = sf4e::Game::Battle::System;
using sf4e::SessionClient;
using sf4e::SessionProtocol::LobbyReady;

const int sf4e::SESSION_CLIENT_MAX_MESSAGES_PER_POLL = 20;
SessionClient* SessionClient::s_pCallbackInstance;

SessionClient::SessionClient(
	const Callbacks& callbacks,
	std::string sidecarHash,
	uint16_t ggpoPort,
	std::string& name
):
	_callbacks(callbacks),
	_sidecarHash(sidecarHash),
	_name(name),
	_ggpoPort(ggpoPort),
	_interface(SteamNetworkingSockets()),
	_conn(k_HSteamNetConnection_Invalid),
	_connected(false)
{
	_serverAddr.Clear();
}

int SessionClient::Connect(HSteamNetConnection newConn) {
	_snapshotsEnabled = false;
	_serverAddr.SetIPv6LocalHost();
	_conn = newConn;
	_connected = true;
	_interface->SetConnectionUserData(newConn, (int64)this);

	// XXX (adanducci): It is absolutely critical to note that
	// `SetConfigValue`'s interface to set callbacks is _not_ the
	// same as the one used by `ConnectByIPAddress`/`SteamNetworkingConfigValue_t`.
	// 
	// Per the documentation for `SetConfigValue` and the header comment @
	// https://github.com/ValveSoftware/GameNetworkingSockets/blob/62b395172f157ca4f01eea3387d1131400f8d604/include/steam/isteamnetworkingutils.h#L296-L307 :
	//
	// NOTE: When setting pointers (e.g. callback functions), do not pass the function pointer
	// directly. Your argument should be a pointer to a function pointer.
	//
	// `ConnectByIPAddress`/`SteamNetworkingConfigValue_t` just takes the
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

	return 0;
}

int SessionClient::Connect(const SteamNetworkingIPAddr& serverAddr) {
	char szAddr[SteamNetworkingIPAddr::k_cchMaxString];
	SteamNetworkingConfigValue_t opts[2];
	_serverAddr = serverAddr;
	_serverAddr.ToString(szAddr, sizeof(szAddr), true);
	spdlog::info("Connecting to session server at {}", szAddr);

	opts[0].SetInt64(
		k_ESteamNetworkingConfig_ConnectionUserData,
		(int64)this
	);
	opts[1].SetPtr(
		k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
		(void*)SteamNetConnectionStatusChangedCallback
	);
	_snapshotsEnabled = true;
	_conn = _interface->ConnectByIPAddress(_serverAddr, 2, opts);
	if (_conn == k_HSteamNetConnection_Invalid) {
		spdlog::error("Client failed to create connection");
	}
	return 0;
}

SessionClient::~SessionClient()
{
	if (_conn != k_HSteamNetConnection_Invalid) {
		_interface->CloseConnection(_conn, k_ESteamNetConnectionEnd_App_Generic, nullptr, true);
		_conn = k_HSteamNetConnection_Invalid;
		_connected = false;
	}
	_interface = nullptr;
}

void SessionClient::PrepareForCallbacks()
{
	s_pCallbackInstance = this;
}

int SessionClient::Step()
{
	if (_interface == nullptr || _conn == k_HSteamNetConnection_Invalid) {
		return -1;
	}

	if (!_connected) {
		// Not yet connected- not an error state, but nothing to do.
		return 0;
	}

	ISteamNetworkingMessage* pIncomingMsgs[SESSION_CLIENT_MAX_MESSAGES_PER_POLL] = { 0 };
	int numMsgs = _interface->ReceiveMessagesOnConnection(_conn, pIncomingMsgs, SESSION_CLIENT_MAX_MESSAGES_PER_POLL);

	if (numMsgs < 0) {
		spdlog::error("Session client error checking for messages: {}", numMsgs);
		return -1;
	}

	for (int i = 0; i < numMsgs; i++) {
		ISteamNetworkingMessage* pIncomingMsg = pIncomingMsgs[i];
		if (!pIncomingMsg) {
			spdlog::error("Client: incoming message enumerated, but not data retrieved");
			return -1;
		}

		const char* start = (const char*)pIncomingMsg->m_pData;
		json msg = json::parse(start, start + pIncomingMsg->m_cbSize);
		SteamNetworkingIPAddr peerAddr = *(pIncomingMsg->m_identityPeer.GetIPAddr());
		pIncomingMsg->Release();

		SessionProtocol::MessageType type;
		try {
			msg.at("type").get_to(type);
		}
		catch (json::exception e) {
			spdlog::info("Client: got a message without a type, or a type that was not a string");
			continue;
		}

		if (type == SessionProtocol::MT_SESSION_JOINREJ) {
			SessionProtocol::SessionJoinReject reject;
			try {
				msg.get_to(reject);
			}
			catch (json::exception e) {
				spdlog::info("Client: couldn't deserialize join rejection?");
				continue;
			}

			spdlog::info("Join rejected, reason: {}", (int)reject.result);
			ErrorType errType = ErrorType::SCE_UNKNOWN;
			switch (reject.result) {
			case SessionProtocol::JoinResult::JR_HASH_INVALID:
				errType = ErrorType::SCE_JOIN_REJECTED_HASH_INVALID;
				break;
			case SessionProtocol::JoinResult::JR_LOBBY_FULL:
				errType = ErrorType::SCE_JOIN_REJECTED_LOBBY_FULL;
				break;
			case SessionProtocol::JoinResult::JR_NAME_TAKEN:
				errType = ErrorType::SCE_JOIN_REJECTED_NAME_TAKEN;
				break;
			case SessionProtocol::JoinResult::JR_REQUEST_INVALID:
				errType = ErrorType::SCE_JOIN_REJECTED_REQUEST_INVALID;
				break;
			default:
				break;
			}
			_callbacks.OnError(errType, this, _callbacks);
			_interface->CloseConnection(_conn, 0, nullptr, false);
			_conn = k_HSteamNetConnection_Invalid;
			return -1;
		}
		else if (type == SessionProtocol::MT_SESSION_DATAUPDATE) {
			SessionProtocol::SessionDataUpdate update;
			try {
				msg.get_to(update);
			}
			catch (json::exception e) {
				spdlog::info("Client: could not deserialize response");
				continue;
			}
			_lobbyData = update.lobbyData;
			_matchData = update.matchData;

			if (_outstandingReadyRequestNumber > -1) {
				for (int i = 0; i < _lobbyData.size() && i < 2; i++) {
					if (_lobbyData[i].name == _name) {
						if (_matchData.readyMessageNum[i] == _outstandingReadyRequestNumber) {
							// This contains the ready data, so there's no longer an outstanding request.
							_outstandingReadyRequestNumber = -1;
						}
						break;
					}
				}
			}

			if (_matchData.readyMessageNum[0] > -1 && _matchData.readyMessageNum[1] > -1) {
				_callbacks.OnReady(this, _callbacks);
			}
		}
		else if (type == SessionProtocol::MT_BATTLE_SNAPSHOT) {
			SessionProtocol::BattleSnapshot m;
			try {
				msg.get_to(m);
			}
			catch (json::exception e) {
				spdlog::info("Client: could not deserialize incoming checksum msg");
				continue;
			}

			auto localSnapshotIter = fSystem::snapshotMap.find(m.snapshot.frameIdx);
			if (localSnapshotIter != fSystem::snapshotMap.end()) {
				// This client is ahead and already has a snapshot for this frame.
				// Compare it.
				SessionProtocol::StateSnapshot& localSnapshot = localSnapshotIter->second.first;
				if (memcmp(&m.snapshot, &localSnapshot, sizeof(SessionProtocol::StateSnapshot)) != 0) {
					spdlog::error("Client: Desync detected!");
					MessageBoxA(NULL, "Desync detected!", NULL, MB_OK);
					*rSystem::GetReadyState(rSystem::staticMethods.GetSingleton()) = rSystem::RS_ISLEAVING;
				}
				localSnapshotIter->second.second.confirmed = true;
				if (localSnapshotIter->second.second.confirmed && localSnapshotIter->second.second.sent) {
					fSystem::snapshotMap.erase(localSnapshotIter);
				}
			}
			else {
				// Opponent's ahead- can't compare yet.
				pendingRemoteSnapshots.emplace(m.snapshot.frameIdx, m.snapshot);
			}
		}
		else {
			spdlog::warn("Client: got unrecognized message type: {}", (int)type);
		}
	}

	// Send all our outstanding local snapshots and compare any to pending
	// snapshots.
	if (_snapshotsEnabled) {
		int mostRecentPredictiveFrame = (
			rSystem::GetNumFramesSimulated_FixedPoint(rSystem::staticMethods.GetSingleton())->integral
		);
		auto localSnapshotIter = fSystem::snapshotMap.begin();
		while (localSnapshotIter != fSystem::snapshotMap.end()) {
			if (mostRecentPredictiveFrame - localSnapshotIter->first < 60) {
				// Snapshot not yet old enough.
				localSnapshotIter++;
				continue;
			}

			if (!localSnapshotIter->second.second.sent) {
				// Snapshot not yet sent. Send it.
				localSnapshotIter->second.second.sent = true;
				SessionProtocol::BattleSnapshot m;
				m.snapshot = localSnapshotIter->second.first;
				json msg = m;
				if (Send(msg, nullptr) != k_EResultOK) {
					spdlog::error("Client: Could not send snapshot update");
				}
			}

			if (!localSnapshotIter->second.second.confirmed) {
				auto remoteSnapshotIter = pendingRemoteSnapshots.find(localSnapshotIter->first);
				if (remoteSnapshotIter != pendingRemoteSnapshots.end()) {
					// Caught up to the opponent- compare to a snapshot already sent by the opponent.
					SessionProtocol::StateSnapshot& localSnapshot = localSnapshotIter->second.first;
					if (memcmp(&remoteSnapshotIter->second, &localSnapshot, sizeof(SessionProtocol::StateSnapshot)) != 0) {
						spdlog::error("Client: Desync detected!");
						MessageBoxA(NULL, "Desync detected!", NULL, MB_OK);
						*rSystem::GetReadyState(rSystem::staticMethods.GetSingleton()) = rSystem::RS_ISLEAVING;
					}
					localSnapshotIter->second.second.confirmed = true;
				}
			}

			if (localSnapshotIter->second.second.confirmed && localSnapshotIter->second.second.sent) {
				localSnapshotIter = fSystem::snapshotMap.erase(localSnapshotIter);
			}
			else {
				localSnapshotIter++;
			}
		}
	}

	return 0;
}

void SessionClient::OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo)
{

	switch (pInfo->m_info.m_eState)
	{
	case k_ESteamNetworkingConnectionState_ClosedByPeer:
	case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
	{
		// Print an appropriate message
		if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting)
		{
			spdlog::error("Client could not connect: {}", pInfo->m_info.m_szEndDebug);
			MessageBoxA(NULL, "Client: could not connect- maybe wrong IP or no forwarding", NULL, MB_OK);
		}
		else if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
		{
			spdlog::error("Client lost contact with host: {}", pInfo->m_info.m_szEndDebug);
			MessageBoxA(NULL, "Client: Problem detected locally- lost contact with host", NULL, MB_OK);
		}

		// Clean up the connection.  This is important!
		// The connection is "closed" in the network sense, but
		// it has not been destroyed.  We must close it on our end, too
		// to finish up.  The reason information do not matter in this case,
		// and we cannot linger because it's already closed on the other end,
		// so we just pass 0's.
		_interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
		_conn = k_HSteamNetConnection_Invalid;
		_connected = false;
		break;
	}
	case k_ESteamNetworkingConnectionState_Connected:
	{
		spdlog::info("Client connected to server OK, attempting to join...");
		_connected = true;
		SessionProtocol::SessionJoinRequest request;
		request.sidecarHash = _sidecarHash;
		request.username = _name;
		request.port = _ggpoPort;
		json msg = request;
		if (Send(msg, nullptr) != k_EResultOK) {
			spdlog::warn("Client could send initial join request");
		}
		break;
	}
	default:
		break;
	}
}

EResult SessionClient::Send(nlohmann::json& msg, int64_t* outMessageNum) {
	std::string buf = msg.dump();
	return _interface->SendMessageToConnection(
		_conn, buf.c_str(), (uint32)buf.length(),
		k_nSteamNetworkingSend_Reliable, outMessageNum
	);
}

EResult SessionClient::Lobby_Ready()
{
	LobbyReady msg;
	json j = msg;
	EResult result = Send(j, &_outstandingReadyRequestNumber);
	if (result != k_EResultOK) {
		spdlog::warn("Client: could not send ready! Result: {}", (int)result);
	}
	return result;
}

EResult SessionClient::Lobby_ReportResults(int loserSide)
{
	SessionProtocol::LobbyReportResults r;
	r.loserSide = loserSide;
	json msg = r;
	EResult result = Send(msg, nullptr);
	if (result != k_EResultOK) {
		spdlog::warn("Client: could not report results! Result: {}", (int)result);
	}
	return result;
}

EResult SessionClient::PreBattle_SetEnv(uint32_t rngSeed)
{
	SessionProtocol::PreBattleSetEnv msg;
	msg.rngSeed = rngSeed;
	json j = msg;
	EResult result = Send(j, nullptr);
	if (result != k_EResultOK) {
		spdlog::warn("Client: could not set prebattle environment! Result: {}", (int)result);
	}
	return result;
}

EResult SessionClient::PreBattle_SetChara(const Dimps::GameEvents::VsMode::ConfirmedCharaConditions& chara)
{
	SessionProtocol::PreBattleSetChara msg;
	msg.chara = chara;
	json j = msg;
	EResult result = Send(j, nullptr);
	if (result != k_EResultOK) {
		spdlog::warn("Client: could not set prebattle character! Result: {}", (int)result);
	}
	return result;
}

EResult SessionClient::PreBattle_SetStage(int32_t stageID)
{
	SessionProtocol::PreBattleSetStage msg;
	msg.stageID = stageID;
	json j = msg;
	EResult result = Send(j, nullptr);
	if (result != k_EResultOK) {
		spdlog::warn("Client: could not set prebattle stage! Result: {}", (int)result);
	}
	return result;
}

void SessionClient::SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
	SessionClient* instance = (SessionClient *)SteamNetworkingSockets()->GetConnectionUserData(pInfo->m_hConn);
	instance->OnSteamNetConnectionStatusChanged(pInfo);
}
