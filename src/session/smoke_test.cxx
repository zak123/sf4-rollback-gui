// Headless exerciser for the session protocol. Hosts a dedicated-mode
// SessionServer in-process, then drives clients through registration,
// lobby browsing, creation, joining, and leaving over real loopback
// sockets, exiting nonzero on the first failed expectation. Run it after
// touching anything in src/session/.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>

#include "sf4e__LobbyRegistry.hxx"
#include <atomic>
#include <thread>

#include "sf4e__Portable.hxx"
#include "sf4e__Resolve.hxx"
#include "sf4e__SessionClient.hxx"
#include "sf4e__SessionProtocol.hxx"
#include "sf4e__SessionServer.hxx"

namespace SessionProtocol = sf4e::SessionProtocol;
using sf4e::SessionClient;
using sf4e::SessionServer;

static const uint16_t TEST_PORT = 23461;

struct TestClientCtx {
	std::string name;
	bool errored = false;
	SessionClient::ErrorType lastError = SessionClient::SCE_UNKNOWN;
	int createResult = -1;
	int listCount = -1;
	bool readyFired = false;
	bool syncedFired = false;
	SessionProtocol::MatchHandoff handoff;
	std::vector<SessionProtocol::LobbyListEntry> lastListing;
	std::vector<SessionProtocol::ChatEvent> chatLog;
	std::vector<SessionProtocol::PresenceEntry> presence;
	int lookingCount = -1;
	int presenceVersion = 0;
	std::string challengeFrom;
	bool gotChallengeResult = false;
	SessionProtocol::ChallengeResultMsg lastChallengeResult;
	std::string matchedOpponent;
	std::unique_ptr<SessionClient> c;
};

static void OnError(SessionClient::ErrorType errType, SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	TestClientCtx* ctx = (TestClientCtx*)callbacks.data;
	ctx->errored = true;
	ctx->lastError = errType;
	spdlog::info("[{}] error callback: {}", ctx->name, (int)errType);
}

static void OnReady(SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	TestClientCtx* ctx = (TestClientCtx*)callbacks.data;
	ctx->readyFired = true;
	spdlog::info("[{}] ready callback", ctx->name);
}

static void OnBattleSynced(SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	TestClientCtx* ctx = (TestClientCtx*)callbacks.data;
	ctx->syncedFired = true;
	spdlog::info("[{}] battle synced callback", ctx->name);
}

static void OnLobbyCreated(SessionProtocol::JoinResult result, SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	TestClientCtx* ctx = (TestClientCtx*)callbacks.data;
	ctx->createResult = (int)result;
	spdlog::info("[{}] lobby create result: {}", ctx->name, (int)result);
}

static void OnLobbyList(SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	TestClientCtx* ctx = (TestClientCtx*)callbacks.data;
	ctx->lastListing = c->_lobbyListing;
	ctx->listCount = (int)c->_lobbyListing.size();
	spdlog::info("[{}] lobby list: {} lobbies", ctx->name, ctx->listCount);
}

static void OnChat(const SessionProtocol::ChatEvent& event, SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	TestClientCtx* ctx = (TestClientCtx*)callbacks.data;
	ctx->chatLog.push_back(event);
	spdlog::info("[{}] chat [{}] {}: {}", ctx->name, event.channel, event.from, event.text);
}

static void OnMatchHandoff(const SessionProtocol::MatchHandoff& handoff, SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	TestClientCtx* ctx = (TestClientCtx*)callbacks.data;
	ctx->handoff = handoff;
	spdlog::info("[{}] match handoff for lobby {}", ctx->name, handoff.lobby.key);
}

static void OnPresence(SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	TestClientCtx* ctx = (TestClientCtx*)callbacks.data;
	ctx->presence = c->_presence;
	ctx->lookingCount = c->_lookingCount;
	ctx->presenceVersion++;
}

static void OnChallengeEvent(const SessionProtocol::ChallengeEvent& event, SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	TestClientCtx* ctx = (TestClientCtx*)callbacks.data;
	ctx->challengeFrom = event.from;
	spdlog::info("[{}] challenged by {}", ctx->name, event.from);
}

