#include <atomic>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <spdlog/sinks/wincolor_sink.h>
#else
#include <csignal>
#include <limits.h>
#include <unistd.h>
#include <spdlog/sinks/ansicolor_sink.h>
#endif

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>

#include "../Dimps/Dimps__Wire.hxx"
#include "../session/sf4e__Portable.hxx"
#include "../session/sf4e__SessionServer.hxx"

using Dimps::Math::FixedPoint;

// Lobbyd is the dedicated, headless session server. It hosts the same
// session protocol that an in-game host would, but runs outside the game
// process so it can live on always-on infrastructure. See
// docs/product-design.md for the roadmap; this iteration hosts a single
// 1v1 lobby, matching the in-game server's behavior.

static std::atomic<bool> g_bQuit(false);

#ifdef _WIN32

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

static LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* pInfo) {
	// The supervisor restarts the process; make sure the log names the
	// crash first. The first live server crash died silently, leaving
	// only the next boot banner- undebuggable.
	if (pInfo && pInfo->ExceptionRecord) {
		uintptr_t addr = (uintptr_t)pInfo->ExceptionRecord->ExceptionAddress;
		uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
		spdlog::critical(
			"FATAL: unhandled exception {:#010x} at {:#010x} (module base {:#010x}, offset {:#x})",
			(uint32_t)pInfo->ExceptionRecord->ExceptionCode,
			addr,
			base,
			addr >= base ? addr - base : 0
		);
		spdlog::shutdown();
	}
	return EXCEPTION_EXECUTE_HANDLER;
}

#else

// On Linux the equivalents come from the platform: systemd/journald
// keep the crash record (plus core dumps), and SIGINT/SIGTERM are the
// shutdown signals.
static void OnPosixSignal(int) {
	g_bQuit = true;
}

#endif

static void OnGNSDebugOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg) {
	if (eType <= k_ESteamNetworkingSocketsDebugOutputType_Warning) {
		spdlog::warn("GNS: {}", pszMsg);
	}
	else {
		spdlog::debug("GNS: {}", pszMsg);
	}
}

// Log to a rotating file beside the exe as well as the console, so a
// headless run (scheduled task / service) still leaves a record- the
// absence of which made the first outage undiagnosable.
static void SetUpLogging(bool verbose) {
	std::vector<spdlog::sink_ptr> sinks;
#ifdef _WIN32
	sinks.push_back(std::make_shared<spdlog::sinks::wincolor_stdout_sink_mt>());

	wchar_t exePath[MAX_PATH] = { 0 };
	if (GetModuleFileNameW(NULL, exePath, MAX_PATH)) {
		std::wstring dir(exePath);
		size_t slash = dir.find_last_of(L"\\/");
		if (slash != std::wstring::npos) {
			std::wstring logPath = dir.substr(0, slash) + L"\\logs\\Lobbyd.log";
			try {
				sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
					logPath, 1048576 * 5, 3, false
				));
			}
			catch (const spdlog::spdlog_ex&) {
				// File logging is best-effort; the console sink still works.
			}
		}
	}
#else
	sinks.push_back(std::make_shared<spdlog::sinks::ansicolor_stdout_sink_mt>());

	char exePath[PATH_MAX] = { 0 };
	ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
	if (n > 0) {
		std::string dir(exePath, (size_t)n);
		size_t slash = dir.find_last_of('/');
		if (slash != std::string::npos) {
			std::string logPath = dir.substr(0, slash) + "/logs/Lobbyd.log";
			try {
				sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
					logPath, 1048576 * 5, 3, false
				));
			}
			catch (const spdlog::spdlog_ex&) {
				// File logging is best-effort; the console sink still works.
			}
		}
	}
