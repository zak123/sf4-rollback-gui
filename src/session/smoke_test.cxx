// Headless exerciser for the session protocol. Hosts a dedicated-mode
// SessionServer in-process, then drives clients through registration,
// lobby browsing, creation, joining, and leaving over real loopback
// sockets, exiting nonzero on the first failed expectation. Run it after
// touching anything in src/session/.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>

#include "sf4e__LobbyRegistry.hxx"
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
	std::vector<SessionProtocol::LobbyListEntry> lastListing;
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
	spdlog::info("[{}] ready callback", ctx->name);
}

static void OnBattleSynced(SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	TestClientCtx* ctx = (TestClientCtx*)callbacks.data;
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

static TestClientCtx* MakeClient(const std::string& name) {
	TestClientCtx* ctx = new TestClientCtx();
	ctx->name = name;

	SessionClient::Callbacks callbacks = {
		ctx,
		OnError,
		OnReady,
		OnBattleSynced,
		OnLobbyCreated,
		OnLobbyList,
	};
	std::string mutableName = name;
	ctx->c.reset(new SessionClient(callbacks, std::string("smokehash"), 24000, mutableName));

	SteamNetworkingIPAddr addr;
	addr.Clear();
	char szAddr[32];
	snprintf(szAddr, sizeof(szAddr), "127.0.0.1:%d", TEST_PORT);
	addr.ParseString(szAddr);
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
		Sleep(5);
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

	int ret = 1;
	{
		// Dedicated mode: no default lobby, accept-any-hash.
		SessionServer server(std::string("smokehost"), std::string(""));
		if (server.Listen(TEST_PORT) != 0) {
			spdlog::error("FAIL: could not listen on {}", TEST_PORT);
			return 1;
		}

		ret = [&server]() -> int {
			TestClientCtx* alice = MakeClient("alice");
			g_clients.push_back(alice);
			TestClientCtx* bob = MakeClient("bob");
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
			TestClientCtx* fakeAlice = MakeClient("alice");
			g_clients.push_back(fakeAlice);
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return fakeAlice->errored &&
						fakeAlice->lastError == SessionClient::SCE_JOIN_REJECTED_NAME_TAKEN;
				}),
				"duplicate name is rejected at registration"
			);

			// A third player can register, but the 1v1 lobby is full.
			TestClientCtx* carol = MakeClient("carol");
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

			bob->c->Lobby_Leave();
			CHECK(
				PumpUntil(server, 5000, [&]() {
					return alice->c->_lobbyData.members.size() == 1 &&
						bob->c->_lobbyData.members.empty();
				}),
				"bob leaves; alice sees one member, bob is back in the lounge"
			);

			alice->c->Lobby_Leave();
			CHECK(
				PumpUntil(server, 5000, [&]() { return server.registry.lobbies.empty(); }),
				"empty user lobby is removed from the registry"
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

	GameNetworkingSockets_Kill();
	if (ret == 0) {
		spdlog::info("SMOKE OK");
	}
	return ret;
}
