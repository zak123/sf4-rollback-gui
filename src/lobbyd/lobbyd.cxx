#include <atomic>
#include <string>

#include <windows.h>

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>

#include "../Dimps/Dimps__Math.hxx"
#include "../session/sf4e__SessionServer.hxx"

using Dimps::Math::FixedPoint;

// Lobbyd is the dedicated, headless session server. It hosts the same
// session protocol that an in-game host would, but runs outside the game
// process so it can live on always-on infrastructure. See
// docs/product-design.md for the roadmap; this iteration hosts a single
// 1v1 lobby, matching the in-game server's behavior.

static std::atomic<bool> g_bQuit(false);

static BOOL WINAPI OnConsoleCtrl(DWORD dwCtrlType) {
	switch (dwCtrlType) {
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
		g_bQuit = true;
		return TRUE;
	default:
		return FALSE;
	}
}

static void OnGNSDebugOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg) {
	if (eType <= k_ESteamNetworkingSocketsDebugOutputType_Warning) {
		spdlog::warn("GNS: {}", pszMsg);
	}
	else {
		spdlog::debug("GNS: {}", pszMsg);
	}
}

int main(int argc, char** argv) {
	uint16_t nPort = 23450;
	std::string identity = "localhost";
	std::string sidecarHash = "";
	int nRoundCount = 3;
	int nRoundTimeSecs = 99;
	bool bNoEditionSelect = false;
	bool bVerbose = false;

	CLI::App app("sf4e dedicated lobby server", "Lobbyd");
	app.add_option("--port", nPort, "UDP port to listen on (default 23450)");
	app.add_option(
		"--identity",
		identity,
		"Routing identity clients see in connection and lobby IDs. Use the "
		"public address of this host once exposed beyond localhost."
	);
	app.add_option(
		"--sidecar-hash",
		sidecarHash,
		"Only admit clients whose Sidecar.dll hash matches. Empty (default) "
		"accepts any build- fine for development, not for public servers."
	);
	app.add_option("--rounds", nRoundCount, "Rounds per match")
		->check(CLI::IsMember({ 1, 3, 5, 7 }));
	app.add_option("--round-time", nRoundTimeSecs, "Round timer in seconds")
		->check(CLI::IsMember({ 30, 60, 99 }));
	app.add_flag("--no-edition-select", bNoEditionSelect, "Disable edition select");
	app.add_flag("--verbose", bVerbose, "Enable debug logging");
	CLI11_PARSE(app, argc, argv);

	if (bVerbose) {
		spdlog::set_level(spdlog::level::debug);
	}

	SteamDatagramErrMsg errMsg;
	if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
		spdlog::error("GameNetworkingSockets_Init failed: {}", errMsg);
		return 1;
	}
	SteamNetworkingUtils()->SetDebugOutputFunction(
		k_ESteamNetworkingSocketsDebugOutputType_Everything,
		OnGNSDebugOutput
	);

	SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);

	int ret = 0;
	{
		sf4e::SessionServer server(
			identity,
			sidecarHash,
			!bNoEditionSelect,
			nRoundCount,
			FixedPoint{ 0, (short)nRoundTimeSecs }
		);
		if (server.Listen(nPort) != 0) {
			spdlog::error("Lobbyd could not listen on port {}", nPort);
			ret = 1;
		}
		else {
			if (sidecarHash.empty()) {
				spdlog::warn("No --sidecar-hash provided; admitting clients of any build");
			}
			spdlog::info(
				"Lobbyd up: port {}, identity \"{}\", {} rounds, {}s timer, edition select {}",
				nPort, identity, nRoundCount, nRoundTimeSecs, bNoEditionSelect ? "off" : "on"
			);

			while (!g_bQuit) {
				server.PrepareForCallbacks();
				SteamNetworkingSockets()->RunCallbacks();
				if (server.Step() != 0) {
					spdlog::error("Lobbyd session server errored; shutting down");
					ret = 1;
					break;
				}
				Sleep(10);
			}

			spdlog::info("Lobbyd shutting down");
			server.Close();
		}
	}

	GameNetworkingSockets_Kill();
	return ret;
}