#endif

	auto logger = std::make_shared<spdlog::logger>("Lobbyd", sinks.begin(), sinks.end());
	spdlog::set_default_logger(logger);
	spdlog::flush_on(spdlog::level::info);
	if (verbose) {
		spdlog::set_level(spdlog::level::debug);
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
	app.add_option("--rounds", nRoundCount, "Rounds per match in server-created lobbies (default, challenge, quickmatch)")
		->check(CLI::IsMember({ 1, 3, 5, 7 }));
	app.add_option("--round-time", nRoundTimeSecs, "Round timer in server-created lobbies")
		->check(CLI::IsMember({ 30, 60, 99 }));
	app.add_flag("--no-edition-select", bNoEditionSelect, "Disable edition select in server-created lobbies");
	bool bNoDefaultLobby = false;
	app.add_flag(
		"--no-default-lobby",
		bNoDefaultLobby,
		"Don't host the compatibility default lobby. Clients joining without "
		"naming a lobby idle in the lounge instead of being seated, so every "
		"lobby must be created explicitly."
	);
	int nMaxPeers = 64;
	app.add_option(
		"--max-peers",
		nMaxPeers,
		"Most registered users admitted at once; 0 means unlimited"
	)->check(CLI::Range(0, 10000));
	app.add_flag("--verbose", bVerbose, "Enable debug logging");
	CLI11_PARSE(app, argc, argv);

	SetUpLogging(bVerbose);
#ifdef _WIN32
	SetUnhandledExceptionFilter(OnUnhandledException);
#endif

	SteamDatagramErrMsg errMsg;
	if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
		spdlog::error("GameNetworkingSockets_Init failed: {}", errMsg);
		return 1;
	}
	SteamNetworkingUtils()->SetDebugOutputFunction(
		k_ESteamNetworkingSocketsDebugOutputType_Everything,
		OnGNSDebugOutput
	);

#ifdef _WIN32
	SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
#else
	signal(SIGINT, OnPosixSignal);
	signal(SIGTERM, OnPosixSignal);
#endif

	int ret = 0;
	{
		std::unique_ptr<sf4e::SessionServer> pServer;
		if (bNoDefaultLobby) {
			pServer.reset(new sf4e::SessionServer(identity, sidecarHash));
		}
		else {
			pServer.reset(new sf4e::SessionServer(
				identity,
				sidecarHash,
				!bNoEditionSelect,
				nRoundCount,
				FixedPoint{ 0, (short)nRoundTimeSecs }
			));
		}
		sf4e::SessionServer& server = *pServer;
		server.maxPeers = (size_t)nMaxPeers;
		server.matchEditionSelect = !bNoEditionSelect;
		server.matchRoundCount = nRoundCount;
		server.matchRoundTime = FixedPoint{ 0, (short)nRoundTimeSecs };
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

			uint64_t nextHeartbeat = sf4e::Portable::NowMs() + 60000;
			uint64_t lastStepErrorLog = 0;
			while (!g_bQuit) {
				server.PrepareForCallbacks();
				SteamNetworkingSockets()->RunCallbacks();

				// A transient message-poll error must not take the whole
				// server down- log it (rate-limited) and keep serving.
				// Previously this broke the loop and exited the process,
				// which, with no auto-restart, meant a single GNS hiccup
				// became a multi-minute outage.
				if (server.Step() != 0) {
					uint64_t now = sf4e::Portable::NowMs();
					if (now - lastStepErrorLog > 1000) {
						lastStepErrorLog = now;
						spdlog::error("Lobbyd Step() errored; continuing to serve");
					}
				}

				uint64_t now = sf4e::Portable::NowMs();
				if (now >= nextHeartbeat) {
					nextHeartbeat = now + 60000;
					spdlog::info("Lobbyd heartbeat: {} peers, {} lobbies", server.peers.size(), server.registry.lobbies.size());
				}

				sf4e::Portable::SleepMs(10);
			}

			spdlog::info("Lobbyd shutting down");
			server.Close();
		}
	}

	GameNetworkingSockets_Kill();
	return ret;
}