static void OnChallengeResult(const SessionProtocol::ChallengeResultMsg& result, SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	TestClientCtx* ctx = (TestClientCtx*)callbacks.data;
	ctx->lastChallengeResult = result;
	ctx->gotChallengeResult = true;
	spdlog::info("[{}] challenge result for {}: {}", ctx->name, result.target, (int)result.result);
}

static void OnMatchmakeEvent(const SessionProtocol::MatchmakeEvent& event, SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	TestClientCtx* ctx = (TestClientCtx*)callbacks.data;
	ctx->matchedOpponent = event.opponent;
	spdlog::info("[{}] matched with {}", ctx->name, event.opponent);
}

static TestClientCtx* MakeClient(
	const std::string& name,
	uint16_t ggpoPort,
	const SessionProtocol::LobbyID& autoJoinLobby = SessionProtocol::LobbyID{ "", "" },
	const std::string& autoJoinHandoff = std::string(),
	const std::string& sidecarHash = std::string("smokehash"),
	uint16_t serverPort = TEST_PORT
) {
	TestClientCtx* ctx = new TestClientCtx();
	ctx->name = name;

	SessionClient::Callbacks callbacks = {
		ctx,
		OnError,
		OnReady,
		OnBattleSynced,
		OnLobbyCreated,
		OnLobbyList,
		OnChat,
		OnMatchHandoff,
		OnPresence,
		OnChallengeEvent,
		OnChallengeResult,
		OnMatchmakeEvent,
	};
	std::string mutableName = name;
	std::string mutableHash = sidecarHash;
	ctx->c.reset(new SessionClient(callbacks, mutableHash, ggpoPort, mutableName));
	ctx->c->_autoJoinLobby = autoJoinLobby;
	ctx->c->_autoJoinHandoff = autoJoinHandoff;

	SteamNetworkingIPAddr addr;
	char szAddr[64];
	snprintf(szAddr, sizeof(szAddr), "localhost:%d", serverPort);
	sf4e::Net::ResolveHostPort(szAddr, addr);
	ctx->c->Connect(addr);

	// The IP-address Connect() enables battle snapshots, but snapshot
	// reconciliation calls into located game memory (Dimps::...::System)
	// that only exists inside the game process. Any client running
	// outside the game must keep snapshots off or Step() crashes on a
	// null function pointer.
	ctx->c->_snapshotsEnabled = false;
	return ctx;
}

static std::vector<TestClientCtx*> g_clients;

static bool PumpUntil(SessionServer& server, int timeoutMs, std::function<bool()> done) {
	int elapsed = 0;
	while (elapsed <= timeoutMs) {
		server.PrepareForCallbacks();
		SteamNetworkingSockets()->RunCallbacks();
		for (auto iter = g_clients.begin(); iter != g_clients.end(); iter++) {
			(*iter)->c->Step();
		}
		server.Step();
		if (done()) {
			return true;
		}
		sf4e::Portable::SleepMs(5);
		elapsed += 5;
	}
	return done();
}

#define CHECK(cond, desc) \
	do { \
		if (!(cond)) { \
			spdlog::error("FAIL: {}", desc); \
			return 1; \
		} \
		spdlog::info("ok: {}", desc); \
	} while (0)

int main(int argc, char** argv) {
	spdlog::set_level(spdlog::level::debug);

	SteamDatagramErrMsg errMsg;
	if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
		spdlog::error("GameNetworkingSockets_Init failed: {}", errMsg);
		return 1;
	}

	// Address resolution: literal IPs, resolvable hostnames, and the
	// ways both can be malformed.
	{
		SteamNetworkingIPAddr addr;
		CHECK(
			sf4e::Net::ResolveHostPort("127.0.0.1:23450", addr) && addr.m_port == 23450,
			"resolver passes literal ip:port through"
		);
		CHECK(
			sf4e::Net::ResolveHostPort("localhost:23450", addr) &&
			addr.m_port == 23450 && addr.GetIPv4() == 0x7f000001,
			"resolver resolves hostnames to their IPv4 address"
		);
		CHECK(!sf4e::Net::ResolveHostPort("localhost", addr), "resolver rejects a missing port");
		CHECK(!sf4e::Net::ResolveHostPort("definitely-not-a-real-host.invalid:23450", addr), "resolver rejects unresolvable hosts");
	}

	int ret = 1;
	{
		// Dedicated mode: no default lobby, accept-any-hash.
		SessionServer server(std::string("smokehost"), std::string(""));
		if (server.Listen(TEST_PORT) != 0) {
			spdlog::error("FAIL: could not listen on {}", TEST_PORT);
			return 1;
		}

		ret = [&server]() -> int {
			// NAT probe echo round trip: the client helper learns its
			// own endpoint through the server's echo port. Loopback has
			// no NAT, so the observed port must equal the bound port.
			{
				SteamNetworkingIPAddr probeServer;
				probeServer.Clear();
				probeServer.SetIPv4(0x7f000001, TEST_PORT);
				sf4e::Net::NatProbe probe;
				std::atomic<bool> probeDone(false);
				bool probeOk = false;
				std::thread prober([&]() {
					probeOk = sf4e::Net::Net_ProbeGgpoPort(probeServer, 24990, probe);
					probeDone = true;
				});
				PumpUntil(server, 3000, [&]() { return probeDone.load(); });
				prober.join();
				CHECK(
					probeOk && probe.ok && probe.publicPort == 24990,
					"the NAT probe learns its endpoint through the server echo"
				);
				CHECK(
					probe.symmetricKnown && !probe.symmetric && probe.publicPort2 == 24990,
					"the dual-destination probe sees a consistent mapping on loopback"
				);
				sf4e::Net::Net_ProbeClose(probe);
			}

			// Relay cross-forward: two fake game sockets register with
			// the relay (port TEST_PORT+3) under one token, then a
			// datagram from one must arrive at the other.
			{
				sf4e::Portable::Socket g0 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
				sf4e::Portable::Socket g1 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
				sockaddr_in any = { 0 };
				any.sin_family = AF_INET;
				any.sin_addr.s_addr = htonl(0x7f000001);
				bind(g0, (sockaddr*)&any, sizeof(any));
				bind(g1, (sockaddr*)&any, sizeof(any));
				sf4e::Portable::SetRecvTimeoutMs(g1, 500);

				sockaddr_in relayAddr = { 0 };
				relayAddr.sin_family = AF_INET;
				relayAddr.sin_addr.s_addr = htonl(0x7f000001);
				relayAddr.sin_port = htons(TEST_PORT + 3);
				const char* reg0 = "SF4ERELAY smoke:1 0";
				const char* reg1 = "SF4ERELAY smoke:1 1";
				sendto(g0, reg0, (int)strlen(reg0), 0, (sockaddr*)&relayAddr, sizeof(relayAddr));
				sendto(g1, reg1, (int)strlen(reg1), 0, (sockaddr*)&relayAddr, sizeof(relayAddr));
				PumpUntil(server, 1000, []() { return false; });
				const char* payload = "GGPO-ish payload";
				sendto(g0, payload, (int)strlen(payload), 0, (sockaddr*)&relayAddr, sizeof(relayAddr));
				PumpUntil(server, 1000, []() { return false; });
				char rbuf[64] = { 0 };
				int rgot = recv(g1, rbuf, sizeof(rbuf) - 1, 0);
				CHECK(
					rgot == (int)strlen(payload) && memcmp(rbuf, payload, rgot) == 0,
					"the relay cross-forwards a datagram between paired seats"
				);
				sf4e::Portable::CloseSocket(g0);
				sf4e::Portable::CloseSocket(g1);
			}

			TestClientCtx* alice = MakeClient("alice", 24001);
			g_clients.push_back(alice);
			TestClientCtx* bob = MakeClient("bob", 24002);
			g_clients.push_back(bob);

			// Both clients auto-send a null-lobby join request after their
			// hello; on a dedicated server that registers them into the
			// lounge without seating them anywhere.
			CHECK(
				PumpUntil(server, 5000, [&]() { return server.peers.size() == 2; }),
				"alice and bob register into the lounge"
			);
			CHECK(server.registry.lobbies.empty(), "no lobbies exist at startup");

			alice->c->Lobby_RequestList();
			CHECK(
				PumpUntil(server, 5000, [&]() { return alice->listCount == 0; }),
				"empty lobby list returns zero entries"
			);

			alice->c->Lobby_Create(std::string("smoke lobby"), true, 3, { 0, 99 });
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return alice->createResult == (int)SessionProtocol::JOIN_OK &&
						alice->c->_lobbyData.members.size() == 1;
				}),
				"alice creates a lobby and is seated in it"
			);
			CHECK(alice->c->_lobbyData.name == "smoke lobby", "lobby carries its display name");

			bob->c->Lobby_RequestList();
			CHECK(
				PumpUntil(server, 5000, [&]() { return bob->listCount == 1; }),
				"bob's listing shows one lobby"
			);
			CHECK(
				bob->lastListing[0].name == "smoke lobby" &&
				bob->lastListing[0].playerCount == 1 &&
				bob->lastListing[0].capacity == 2,
				"listing entry has correct name and occupancy"
			);

			bob->c->Lobby_Join(bob->lastListing[0].id);
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return alice->c->_lobbyData.members.size() == 2 &&
						bob->c->_lobbyData.members.size() == 2;
				}),
				"bob joins and both clients see two members"
			);

			// A third client with a duplicate name must be rejected at
			// registration.
			TestClientCtx* fakeAlice = MakeClient("alice", 24003);
			g_clients.push_back(fakeAlice);
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return fakeAlice->errored &&
						fakeAlice->lastError == SessionClient::SCE_JOIN_REJECTED_NAME_TAKEN;
				}),
				"duplicate name is rejected at registration"
			);

			// A third player can register, but the 1v1 lobby is full.
			TestClientCtx* carol = MakeClient("carol", 24004);
			g_clients.push_back(carol);
			CHECK(
				PumpUntil(server, 5000, [&]() { return server.peers.size() == 3; }),
				"carol registers into the lounge"
			);

			// Regression check: carol's registration used a null lobby ID.
			// On a dedicated server that must land her in the lounge, even
			// though a user-created lobby is coincidentally keyed "1"- it
			// must not be mistaken for a default lobby.
			PumpUntil(server, 300, []() { return false; });
			CHECK(
				!carol->errored && carol->c->_lobbyData.members.empty(),
				"null-lobby registration lands in the lounge despite user lobby keyed 1"
			);
			carol->c->Lobby_Join(bob->lastListing[0].id);
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return carol->errored &&
						carol->lastError == SessionClient::SCE_JOIN_REJECTED_LOBBY_FULL;
				}),
				"joining a full 1v1 lobby is rejected"
			);

			// Joining a lobby that doesn't exist is rejected.
			carol->errored = false;
			SessionProtocol::LobbyID bogus = { "smokehost", "999" };
			carol->c->Lobby_Join(bogus);
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return carol->errored &&
						carol->lastError == SessionClient::SCE_JOIN_REJECTED_NO_SUCH_LOBBY;
				}),
				"joining a nonexistent lobby is rejected"
			);

			// Chat: the lounge reaches every registered peer, lobby
			// channels reach only that lobby's members.
			alice->c->Chat_Send(SessionProtocol::CHAT_CHANNEL_LOUNGE, std::string("hello all"));
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return alice->chatLog.size() == 1 &&
						bob->chatLog.size() == 1 &&
						carol->chatLog.size() == 1;
				}),
				"lounge chat reaches every registered peer"
			);
			CHECK(
				carol->chatLog[0].from == "alice" &&
				carol->chatLog[0].text == "hello all" &&
				carol->chatLog[0].channel == SessionProtocol::CHAT_CHANNEL_LOUNGE,
				"chat events carry sender, text, and channel"
			);

			std::string lobbyChannel = alice->c->_lobbyData.id.key;
			alice->c->Chat_Send(lobbyChannel, std::string("gg"));
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return alice->chatLog.size() == 2 && bob->chatLog.size() == 2;
				}),
				"lobby chat reaches lobby members"
			);
			PumpUntil(server, 300, []() { return false; });
			CHECK(carol->chatLog.size() == 1, "lobby chat does not reach the lounge");

			carol->c->Chat_Send(lobbyChannel, std::string("sneaky"));
			PumpUntil(server, 300, []() { return false; });
			CHECK(
				alice->chatLog.size() == 2 && bob->chatLog.size() == 2,
				"chat to a lobby the sender isn't in is dropped"
			);

			for (int i = 0; i < 8; i++) {
				bob->c->Chat_Send(SessionProtocol::CHAT_CHANNEL_LOUNGE, std::string("spam"));
			}
			PumpUntil(server, 1000, []() { return false; });
			CHECK(
				carol->chatLog.size() == 1 + 5,
				"chat bursts are rate limited to five per window"
			);

			bob->c->Lobby_Leave();
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return alice->c->_lobbyData.members.size() == 1 &&
						bob->c->_lobbyData.members.empty();
				}),
				"bob leaves; alice sees one member, bob is back in the lounge"
			);

			bob->c->Lobby_Join(alice->c->_lobbyData.id);
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return bob->c->_lobbyData.members.size() == 2;
				}),
				"bob rejoins after leaving"
			);

			// Ready flow: both sides pick and ready up, which makes the
			// server broadcast all-ready and issue seat handoff tokens.
			Dimps::GameEvents::Wire::ConfirmedCharaConditions chara;
			memset(&chara, 0, sizeof(chara));
			chara.charaID = 5;
			alice->c->PreBattle_SetChara(chara);
			alice->c->PreBattle_SetEnv(1234);
			alice->c->PreBattle_SetStage(7);
			alice->c->Lobby_Ready();
			chara.charaID = 9;
			bob->c->PreBattle_SetChara(chara);
			bob->c->Lobby_Ready();
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return alice->readyFired && bob->readyFired &&
						!alice->handoff.token.empty() && !bob->handoff.token.empty();
				}),
				"all-ready fires and both seats receive handoff tokens"
			);
			CHECK(alice->handoff.token != bob->handoff.token, "handoff tokens are per-seat");
			std::string lobbyKey = alice->handoff.lobby.key;

			// "Game" connections take over the seats using the tokens,
			// exactly as the launched game processes will.
			TestClientCtx* gameAlice = MakeClient("alice", 24101, alice->handoff.lobby, alice->handoff.token);
			g_clients.push_back(gameAlice);
			TestClientCtx* gameBob = MakeClient("bob", 24102, bob->handoff.lobby, bob->handoff.token);
			g_clients.push_back(gameBob);
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return gameAlice->readyFired && gameBob->readyFired &&
						gameAlice->c->_lobbyData.members.size() == 2 &&
						gameBob->c->_lobbyData.members.size() == 2;
				}),
				"game connections take the seats and receive a targeted all-ready"
			);
			CHECK(
				gameAlice->c->_matchData.stageID == 7 &&
				gameAlice->c->_matchData.rngSeed == 1234 &&
				gameAlice->c->_matchData.chara[0].charaID == 5 &&
				gameAlice->c->_matchData.chara[1].charaID == 9,
				"game connections see the match setup agreed in the apps"
			);
			{
				sf4e::Lobby* lobby = server.registry.FindByKey(lobbyKey);
				CHECK(
					lobby &&
					lobby->members[0].data.port == 24101 &&
					lobby->members[1].data.port == 24102,
					"seats carry the game processes' GGPO ports after handoff"
				);
			}
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return alice->c->_lobbyData.members.empty() &&
						bob->c->_lobbyData.members.empty();
				}),
				"apps are returned to the lounge after their seats hand off"
			);

			size_t carolChats = carol->chatLog.size();
			alice->c->Chat_Send(SessionProtocol::CHAT_CHANNEL_LOUNGE, std::string("good luck!"));
			CHECK(
				PumpUntil(server, 5000, [&]() { return carol->chatLog.size() == carolChats + 1; }),
				"apps keep lounge chat after handing off their seats"
			);

			// Tokens are single-use and must not admit strangers.
			TestClientCtx* mallory = MakeClient("mallory", 24998, alice->handoff.lobby, alice->handoff.token);
			g_clients.push_back(mallory);
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return mallory->errored &&
						mallory->lastError == SessionClient::SCE_JOIN_REJECTED_HANDOFF_INVALID;
				}),
				"a consumed handoff token is rejected"
			);
			TestClientCtx* mallory2 = MakeClient("mallory2", 24997, alice->handoff.lobby, std::string("deadbeefdeadbeefdeadbeefdeadbeef"));
			g_clients.push_back(mallory2);
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return mallory2->errored &&
						mallory2->lastError == SessionClient::SCE_JOIN_REJECTED_HANDOFF_INVALID;
				}),
				"a forged handoff token is rejected"
			);

			// A full battle round trip: both games load, the server
			// releases the sync barrier, and when the battle ends the
			// ready/loaded cycle resets so the same seats can rematch
			// without reporting any results.
			gameAlice->c->Battle_Loaded();
			gameBob->c->Battle_Loaded();
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return gameAlice->syncedFired && gameBob->syncedFired;
				}),
				"both games loading releases the battle sync barrier"
			);

			uint32_t seedBefore = 0;
			{
				sf4e::Lobby* lobby = server.registry.FindByKey(lobbyKey);
				if (lobby) {
					seedBefore = lobby->match.rngSeed;
				}
			}
			gameAlice->c->Battle_Ended();
			gameBob->c->Battle_Ended();
			CHECK(
				PumpUntil(server, 5000, [&]() {
					sf4e::Lobby* lobby = server.registry.FindByKey(lobbyKey);
					if (!lobby) {
						return false;
					}
					bool flagsClear = true;
					for (auto memberIter = lobby->members.begin(); memberIter != lobby->members.end(); memberIter++) {
						flagsClear = flagsClear && !(memberIter->data.flags & SessionProtocol::MF_BATTLE_LOADED);
					}
					return !lobby->match.IsAllReady() && flagsClear && lobby->match.rngSeed != seedBefore;
				}),
				"battle ended resets the ready cycle and re-rolls the seed"
			);

			gameAlice->readyFired = false;
			gameBob->readyFired = false;
			chara.charaID = 12;
			gameAlice->c->PreBattle_SetChara(chara);
			gameAlice->c->PreBattle_SetEnv(777);
			gameAlice->c->PreBattle_SetStage(3);
			gameAlice->c->Lobby_Ready();
			chara.charaID = 20;
			gameBob->c->PreBattle_SetChara(chara);
			gameBob->c->Lobby_Ready();
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return gameAlice->readyFired && gameBob->readyFired;
				}),
				"the same seats can ready up again for a rematch"
			);

			// The opponent's game dying (crash, quit, refused launch)
			// frees its seat and resets the half-agreed match, but the
			// lobby itself survives for a rematch.
			gameBob->c->Disconnect();
			CHECK(
				PumpUntil(server, 5000, [&]() {
					sf4e::Lobby* lobby = server.registry.FindByKey(lobbyKey);
					return lobby &&
						lobby->members.size() == 1 &&
						!lobby->match.IsAllReady();
				}),
				"a dying game frees its seat and resets the match"
			);

			// The app re-seats its user for the rematch, exactly as
			// TickGameWatch does when it notices the game exited.
			bob->c->Lobby_Join(bob->handoff.lobby);
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return bob->c->_lobbyData.members.size() == 2;
				}),
				"the app re-seats its user after its game exits"
			);

			gameAlice->c->Disconnect();
			bob->c->Lobby_Leave();
			CHECK(
				PumpUntil(server, 5000, [&]() { return server.registry.lobbies.empty(); }),
				"empty user lobby is removed after everyone leaves"
			);

			return 0;
		}();

		for (auto iter = g_clients.begin(); iter != g_clients.end(); iter++) {
			(*iter)->c->Disconnect();
		}
		server.Step();
		server.Close();
		for (auto iter = g_clients.begin(); iter != g_clients.end(); iter++) {
			delete *iter;
		}
		g_clients.clear();
	}

	// Scenario 2: a hash-pinned server. The pin keeps mismatched game
	// builds (which would desync) out, but gameless app connections
	// carry no sidecar hash and must still be admitted.
	if (ret == 0) {
		const uint16_t PINNED_PORT = TEST_PORT + 1;
		SessionServer pinned(std::string("smokehost"), std::string("pinnedhash"));
		if (pinned.Listen(PINNED_PORT) != 0) {
			spdlog::error("FAIL: could not listen on {}", PINNED_PORT);
			ret = 1;
		}
		else {
			ret = [&pinned, PINNED_PORT]() -> int {
				SessionProtocol::LobbyID noLobby = { "", "" };

				TestClientCtx* app = MakeClient("app", 25001, noLobby, std::string(), std::string(""), PINNED_PORT);
				g_clients.push_back(app);
				CHECK(
					PumpUntil(pinned, 5000, [&]() { return pinned.peers.size() == 1; }),
					"a pinned server admits gameless app connections"
				);

				TestClientCtx* wrongGame = MakeClient("wrong", 25002, noLobby, std::string(), std::string("otherhash"), PINNED_PORT);
				g_clients.push_back(wrongGame);
				CHECK(
					PumpUntil(pinned, 5000, [&]() {
						return wrongGame->errored &&
							wrongGame->lastError == SessionClient::SCE_JOIN_REJECTED_HASH_INVALID;
					}),
					"a pinned server rejects mismatched game builds"
				);

				TestClientCtx* rightGame = MakeClient("right", 25003, noLobby, std::string(), std::string("pinnedhash"), PINNED_PORT);
				g_clients.push_back(rightGame);
				CHECK(
					PumpUntil(pinned, 5000, [&]() { return pinned.peers.size() == 2; }),
					"a pinned server admits the matching game build"
				);

				return 0;
			}();

			for (auto iter = g_clients.begin(); iter != g_clients.end(); iter++) {
				(*iter)->c->Disconnect();
			}
			pinned.Step();
			pinned.Close();
			for (auto iter = g_clients.begin(); iter != g_clients.end(); iter++) {
				delete *iter;
			}
			g_clients.clear();
		}
	}

	// Scenario 3: presence, private challenges, and quickmatch.
	if (ret == 0) {
		const uint16_t SOCIAL_PORT = TEST_PORT + 2;
		SessionServer social(std::string("smokehost"), std::string(""));
		social.challengeTtlMs = 300;
		if (social.Listen(SOCIAL_PORT) != 0) {
			spdlog::error("FAIL: could not listen on {}", SOCIAL_PORT);
			ret = 1;
		}
		else {
			ret = [&social, SOCIAL_PORT]() -> int {
				SessionProtocol::LobbyID noLobby = { "", "" };
				TestClientCtx* dave = MakeClient("dave", 24010, noLobby, std::string(), std::string(""), SOCIAL_PORT);
				g_clients.push_back(dave);
				TestClientCtx* erin = MakeClient("erin", 24011, noLobby, std::string(), std::string(""), SOCIAL_PORT);
				g_clients.push_back(erin);
				TestClientCtx* frank = MakeClient("frank", 24012, noLobby, std::string(), std::string(""), SOCIAL_PORT);
				g_clients.push_back(frank);
				CHECK(
					PumpUntil(social, 5000, [&]() { return social.peers.size() == 3; }),
					"dave, erin, and frank register"
				);

				auto statusOf = [](TestClientCtx* who, const std::string& name) -> int {
					for (auto iter = who->presence.begin(); iter != who->presence.end(); iter++) {
						if (iter->name == name) {
							return (int)iter->status;
						}
					}
					return -1;
				};
				auto refreshPresence = [&social](TestClientCtx* who) -> bool {
					int version = who->presenceVersion;
					who->c->Presence_RequestList();
					return PumpUntil(social, 5000, [&]() { return who->presenceVersion > version; });
				};

				CHECK(
					refreshPresence(dave) &&
					dave->presence.size() == 3 &&
					statusOf(dave, "dave") == (int)SessionProtocol::PS_LOUNGE &&
					statusOf(dave, "erin") == (int)SessionProtocol::PS_LOUNGE &&
					statusOf(dave, "frank") == (int)SessionProtocol::PS_LOUNGE,
					"presence lists every player in the lounge"
				);

				// Decline round trip.
				dave->c->Challenge_Send(std::string("erin"));
				CHECK(
					PumpUntil(social, 5000, [&]() { return erin->challengeFrom == "dave"; }),
					"the challenge reaches its target"
				);
				erin->c->Challenge_Answer(std::string("dave"), false);
				CHECK(
					PumpUntil(social, 5000, [&]() {
						return dave->gotChallengeResult &&
							dave->lastChallengeResult.result == SessionProtocol::CR_DECLINED;
					}),
					"a declined challenge reports back"
				);

				// Accept round trip pairs both into an unlisted lobby.
				dave->gotChallengeResult = false;
				erin->challengeFrom.clear();
				dave->c->Challenge_Send(std::string("erin"));
				CHECK(
					PumpUntil(social, 5000, [&]() { return erin->challengeFrom == "dave"; }),
					"the rematch challenge reaches its target"
				);
				erin->c->Challenge_Answer(std::string("dave"), true);
				CHECK(
					PumpUntil(social, 5000, [&]() {
						return dave->gotChallengeResult &&
							dave->lastChallengeResult.result == SessionProtocol::CR_ACCEPTED &&
							dave->c->_lobbyData.members.size() == 2 &&
							erin->c->_lobbyData.members.size() == 2;
					}),
					"an accepted challenge seats both players together"
				);
				frank->listCount = -1;
				frank->c->Lobby_RequestList();
				CHECK(
					PumpUntil(social, 5000, [&]() { return frank->listCount == 0; }),
					"challenge lobbies are unlisted"
				);
				CHECK(
					refreshPresence(frank) &&
					statusOf(frank, "dave") == (int)SessionProtocol::PS_IN_LOBBY,
					"presence shows challenged players in a lobby"
				);

				// Challenging someone already seated is refused as busy.
				frank->gotChallengeResult = false;
				frank->c->Challenge_Send(std::string("dave"));
				CHECK(
					PumpUntil(social, 5000, [&]() {
						return frank->gotChallengeResult &&
							frank->lastChallengeResult.result == SessionProtocol::CR_BUSY;
					}),
					"challenging a seated player reports busy"
				);

				dave->c->Lobby_Leave();
				erin->c->Lobby_Leave();
				CHECK(
					PumpUntil(social, 5000, [&]() { return social.registry.lobbies.empty(); }),
					"the challenge lobby empties away"
				);

				// An unanswered challenge expires (shortened TTL).
				frank->gotChallengeResult = false;
				frank->c->Challenge_Send(std::string("erin"));
				CHECK(
					PumpUntil(social, 5000, [&]() {
						return frank->gotChallengeResult &&
							frank->lastChallengeResult.result == SessionProtocol::CR_EXPIRED;
					}),
					"an unanswered challenge expires"
				);
				erin->challengeFrom.clear();

				// Quickmatch pairs the two queued players.
				dave->c->Matchmake_Set(true);
				PumpUntil(social, 300, []() { return false; });
				CHECK(
					refreshPresence(frank) && frank->lookingCount == 1,
					"the quickmatch queue is visible in presence"
				);
				erin->c->Matchmake_Set(true);
				CHECK(
					PumpUntil(social, 5000, [&]() {
						return dave->matchedOpponent == "erin" &&
							erin->matchedOpponent == "dave" &&
							dave->c->_lobbyData.members.size() == 2 &&
							erin->c->_lobbyData.members.size() == 2;
					}),
					"quickmatch pairs both queued players"
				);
				frank->listCount = -1;
				frank->c->Lobby_RequestList();
				CHECK(
					PumpUntil(social, 5000, [&]() { return frank->listCount == 0; }),
					"quickmatch lobbies are unlisted"
				);
				CHECK(
					refreshPresence(frank) && frank->lookingCount == 0,
					"pairing clears the quickmatch queue"
				);

				// A challenged player going offline reports back.
				dave->c->Lobby_Leave();
				erin->c->Lobby_Leave();
				PumpUntil(social, 300, []() { return false; });
				frank->gotChallengeResult = false;
				frank->c->Challenge_Send(std::string("dave"));
				CHECK(
					PumpUntil(social, 5000, [&]() { return dave->challengeFrom == "frank"; }),
					"the offline-test challenge reaches its target"
				);
				dave->c->Disconnect();
				CHECK(
					PumpUntil(social, 5000, [&]() {
						return frank->gotChallengeResult &&
							frank->lastChallengeResult.result == SessionProtocol::CR_OFFLINE;
					}),
					"a challenged player disconnecting reports offline"
				);

				return 0;
			}();

			for (auto iter = g_clients.begin(); iter != g_clients.end(); iter++) {
				(*iter)->c->Disconnect();
			}
			social.Step();
			social.Close();
			for (auto iter = g_clients.begin(); iter != g_clients.end(); iter++) {
				delete *iter;
			}
			g_clients.clear();
		}
	}

	GameNetworkingSockets_Kill();
	if (ret == 0) {
		spdlog::info("SMOKE OK");
	}
	return ret;
}
