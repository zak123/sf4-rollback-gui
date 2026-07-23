// LobbyClient: the sf4e lobby desktop app. Connects to a Lobbyd server
// for chatting, browsing, and joining 1v1 lobbies. Launching the game
// straight into a ready match is the next milestone; until then, matches
// set up here are joined from the in-game overlay.
//
// The window skeleton is the stock Dear ImGui DX9/Win32 example, shared
// with src/session/interactive_test.cxx.
#ifndef UNICODE
#define UNICODE
#endif

#include <algorithm>
#include <deque>
#include <float.h>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <time.h>

#include <windows.h>
#include <mmsystem.h>
#include <d3d9.h>
#include <KnownFolders.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <tchar.h>

#include <CLI/CLI.hpp>
#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/wincolor_sink.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>

#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../session/sf4e__Resolve.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "../session/sf4e__SessionProtocol.hxx"
#include "sf4e__Roster.hxx"

namespace SessionProtocol = sf4e::SessionProtocol;
namespace Roster = sf4e::Roster;
using rVsMode = Dimps::GameEvents::VsMode;
using sf4e::SessionClient;

// -------------------------------------------------------------------
// D3D9 window skeleton state
static LPDIRECT3D9              g_pD3D = nullptr;
static LPDIRECT3DDEVICE9        g_pd3dDevice = nullptr;
static bool                     g_DeviceLost = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static D3DPRESENT_PARAMETERS    g_d3dpp = {};

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void ResetDevice();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// The main window, for taskbar attention flashes from session callbacks.
static HWND g_hWnd = nullptr;

// -------------------------------------------------------------------
// App state

static const size_t CHAT_HISTORY_MAX = 500;
static const uint64_t LOBBY_LIST_REFRESH_MS = 2000;

enum AppScreen {
	SCREEN_LOGIN,
	SCREEN_SESSION,
};

struct App {
	AppScreen screen = SCREEN_LOGIN;
	std::unique_ptr<SessionClient> client;

	char szName[32] = { 0 };
	char szServerAddr[64] = "sf4.zak123.com";
	char szChatInput[SessionProtocol::CHAT_TEXT_MAX] = { 0 };
	char szNewLobbyName[64] = { 0 };
	int newLobbyRoundCountIdx = 1;
	int newLobbyRoundTimeIdx = 2;

	rVsMode::ConfirmedCharaConditions myConditions;
	int myCharaID = 0;
	int stageID = 0;

	std::deque<SessionProtocol::ChatEvent> chatLog;
	std::deque<std::string> alerts;
	// When the currently shown (front) alert appeared, for auto-expiry.
	uint64_t alertFrontSince = 0;
	// One version warning per connection, not one per hello.
	bool bVersionWarned = false;
	uint64_t nextListRefresh = 0;
	bool bothReady = false;
	bool gameLaunched = false;
	uint16_t ggpoPort = 23457;

	// The launched game process, watched so the app can reset and
	// re-seat its user when the match ends (or the game dies).
	HANDLE hGameProcess = NULL;
	SessionProtocol::LobbyID rejoinTarget = { "", "" };
	int rejoinAttempts = 0;
	uint64_t nextRejoinAt = 0;

	// Challenge state: an incoming challenge awaiting an answer, and
	// the target of our outgoing one.
	char szChallengeFrom[32] = { 0 };
	uint64_t challengeReceivedAt = 0;
	char szChallengeTarget[32] = { 0 };

	// Quickmatch toggle, mirroring the server's queue flag.
	bool bLooking = false;

	// Silences the notification sound (challenges, quickmatch). The
	// taskbar flash stays on- that's the quiet channel.
	bool bMuted = false;

	// Launch the game with --force-relay so its matches route through
	// the server's UDP relay even when the NAT probe looks clean. The
	// rescue for probe-invisible NATs (per-destination-IP mappings);
	// either side setting it relays the match for both.
	bool bForceRelay = false;

	std::mt19937 rand;

	App() {
		memset(&myConditions, 0, sizeof(myConditions));
		myConditions.unc_edition = Dimps::Game::Battle::ED_USF4;
		rand.seed((unsigned int)time(nullptr));
	}
};

static App g_app;

static const std::pair<int, const char* const> kRoundCounts[4] = {
	{1, "1"}, {3, "3"}, {5, "5"}, {7, "7"},
};
static const std::pair<Dimps::Math::FixedPoint, const char* const> kRoundTimes[3] = {
	{{0, 30}, "30"}, {{0, 60}, "60"}, {{0, 99}, "99"},
};

// -------------------------------------------------------------------
// Config persistence: name, server, and the player's main.

static std::wstring ConfigPath() {
	PWSTR appdata;
	std::wstring out;
	if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &appdata) == S_OK) {
		wchar_t dir[MAX_PATH];
		wchar_t file[MAX_PATH];
		PathCombineW(dir, appdata, L"sf4e");
		CreateDirectoryW(dir, NULL);
		PathCombineW(file, dir, L"LobbyClient.json");
		out = file;
		CoTaskMemFree(appdata);
	}
	return out;
}

static void LoadConfig() {
	std::wstring path = ConfigPath();
	if (path.empty()) {
		return;
	}
	std::ifstream f(path);
	if (!f.good()) {
		return;
	}
	try {
		nlohmann::json j;
		f >> j;
		strncpy_s(g_app.szName, j.value("name", std::string()).c_str(), _TRUNCATE);
		strncpy_s(g_app.szServerAddr, j.value("server", std::string(g_app.szServerAddr)).c_str(), _TRUNCATE);
		g_app.myCharaID = j.value("charaID", 0);
		g_app.myConditions.color = (BYTE)j.value("color", 0);
		g_app.myConditions.costume = (BYTE)j.value("costume", 0);
		g_app.myConditions.ultraCombo = (BYTE)j.value("ultra", 0);
		g_app.stageID = j.value("stageID", 0);
		g_app.bMuted = j.value("muted", false);
		g_app.bForceRelay = j.value("forceRelay", false);
	}
	catch (...) {
		// A mangled config just means defaults.
	}
	if (g_app.myCharaID < 0 || g_app.myCharaID >= Roster::NUM_CHARACTERS) {
		g_app.myCharaID = 0;
	}
	if (g_app.stageID < 0 || g_app.stageID >= Roster::NUM_STAGES) {
		g_app.stageID = 0;
	}
}

static void SaveConfig() {
	std::wstring path = ConfigPath();
	if (path.empty()) {
		return;
	}
	try {
		nlohmann::json j;
		j["name"] = std::string(g_app.szName);
		j["server"] = std::string(g_app.szServerAddr);
		j["charaID"] = g_app.myCharaID;
		j["color"] = (int)g_app.myConditions.color;
		j["costume"] = (int)g_app.myConditions.costume;
		j["ultra"] = (int)g_app.myConditions.ultraCombo;
		j["stageID"] = g_app.stageID;
		j["muted"] = g_app.bMuted;
		j["forceRelay"] = g_app.bForceRelay;
		std::ofstream f(path);
		f << j.dump(2);
	}
	catch (...) {
	}
}

// -------------------------------------------------------------------
// Alerts
//
// One alert shows at a time and auto-expires; everything pushed here
// also belongs in the log, which is where the full history lives.
// Consecutive duplicates collapse so a retry loop can't stack the
// queue with the same line.

static void PushAlert(const std::string& text) {
	if (!g_app.alerts.empty() && g_app.alerts.back() == text) {
		return;
	}
	g_app.alerts.push_back(text);
}

// -------------------------------------------------------------------
// Session client callbacks

static void OnError(SessionClient::ErrorType errType, SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	// Lobby-level rejections during the automatic post-game rejoin are
	// part of a retry loop- surface only the loop's terminal outcome,
	// not every attempt.
	bool bRejoining = g_app.rejoinAttempts > 0;

	switch (errType) {
	case SessionClient::SCE_JOIN_REJECTED_HASH_INVALID:
		PushAlert("Rejected by server: version mismatch");
		break;
	case SessionClient::SCE_JOIN_REJECTED_NAME_TAKEN:
		PushAlert("Rejected by server: that name is taken");
		break;
	case SessionClient::SCE_JOIN_REJECTED_LOBBY_FULL:
		if (bRejoining) {
			spdlog::info("Rejoin attempt bounced: lobby full, retrying");
			break;
		}
		PushAlert("Could not join: lobby is full");
		break;
	case SessionClient::SCE_JOIN_REJECTED_NO_SUCH_LOBBY:
		// No sense retrying a rejoin into a dead lobby.
		if (bRejoining) {
			spdlog::info("Rejoin abandoned: the lobby is gone");
			g_app.rejoinAttempts = 0;
			break;
		}
		g_app.rejoinAttempts = 0;
		PushAlert("Could not join: lobby no longer exists");
		break;
	case SessionClient::SCE_JOIN_REJECTED_ALREADY_IN_LOBBY:
		if (bRejoining) {
			spdlog::info("Rejoin attempt bounced: already seated");
			g_app.rejoinAttempts = 0;
			break;
		}
		PushAlert("Could not join: already in a lobby");
		break;
	case SessionClient::SCE_JOIN_REJECTED_REQUEST_INVALID:
		PushAlert("Rejected by server: bad request- version mismatch?");
		break;
	case SessionClient::SCE_JOIN_REJECTED_SERVER_FULL:
		PushAlert("The server is full- try again later");
		break;
	default:
		PushAlert("Unknown session error");
		break;
	}
}

static void OnReady(SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	g_app.bothReady = true;
}

static void OnBattleSynced(SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	spdlog::info("Battle synced");
}

static void OnLobbyCreated(SessionProtocol::JoinResult result, SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	if (result != SessionProtocol::JOIN_OK) {
		PushAlert("Could not create lobby");
	}
}

static void OnLobbyList(SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	// Listing is read straight from client->_lobbyListing at draw time.
}

static void OnChat(const SessionProtocol::ChatEvent& event, SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	g_app.chatLog.push_back(event);
	while (g_app.chatLog.size() > CHAT_HISTORY_MAX) {
		g_app.chatLog.pop_front();
	}
}

// The server issued this app's seat a handoff token: launch the game
// with the join parameters so it can take the seat and start the match.
static void OnMatchHandoff(const SessionProtocol::MatchHandoff& handoff, SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	wchar_t szDir[MAX_PATH] = { 0 };
	wchar_t szLauncher[MAX_PATH] = { 0 };
	GetModuleFileNameW(NULL, szDir, MAX_PATH);
	PathRemoveFileSpecW(szDir);
	PathCombineW(szLauncher, szDir, L"Launcher.exe");
	if (!PathFileExistsW(szLauncher)) {
		PushAlert("Match is ready, but Launcher.exe isn't next to this app!");
		return;
	}

	char cmdA[1024];
	snprintf(
		cmdA,
		sizeof(cmdA),
		"\"%ws\" --join-server \"%s\" --join-lobby-host \"%s\" --join-lobby-key \"%s\" --join-token \"%s\" --join-name \"%s\" --ggpo-port %u%s",
		szLauncher,
		g_app.szServerAddr,
		handoff.lobby.host.c_str(),
		handoff.lobby.key.c_str(),
		handoff.token.c_str(),
		c->_name.c_str(),
		(unsigned)g_app.ggpoPort,
		g_app.bForceRelay ? " --force-relay" : ""
	);
	wchar_t cmdW[1024];
	MultiByteToWideChar(CP_UTF8, 0, cmdA, -1, cmdW, 1024);

	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	ZeroMemory(&pi, sizeof(pi));
	si.cb = sizeof(si);
	if (CreateProcessW(szLauncher, cmdW, NULL, NULL, FALSE, 0, NULL, szDir, &si, &pi)) {
		CloseHandle(pi.hThread);
		if (g_app.hGameProcess) {
			CloseHandle(g_app.hGameProcess);
		}
		g_app.hGameProcess = pi.hProcess;
		g_app.rejoinTarget = handoff.lobby;
		g_app.gameLaunched = true;
		// No alert: the room card's NOW PLAYING banner carries the
		// controller-binding instruction for as long as it's relevant.
		spdlog::info("Launched game for lobby {} handoff", handoff.lobby.key);
	}
	else {
		PushAlert("Match is ready, but the game could not be launched");
		spdlog::error("CreateProcess for Launcher.exe failed: {}", GetLastError());
	}
}

// Ring the bundled notification sound (notify.wav beside the exe) when
// something needs the player's attention. A missing file is silence,
// never a fallback beep.
static void PlayNotifySound() {
	static wchar_t szWav[MAX_PATH] = { 0 };
	if (!szWav[0]) {
		wchar_t szDir[MAX_PATH] = { 0 };
		GetModuleFileNameW(NULL, szDir, MAX_PATH);
		PathRemoveFileSpecW(szDir);
		PathCombineW(szWav, szDir, L"notify.wav");
	}
	PlaySoundW(szWav, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

// Something needs the player: sound (unless muted) plus a taskbar
// flash when the window is in the background. The flash runs until
// the window comes back to the foreground.
static void NotifyAttention() {
	if (!g_app.bMuted) {
		PlayNotifySound();
	}
	if (g_hWnd && GetForegroundWindow() != g_hWnd) {
		FLASHWINFO fi = { 0 };
		fi.cbSize = sizeof(fi);
		fi.hwnd = g_hWnd;
		fi.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
		FlashWindowEx(&fi);
	}
}

static void OnChallengeEvent(const SessionProtocol::ChallengeEvent& event, SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	strncpy_s(g_app.szChallengeFrom, event.from.c_str(), _TRUNCATE);
	g_app.challengeReceivedAt = GetTickCount64();
	NotifyAttention();
}

static void OnChallengeResult(const SessionProtocol::ChallengeResultMsg& result, SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	switch (result.result) {
	case SessionProtocol::CR_ACCEPTED:
		PushAlert(result.target + " accepted- get ready!");
		break;
	case SessionProtocol::CR_DECLINED:
		PushAlert(result.target + " declined the challenge");
		break;
	case SessionProtocol::CR_EXPIRED:
		PushAlert(result.target + " didn't answer");
		break;
	case SessionProtocol::CR_BUSY:
		PushAlert(result.target + " is busy");
		break;
	case SessionProtocol::CR_OFFLINE:
		PushAlert(result.target + " went offline");
		break;
	}
	g_app.szChallengeTarget[0] = 0;
}

static void OnMatchmakeEvent(const SessionProtocol::MatchmakeEvent& event, SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	PushAlert("Matched with " + event.opponent + "!");
	g_app.bLooking = false;
	NotifyAttention();
}

static SessionClient::Callbacks kCallbacks = {
	nullptr,
	OnError,
	OnReady,
	OnBattleSynced,
	OnLobbyCreated,
	OnLobbyList,
	OnChat,
	OnMatchHandoff,
	nullptr,
	OnChallengeEvent,
	OnChallengeResult,
	OnMatchmakeEvent,
};

// -------------------------------------------------------------------
// Game graphics settings light. Two of the game's own settings break
// rollback netplay: a "Smooth"/"Variable" frame rate changes the
// simulation rate itself (mismatched peers desync), and motion blur
// holds state the save states don't capture. The UI shows a red/green
// light with a one-click fix; the values live in the game's config.ini
// and are also fine settings for offline play.

enum GameSettingsState {
	GS_UNKNOWN = 0,   // config.ini missing or unreadable
	GS_READY,
	GS_NEEDS_FIX,
};

static const struct { const char* key; const char* required; } kRequiredGameSettings[] = {
	{ "FrameRate", "FIXED" },
	{ "MotionBlurQuality", "OFF" },
};

static GameSettingsState g_gameSettingsState = GS_UNKNOWN;
static uint64_t g_nextGameSettingsCheck = 0;

static std::wstring GameConfigPath() {
	PWSTR docs;
	std::wstring out;
	if (SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &docs) == S_OK) {
		wchar_t file[MAX_PATH];
		PathCombineW(file, docs, L"CAPCOM\\SUPERSTREETFIGHTERIV\\config.ini");
		out = file;
		CoTaskMemFree(docs);
	}
	return out;
}

// Value span of "key=value" at a line start; the game writes one key
// per line.
static bool FindIniValue(const std::string& content, const char* key, size_t* valueBegin, size_t* valueEnd) {
	std::string needle = std::string(key) + "=";
	size_t pos;
	if (content.compare(0, needle.size(), needle) == 0) {
		pos = 0;
	}
	else {
		pos = content.find("\n" + needle);
		if (pos == std::string::npos) {
			return false;
		}
		pos += 1;
	}
	*valueBegin = pos + needle.size();
	*valueEnd = content.find_first_of("\r\n", *valueBegin);
	if (*valueEnd == std::string::npos) {
		*valueEnd = content.size();
	}
	return true;
}

static void CheckGameSettings() {
	GameSettingsState prev = g_gameSettingsState;
	std::string content;
	std::wstring path = GameConfigPath();
	if (!path.empty()) {
		std::ifstream in(path.c_str(), std::ios::binary);
		if (in) {
			content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		}
	}
	if (content.empty()) {
		g_gameSettingsState = GS_UNKNOWN;
	}
	else {
		g_gameSettingsState = GS_READY;
		for (auto& setting : kRequiredGameSettings) {
			size_t b, e;
			if (!FindIniValue(content, setting.key, &b, &e) ||
				content.compare(b, e - b, setting.required) != 0) {
				g_gameSettingsState = GS_NEEDS_FIX;
				break;
			}
		}
	}
	if (g_gameSettingsState != prev) {
		spdlog::info(
			"Game settings check: {}",
			g_gameSettingsState == GS_READY ? "netplay ready" :
			g_gameSettingsState == GS_NEEDS_FIX ? "needs fixing" :
			"config.ini not found"
		);
	}
}

static void FixGameSettings() {
	std::wstring path = GameConfigPath();
	if (path.empty()) {
		return;
	}
	std::string content;
	{
		std::ifstream in(path.c_str(), std::ios::binary);
		if (!in) {
			return;
		}
		content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}
	for (auto& setting : kRequiredGameSettings) {
		size_t b, e;
		if (FindIniValue(content, setting.key, &b, &e)) {
			if (content.compare(b, e - b, setting.required) != 0) {
				spdlog::info(
					"Fixing game setting {}: {} -> {}",
					setting.key, content.substr(b, e - b), setting.required
				);
				content.replace(b, e - b, setting.required);
			}
		}
		else {
			// The game writes a full config; a missing key means
			// something unexpected- punt to the game's own options
			// instead of guessing where the line belongs.
			spdlog::warn("Game config has no {} line", setting.key);
			PushAlert(
				std::string("Couldn't fix ") + setting.key +
				"- set it in the game's PC graphic options"
			);
		}
	}
	// Players pin their config read-only to stop the game rewriting it
	// (a common community workaround for refresh-rate settings)- lift
	// the attribute for the write and put it back.
	DWORD attrs = GetFileAttributesW(path.c_str());
	bool bWasReadOnly =
		attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY);
	if (bWasReadOnly) {
		SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
	}
	bool bWrote = false;
	{
		std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
		if (out) {
			out.write(content.data(), (std::streamsize)content.size());
			bWrote = true;
		}
	}
	if (bWasReadOnly) {
		SetFileAttributesW(path.c_str(), attrs);
	}
	if (!bWrote) {
		spdlog::warn("Could not write the game config");
		PushAlert(
			"Couldn't write the game's config.ini- fix the settings in "
			"the game's PC options instead"
		);
	}
	g_nextGameSettingsCheck = 0;
}

static void DrawGameSettingsLight() {
	uint64_t now = GetTickCount64();
	if (now >= g_nextGameSettingsCheck) {
		g_nextGameSettingsCheck = now + 2000;
		CheckGameSettings();
	}

	ImU32 color =
		g_gameSettingsState == GS_READY ? IM_COL32(70, 220, 100, 255) :
		g_gameSettingsState == GS_NEEDS_FIX ? IM_COL32(235, 70, 60, 255) :
		IM_COL32(150, 150, 150, 255);
	float h = ImGui::GetTextLineHeight();
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::GetWindowDrawList()->AddCircleFilled(
		ImVec2(p.x + h * 0.5f, p.y + h * 0.6f), h * 0.3f, color
	);
	ImGui::Dummy(ImVec2(h, h));
	ImGui::SameLine();
	switch (g_gameSettingsState) {
	case GS_READY:
		ImGui::TextUnformatted("Game settings: netplay ready");
		break;
	case GS_NEEDS_FIX:
		ImGui::TextUnformatted("Game settings: fixed framerate + motion blur off required");
		ImGui::SameLine();
		if (ImGui::SmallButton("Fix")) {
			FixGameSettings();
		}
		break;
	default:
		ImGui::TextUnformatted("Game settings: config.ini not found (run USF4 once)");
		break;
	}
}

// -------------------------------------------------------------------
// Actions

static const int kDefaultServerPort = 23450;

// A bare hostname or IPv4 address (no colon anywhere) gets the default
// port appended, so "sf4.zak123.com" is enough at the login screen.
// The normalized form is written back into the field: the header, the
// saved config, and the --join-server handed to the launched game all
// expect host:port.
static void NormalizeServerAddr() {
	if (g_app.szServerAddr[0] == 0 || strchr(g_app.szServerAddr, ':') != nullptr) {
		return;
	}
	char full[sizeof(g_app.szServerAddr) + 8];
	snprintf(full, sizeof(full), "%s:%d", g_app.szServerAddr, kDefaultServerPort);
	strncpy_s(g_app.szServerAddr, full, _TRUNCATE);
}

static void ConnectToServer() {
	NormalizeServerAddr();
	SteamNetworkingIPAddr addr;
	if (!sf4e::Net::ResolveHostPort(g_app.szServerAddr, addr)) {
		PushAlert(
			"Could not find that server- the address should look like "
			"sf4.zak123.com or 203.0.113.7 (port optional, default 23450)"
		);
		return;
	}

	std::string name(g_app.szName);
	g_app.client.reset(new SessionClient(kCallbacks, std::string(""), g_app.ggpoPort, name));
	g_app.client->Connect(addr);

	// Snapshot reconciliation walks located game memory that only exists
	// inside the game process; it must stay off in this app.
	g_app.client->_snapshotsEnabled = false;

	g_app.chatLog.clear();
	g_app.bothReady = false;
	g_app.bLooking = false;
	g_app.szChallengeFrom[0] = 0;
	g_app.szChallengeTarget[0] = 0;
	g_app.nextListRefresh = 0;
	g_app.bVersionWarned = false;
	g_app.screen = SCREEN_SESSION;
	SaveConfig();
}

static void DropToLogin(const char* reason) {
	g_app.client.reset();
	g_app.screen = SCREEN_LOGIN;
	g_app.bothReady = false;
	g_app.rejoinAttempts = 0;
	g_app.bLooking = false;
	g_app.szChallengeFrom[0] = 0;
	g_app.szChallengeTarget[0] = 0;
	if (reason) {
		PushAlert(reason);
	}
}

static int MySide() {
	if (!g_app.client) {
		return -1;
	}
	std::vector<SessionProtocol::MemberData>& members = g_app.client->_lobbyData.members;
	for (int i = 0; i < (int)members.size(); i++) {
		if (members[i].connId == g_app.client->_cid) {
			return i;
		}
	}
	return -1;
}

static void SendChat(const std::string& channel) {
	if (g_app.szChatInput[0] == 0) {
		return;
	}
	g_app.client->Chat_Send(channel, std::string(g_app.szChatInput));
	g_app.szChatInput[0] = 0;
}

// Watch the launched game and, once it exits (match over, or it died),
// put the user back in their lobby for a rematch. Retries cover the
// window where the opponent's game still holds its seat.
static void TickGameWatch() {
	if (g_app.hGameProcess && WaitForSingleObject(g_app.hGameProcess, 0) == WAIT_OBJECT_0) {
		CloseHandle(g_app.hGameProcess);
		g_app.hGameProcess = NULL;
		g_app.gameLaunched = false;
		g_app.bothReady = false;
		// Closing the game is the normal end of every match- the app
		// visibly returning to the lobby is the whole message.
		spdlog::info("The game closed- re-seating into the lobby");
		g_app.rejoinAttempts = 5;
		g_app.nextRejoinAt = 0;
	}

	if (g_app.rejoinAttempts <= 0 || !g_app.client || g_app.client->_cid.user.empty()) {
		return;
	}

	if (!g_app.client->_lobbyData.id.key.empty()) {
		// Seated again- done retrying.
		g_app.rejoinAttempts = 0;
		return;
	}

	uint64_t now = GetTickCount64();
	if (now >= g_app.nextRejoinAt) {
		g_app.client->Lobby_Join(g_app.rejoinTarget);
		g_app.rejoinAttempts--;
		g_app.nextRejoinAt = now + 2000;
	}
}

// An unanswered incoming challenge quietly expires on this side too.
static void TickChallengeExpiry() {
	if (
		g_app.szChallengeFrom[0] != 0 &&
		GetTickCount64() - g_app.challengeReceivedAt > 30 * 1000
	) {
		g_app.szChallengeFrom[0] = 0;
	}
}

// -------------------------------------------------------------------
// UI
//
// Layout and flow borrow from +R's post-rollback lobby rework, the
// best-regarded lobby system in the genre: quick match is the front
// door (one open pool, one button), rooms show who's inside and who's
// playing at a glance, and player status is a colored dot rather than
// a sentence.

static const ImVec4 kAccent = ImVec4(0.89f, 0.23f, 0.18f, 1.00f);
static const ImVec4 kAccentHover = ImVec4(0.94f, 0.30f, 0.25f, 1.00f);
static const ImVec4 kAccentActive = ImVec4(0.75f, 0.18f, 0.14f, 1.00f);
static const ImU32 kDotLounge = IM_COL32(150, 150, 158, 255);
static const ImU32 kDotLooking = IM_COL32(80, 160, 255, 255);
static const ImU32 kDotInLobby = IM_COL32(235, 190, 60, 255);
static const ImU32 kDotInMatch = IM_COL32(235, 70, 60, 255);
static const ImU32 kDotReady = IM_COL32(70, 220, 100, 255);

static void SetupTheme() {
	ImGuiStyle& style = ImGui::GetStyle();
	ImGui::StyleColorsDark(&style);
	style.WindowRounding = 0.0f;
	style.ChildRounding = 6.0f;
	style.FrameRounding = 4.0f;
	style.PopupRounding = 4.0f;
	style.GrabRounding = 4.0f;
	style.TabRounding = 4.0f;
	style.WindowPadding = ImVec2(12, 12);
	style.FramePadding = ImVec2(8, 5);
	style.ItemSpacing = ImVec2(8, 6);
	style.ChildBorderSize = 1.0f;

	ImVec4* colors = style.Colors;
	colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
	colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
	colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
	colors[ImGuiCol_Border] = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
	colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
	colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
	colors[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.24f, 0.29f, 1.00f);
	colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
	colors[ImGuiCol_Button] = ImVec4(0.19f, 0.19f, 0.23f, 1.00f);
	colors[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.26f, 0.31f, 1.00f);
	colors[ImGuiCol_ButtonActive] = ImVec4(0.31f, 0.31f, 0.37f, 1.00f);
	colors[ImGuiCol_Header] = ImVec4(0.19f, 0.19f, 0.23f, 1.00f);
	colors[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.26f, 0.31f, 1.00f);
	colors[ImGuiCol_HeaderActive] = ImVec4(0.31f, 0.31f, 0.37f, 1.00f);
	colors[ImGuiCol_Tab] = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
	colors[ImGuiCol_TabHovered] = kAccentHover;
	colors[ImGuiCol_TabActive] = ImVec4(0.55f, 0.17f, 0.14f, 1.00f);
	colors[ImGuiCol_CheckMark] = kAccent;
	colors[ImGuiCol_SliderGrab] = kAccent;
	colors[ImGuiCol_SeparatorHovered] = kAccentHover;
	colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.12f, 0.12f, 0.14f, 0.60f);
	colors[ImGuiCol_TextSelectedBg] = ImVec4(0.89f, 0.23f, 0.18f, 0.35f);
	colors[ImGuiCol_NavHighlight] = kAccent;
}

// A filled status dot inline with text.
static void StatusDot(ImU32 color) {
	float h = ImGui::GetTextLineHeight();
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::GetWindowDrawList()->AddCircleFilled(
		ImVec2(p.x + h * 0.5f, p.y + h * 0.6f), h * 0.3f, color
	);
	ImGui::Dummy(ImVec2(h, h));
}

static ImU32 PresenceDotColor(SessionProtocol::PresenceStatus status) {
	switch (status) {
	case SessionProtocol::PS_LOOKING: return kDotLooking;
	case SessionProtocol::PS_IN_LOBBY: return kDotInLobby;
	case SessionProtocol::PS_IN_MATCH: return kDotInMatch;
	default: return kDotLounge;
	}
}

// Accent-styled primary action button.
static bool AccentButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
	ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive);
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
	bool pressed = ImGui::Button(label, size);
	ImGui::PopStyleColor(4);
	return pressed;
}

static const uint64_t ALERT_LIFETIME_MS = 8000;

static void DrawAlerts() {
	if (g_app.alerts.empty()) {
		g_app.alertFrontSince = 0;
		return;
	}

	// Alerts self-dismiss so a burst (a failed match can produce a few)
	// never turns into a click-through queue; OK just skips the wait.
	uint64_t now = GetTickCount64();
	if (g_app.alertFrontSince == 0) {
		g_app.alertFrontSince = now;
	}
	if (now - g_app.alertFrontSince > ALERT_LIFETIME_MS) {
		g_app.alerts.pop_front();
		g_app.alertFrontSince = 0;
		if (g_app.alerts.empty()) {
			return;
		}
		g_app.alertFrontSince = now;
	}

	ImU32 red = IM_COL32(255, 80, 80, 255);
	ImGui::PushStyleColor(ImGuiCol_Text, red);
	ImGui::TextWrapped("%s", g_app.alerts.front().c_str());
	ImGui::PopStyleColor();
	ImGui::SameLine();
	if (ImGui::SmallButton("OK")) {
		g_app.alerts.pop_front();
		g_app.alertFrontSince = 0;
	}
	ImGui::Separator();
}

static void DrawLoginScreen() {
	// A centered card- the front door should read like a game.
	ImVec2 avail = ImGui::GetContentRegionAvail();
	const float cardW = 400.0f;
	const float cardH = 330.0f;
	float x = (avail.x - cardW) * 0.5f;
	float y = avail.y * 0.16f;
	ImGui::SetCursorPosX(x > 0 ? x : 0);
	ImGui::SetCursorPosY(y > 16 ? y : 16);

	ImGui::BeginChild("login_card", ImVec2(cardW, cardH), true);
	ImGui::SetWindowFontScale(1.7f);
	ImGui::TextUnformatted("sf4e");
	ImGui::SetWindowFontScale(1.0f);
	ImGui::SameLine();
	ImGui::TextDisabled("v" SF4E_VERSION);
	ImGui::TextDisabled("Rollback netplay lobby for Ultra Street Fighter IV");
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	DrawAlerts();

	ImGui::TextUnformatted("Name");
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::InputText("##name", g_app.szName, sizeof(g_app.szName));
	ImGui::TextUnformatted("Server");
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::InputText("##server", g_app.szServerAddr, sizeof(g_app.szServerAddr));
	ImGui::Spacing();

	bool valid = g_app.szName[0] != 0;
	ImGui::BeginDisabled(!valid);
	if (AccentButton("CONNECT", ImVec2(-FLT_MIN, 34))) {
		ConnectToServer();
	}
	ImGui::EndDisabled();
	if (!valid) {
		ImGui::TextDisabled("Pick a name first");
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	DrawGameSettingsLight();
	ImGui::EndChild();
}

static void DrawLobbyBrowser() {
	SessionClient& c = *g_app.client;

	// The +R lesson: quick match is the front door- one open pool, one
	// button, state you can see.
	if (g_app.bLooking) {
		char szLabel[64];
		snprintf(szLabel, sizeof(szLabel), "SEARCHING (%d in queue) - tap to cancel", c._lookingCount);
		if (AccentButton(szLabel, ImVec2(-FLT_MIN, 36))) {
			g_app.bLooking = false;
			c.Matchmake_Set(false);
		}
	}
	else {
		if (ImGui::Button("FIND MATCH", ImVec2(-FLT_MIN, 36))) {
			g_app.bLooking = true;
			c.Matchmake_Set(true);
		}
	}
	ImGui::Spacing();

	ImGui::TextUnformatted("Lobbies");
	ImGui::SameLine();
	if (ImGui::SmallButton("Refresh")) {
		c.Lobby_RequestList();
	}

	if (ImGui::BeginTable("lobbies", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, 56.0f);
		ImGui::TableSetupColumn("##join", ImGuiTableColumnFlags_WidthFixed, 52.0f);
		for (int i = 0; i < (int)c._lobbyListing.size(); i++) {
			SessionProtocol::LobbyListEntry& entry = c._lobbyListing[i];
			bool full = entry.playerCount >= entry.capacity;
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(entry.name.c_str());
			// Rooms show who's inside at a glance (hover), like +R.
			if (ImGui::IsItemHovered() && !entry.players.empty()) {
				std::string who;
				for (size_t p = 0; p < entry.players.size(); p++) {
					if (p) {
						who += ", ";
					}
					who += entry.players[p];
				}
				ImGui::SetTooltip("%s", who.c_str());
			}
			ImGui::TableNextColumn();
			if (full) {
				ImGui::TextDisabled("%d/%d", entry.playerCount, entry.capacity);
			}
			else {
				ImGui::Text("%d/%d", entry.playerCount, entry.capacity);
			}
			ImGui::TableNextColumn();
			ImGui::BeginDisabled(full);
			ImGui::PushID(i);
			if (ImGui::SmallButton(full ? "Full" : "Join")) {
				c.Lobby_Join(entry.id);
			}
			ImGui::PopID();
			ImGui::EndDisabled();
		}
		ImGui::EndTable();
	}
	if (c._lobbyListing.empty()) {
		ImGui::TextDisabled("No open lobbies- FIND MATCH or create one.");
	}

	ImGui::Spacing();
	if (ImGui::CollapsingHeader("Create a lobby")) {
		ImGui::InputText("Lobby name", g_app.szNewLobbyName, sizeof(g_app.szNewLobbyName));
		ImGui::Combo("Rounds", &g_app.newLobbyRoundCountIdx, "1\0003\0005\0007\000");
		ImGui::Combo("Round time", &g_app.newLobbyRoundTimeIdx, "30\00060\00099\000");
		if (ImGui::Button("Create", ImVec2(120, 0))) {
			// Edition select is not offered here: the app pins every pick
			// to the USF4 edition, and Lobbyd's own lobbies (quickmatch,
			// challenges) run with edition select on- so created lobbies
			// pass true to match them.
			c.Lobby_Create(
				std::string(g_app.szNewLobbyName),
				true,
				kRoundCounts[g_app.newLobbyRoundCountIdx].first,
				kRoundTimes[g_app.newLobbyRoundTimeIdx].first
			);
		}
	}
}

// The character and stage combos list alphabetically, but the Roster
// tables are in game-ID order and must stay that way (the IDs are what
// the game consumes and what the config persists). displayOrder[row] =
// the game ID shown at that row; the combos convert on the way in/out.
static int g_charaDisplayOrder[Roster::NUM_CHARACTERS];
static int g_stageDisplayOrder[Roster::NUM_STAGES];

static void InitDisplayOrders() {
	for (int i = 0; i < Roster::NUM_CHARACTERS; i++) {
		g_charaDisplayOrder[i] = i;
	}
	for (int i = 0; i < Roster::NUM_STAGES; i++) {
		g_stageDisplayOrder[i] = i;
	}
	std::sort(g_charaDisplayOrder, g_charaDisplayOrder + Roster::NUM_CHARACTERS, [](int a, int b) {
		return strcmp(Roster::characterNames[a], Roster::characterNames[b]) < 0;
	});
	std::sort(g_stageDisplayOrder, g_stageDisplayOrder + Roster::NUM_STAGES, [](int a, int b) {
		return strcmp(Roster::stageNames[a], Roster::stageNames[b]) < 0;
	});
}

static const char* CharaRowName(void*, int row) {
	return Roster::characterNames[g_charaDisplayOrder[row]];
}

static const char* StageRowName(void*, int row) {
	return Roster::stageNames[g_stageDisplayOrder[row]];
}

// Draws a combo over displayOrder rows while *id stays a game ID.
static void DrawSortedCombo(const char* label, int* id, int* displayOrder, const char* (*rowName)(void*, int), int count) {
	int row = 0;
	for (int i = 0; i < count; i++) {
		if (displayOrder[i] == *id) {
			row = i;
			break;
		}
	}
	ImGui::Combo(label, &row, rowName, nullptr, count);
	*id = displayOrder[row];
}

// Named pick widgets, backed by the Roster tables. These write the
// raw bytes the game consumes (docs/roster-names-research.md §4):
// everything 0-based; ultra 0/1/2 = U1/U2/W. Out-of-range persisted
// values are healed here before READY can send them.
static void DrawPickCombos() {
	rVsMode::ConfirmedCharaConditions& cond = g_app.myConditions;
	int charaID = g_app.myCharaID;

	{
		char u1[64], u2[64];
		snprintf(u1, sizeof(u1), "U1: %s", Roster::ultra1Names[charaID]);
		snprintf(u2, sizeof(u2), "U2: %s", Roster::ultra2Names[charaID]);
		const char* items[3] = { u1, u2, "W: both (reduced damage)" };
		int sel = cond.ultraCombo < 3 ? cond.ultraCombo : 0;
		ImGui::Combo("Ultra", &sel, items, 3);
		cond.ultraCombo = (BYTE)sel;
	}

	int costumeCount = Roster::costumeCounts[charaID];
	{
		// Slot order: Default, Alt 1..N, then the 2014 trio- always
		// Vacation, Wild, Horror, and always the last three slots.
		char alt[3][8];
		const char* items[7];
		int n = 0;
		items[n++] = "Default";
		for (int a = 1; a <= costumeCount - 4; a++) {
			snprintf(alt[a - 1], sizeof(alt[0]), "Alt %d", a);
			items[n++] = alt[a - 1];
		}
		items[n++] = "Vacation";
		items[n++] = "Wild";
		items[n++] = "Horror";
		int sel = cond.costume < costumeCount ? cond.costume : 0;
		ImGui::Combo("Costume", &sel, items, costumeCount);
		cond.costume = (BYTE)sel;
	}

	{
		// Every costume has colors 1-10 plus two stylized takes on
		// color 1; the 2014 trio adds 13-22.
		static char labels[22][16];
		static const char* items[22];
		if (!items[0]) {
			for (int i = 0; i < 22; i++) {
				if (i == 10 || i == 11) {
					snprintf(labels[i], sizeof(labels[0]), "%d (stylized)", i + 1);
				}
				else {
					snprintf(labels[i], sizeof(labels[0]), "%d", i + 1);
				}
				items[i] = labels[i];
			}
		}
		bool has2014Colors = cond.costume >= costumeCount - 3;
		int colorCount = has2014Colors ? 22 : 12;
		int sel = cond.color < colorCount ? cond.color : 0;
		ImGui::Combo("Color", &sel, items, colorCount);
		cond.color = (BYTE)sel;
	}
}

static void DrawLobbyPanel() {
	SessionClient& c = *g_app.client;
	std::vector<SessionProtocol::MemberData>& members = c._lobbyData.members;
	int mySide = MySide();

	// Room header, +R player-room style: name, rules, then the seats.
	ImGui::SetWindowFontScale(1.15f);
	ImGui::TextUnformatted(c._lobbyData.name.c_str());
	ImGui::SetWindowFontScale(1.0f);
	ImGui::TextDisabled(
		"%d rounds / %ds",
		c._lobbyData.roundCount,
		(int)c._lobbyData.roundTime.integral
	);
	ImGui::Spacing();

	// The room always answers "who is playing right now?"
	if (g_app.bothReady && members.size() >= 2) {
		ImGui::BeginChild("now_playing", ImVec2(0, 46), true);
		StatusDot(kDotInMatch);
		ImGui::SameLine();
		ImGui::Text("NOW PLAYING  %s  vs  %s", members[0].name.c_str(), members[1].name.c_str());
		ImGui::TextDisabled(
			g_app.gameLaunched
			? "Press a button in-game to bind your controller."
			: "Waiting for the game to launch..."
		);
		ImGui::EndChild();
		ImGui::Spacing();
	}

	// Seat slots.
	for (int i = 0; i < 2; i++) {
		ImGui::PushID(i);
		ImGui::BeginChild("seat", ImVec2(0, 46), true);
		if (i < (int)members.size()) {
			bool isMe = members[i].connId == c._cid;
			bool ready = c._matchData.readyMessageNum[i] > -1;
			StatusDot(ready ? kDotReady : kDotInLobby);
			ImGui::SameLine();
			ImGui::Text("P%d  %s", i + 1, members[i].name.c_str());
			if (isMe) {
				ImGui::SameLine();
				ImGui::TextColored(kAccent, "(you)");
			}
			ImGui::SameLine();
			ImGui::TextDisabled(ready ? "- ready" : "- picking");
		}
		else {
			StatusDot(kDotLounge);
			ImGui::SameLine();
			ImGui::TextDisabled("P%d  waiting for opponent...", i + 1);
		}
		ImGui::EndChild();
		ImGui::PopID();
	}
	ImGui::Spacing();

	if (!g_app.bothReady) {
		if (mySide < 0 || mySide > 1) {
			ImGui::TextDisabled("Watching- the seats are taken.");
		}
		else if (c._outstandingReadyRequestNumber > -1) {
			ImGui::TextUnformatted("Waiting for server...");
		}
		else if (c._matchData.readyMessageNum[mySide] > -1) {
			ImGui::TextUnformatted("Ready! Waiting for your opponent.");
		}
		else {
			DrawSortedCombo("Character", &g_app.myCharaID, g_charaDisplayOrder, CharaRowName, Roster::NUM_CHARACTERS);
			DrawPickCombos();
			if (mySide == 0) {
				DrawSortedCombo("Stage", &g_app.stageID, g_stageDisplayOrder, StageRowName, Roster::NUM_STAGES);
			}
			if (ImGui::Checkbox("Route through relay", &g_app.bForceRelay)) {
				SaveConfig();
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Send this match's traffic through the server instead of\n"
					"connecting directly. Turn it on if matches keep timing out\n"
					"with \"could not reach the opponent\"- either player setting\n"
					"it is enough for both. Adds the server's ping to the match."
				);
			}
			ImGui::Spacing();
			if (AccentButton("READY", ImVec2(-FLT_MIN, 34))) {
				g_app.myConditions.charaID = g_app.myCharaID;
				SaveConfig();
				bool ok = c.PreBattle_SetChara(g_app.myConditions) == k_EResultOK;
				if (ok && mySide == 0) {
					ok = ok && c.PreBattle_SetEnv(g_app.rand()) == k_EResultOK;
					ok = ok && c.PreBattle_SetStage(g_app.stageID) == k_EResultOK;
				}
				if (ok) {
					c.Lobby_Ready();
				}
				else {
					PushAlert("Could not send match setup");
				}
			}
		}
	}

	ImGui::Spacing();
	if (ImGui::Button("Leave lobby")) {
		c.Lobby_Leave();
		g_app.bothReady = false;
		// Leaving on purpose also cancels any pending auto-rejoin.
		g_app.rejoinAttempts = 0;
		g_app.rejoinTarget = { "", "" };
	}
}

static const char* PresenceStatusLabel(SessionProtocol::PresenceStatus status) {
	switch (status) {
	case SessionProtocol::PS_LOOKING: return "looking";
	case SessionProtocol::PS_IN_LOBBY: return "in lobby";
	case SessionProtocol::PS_IN_MATCH: return "in match";
	default: return "lounge";
	}
}

static void DrawPlayersPane() {
	SessionClient& c = *g_app.client;
	bool inLounge = c._lobbyData.id.key.empty();

	ImGui::Text("Online - %d", (int)c._presence.size());
	ImGui::Separator();

	ImGui::BeginChild("players_scroll", ImVec2(0, 0));
	for (int i = 0; i < (int)c._presence.size(); i++) {
		SessionProtocol::PresenceEntry& entry = c._presence[i];
		bool isMe = entry.name == c._name;
		bool challengeable =
			!isMe &&
			inLounge &&
			(entry.status == SessionProtocol::PS_LOUNGE || entry.status == SessionProtocol::PS_LOOKING);

		ImGui::PushID(i);
		StatusDot(PresenceDotColor(entry.status));
		ImGui::SameLine();
		if (isMe) {
			ImGui::Text("%s", entry.name.c_str());
			ImGui::SameLine();
			ImGui::TextColored(kAccent, "(you)");
		}
		else {
			ImGui::TextUnformatted(entry.name.c_str());
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", PresenceStatusLabel(entry.status));
		}
		if (challengeable) {
			ImGui::SameLine(ImGui::GetContentRegionAvail().x - 24.0f);
			if (ImGui::SmallButton("vs")) {
				if (c.Challenge_Send(entry.name) == k_EResultOK) {
					// The challenge corner shows the outgoing state;
					// no alert needed.
					strncpy_s(g_app.szChallengeTarget, entry.name.c_str(), _TRUNCATE);
				}
			}
		}
		ImGui::PopID();
	}
	ImGui::EndChild();
}

static void DrawChatPane() {
	SessionClient& c = *g_app.client;
	bool inLobby = !c._lobbyData.id.key.empty();

	static std::string activeChannel = SessionProtocol::CHAT_CHANNEL_LOUNGE;
	if (!inLobby) {
		activeChannel = SessionProtocol::CHAT_CHANNEL_LOUNGE;
	}

	if (ImGui::BeginTabBar("chat_tabs")) {
		if (ImGui::BeginTabItem("Lounge")) {
			activeChannel = SessionProtocol::CHAT_CHANNEL_LOUNGE;
			ImGui::EndTabItem();
		}
		if (inLobby && ImGui::BeginTabItem("Lobby")) {
			activeChannel = c._lobbyData.id.key;
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	float footer = ImGui::GetFrameHeightWithSpacing();
	ImGui::BeginChild("chat_scroll", ImVec2(0, -footer), true);
	for (auto iter = g_app.chatLog.begin(); iter != g_app.chatLog.end(); iter++) {
		if (iter->channel != activeChannel) {
			continue;
		}
		time_t ts = (time_t)iter->ts;
		struct tm tmbuf;
		char szTime[8] = "??:??";
		if (localtime_s(&tmbuf, &ts) == 0) {
			strftime(szTime, sizeof(szTime), "%H:%M", &tmbuf);
		}
		ImGui::TextWrapped("[%s] %s: %s", szTime, iter->from.c_str(), iter->text.c_str());
	}
	if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
		ImGui::SetScrollHereY(1.0f);
	}
	ImGui::EndChild();

	bool send = false;
	ImGui::SetNextItemWidth(-70.0f);
	if (ImGui::InputText("##chat_input", g_app.szChatInput, sizeof(g_app.szChatInput), ImGuiInputTextFlags_EnterReturnsTrue)) {
		send = true;
		ImGui::SetKeyboardFocusHere(-1);
	}
	ImGui::SameLine();
	if (ImGui::Button("Send", ImVec2(60, 0))) {
		send = true;
	}
	if (send) {
		SendChat(activeChannel);
	}
}

// The challenge corner, bottom-left. It always exists- an empty state
// tells players where challenges will land before the first one ever
// arrives- and an incoming challenge turns it into the accept prompt.
static void DrawChallengeBox() {
	SessionClient& c = *g_app.client;

	ImGui::TextUnformatted("Challenges");
	ImGui::SameLine(ImGui::GetContentRegionMax().x - 64.0f);
	if (ImGui::Checkbox("Mute", &g_app.bMuted)) {
		SaveConfig();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Silence the notification sound- the taskbar still flashes");
	}
	ImGui::Separator();

	if (g_app.szChallengeFrom[0] != 0) {
		int secondsLeft = (int)(30 - (GetTickCount64() - g_app.challengeReceivedAt) / 1000);
		if (secondsLeft < 0) {
			secondsLeft = 0;
		}
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 60, 255));
		ImGui::Text("%s wants to fight!", g_app.szChallengeFrom);
		ImGui::PopStyleColor();
		if (AccentButton("Accept", ImVec2(110, 0))) {
			c.Challenge_Answer(std::string(g_app.szChallengeFrom), true);
			g_app.szChallengeFrom[0] = 0;
			return;
		}
		ImGui::SameLine();
		if (ImGui::Button("Decline", ImVec2(80, 0))) {
			c.Challenge_Answer(std::string(g_app.szChallengeFrom), false);
			g_app.szChallengeFrom[0] = 0;
			return;
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%ds", secondsLeft);
		return;
	}

	if (g_app.szChallengeTarget[0] != 0) {
		ImGui::Text("Challenge sent to %s", g_app.szChallengeTarget);
		ImGui::TextDisabled("Waiting for an answer...");
		return;
	}

	ImGui::TextDisabled("No challenges right now.");
	ImGui::TextDisabled("Hit \"vs\" on a player to send one.");
}

static void DrawSessionScreen() {
	SessionClient& c = *g_app.client;
	bool inLobby = !c._lobbyData.id.key.empty();

	// Until the server assigns a connection ID, the handshake (and
	// registration right behind it) is still in flight- sending lobby
	// requests now would just be dropped as unregistered.
	if (c._cid.user.empty()) {
		DrawAlerts();
		ImGui::TextUnformatted("Connecting...");
		if (ImGui::Button("Cancel")) {
			DropToLogin(nullptr);
		}
		return;
	}

	// The handshake landed: if the server build differs from ours, say
	// so once as an alert, and keep a tag in the header for the rest of
	// the session. An empty server version is itself an old server.
	bool bVersionMismatch = c._serverVersion != SF4E_VERSION;
	if (!g_app.bVersionWarned) {
		g_app.bVersionWarned = true;
		if (bVersionMismatch) {
			PushAlert(
				c._serverVersion.empty()
				? "This app is newer than the server- versions should match. Check the releases page."
				: "Version mismatch: the server runs v" + c._serverVersion +
				", this app is v" SF4E_VERSION ". Get the matching build from the releases page."
			);
		}
	}

	// Header strip: who you are, where you are, how the connection
	// feels (+R shows ping up front), and the settings light. The
	// default port is dropped from the display- "@ sf4.zak123.com"
	// reads nicer- but a nonstandard port stays visible because it's
	// exactly what someone on a custom server needs to see.
	char displayAddr[sizeof(g_app.szServerAddr)];
	strncpy_s(displayAddr, g_app.szServerAddr, _TRUNCATE);
	{
		char defaultSuffix[16];
		snprintf(defaultSuffix, sizeof(defaultSuffix), ":%d", kDefaultServerPort);
		size_t addrLen = strlen(displayAddr);
		size_t sufLen = strlen(defaultSuffix);
		if (addrLen > sufLen && strcmp(displayAddr + addrLen - sufLen, defaultSuffix) == 0) {
			displayAddr[addrLen - sufLen] = 0;
		}
	}
	ImGui::Text("%s", c._name.c_str());
	ImGui::SameLine();
	int ping = c.GetPingMs();
	if (ping >= 0) {
		ImGui::TextDisabled("@ %s  |  %d ms", displayAddr, ping);
		if (ImGui::IsItemHovered()) {
			// The relay runs on the same host as the lobby server, so
			// this one number covers both roles.
			ImGui::SetTooltip(
				"Ping to the lobby server.\n"
				"Matches normally connect straight to your opponent, but when\n"
				"that isn't possible (strict NAT), match traffic is relayed\n"
				"through this server- so this ping matters for relayed matches."
			);
		}
	}
	else {
		ImGui::TextDisabled("@ %s", displayAddr);
	}
	if (bVersionMismatch) {
		ImGui::SameLine(0.0f, 24.0f);
		ImGui::TextColored(ImVec4(0.92f, 0.78f, 0.25f, 1.0f), "version mismatch");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Server: %s\nThis app: %s",
				c._serverVersion.empty() ? "(older than " SF4E_VERSION ")" : c._serverVersion.c_str(),
				SF4E_VERSION
			);
		}
	}
	ImGui::SameLine(0.0f, 24.0f);
	DrawGameSettingsLight();
	ImGui::SameLine(ImGui::GetContentRegionMax().x - 84.0f);
	if (ImGui::SmallButton("Disconnect")) {
		DropToLogin(nullptr);
		return;
	}
	ImGui::Separator();
	DrawAlerts();

	// Seated anywhere means the server dropped us from the quickmatch
	// queue; mirror that.
	if (inLobby) {
		g_app.bLooking = false;
	}

	// Poll presence always, and the lobby list while browsing.
	{
		uint64_t now = GetTickCount64();
		if (now >= g_app.nextListRefresh) {
			c.Presence_RequestList();
			if (!inLobby) {
				c.Lobby_RequestList();
			}
			g_app.nextListRefresh = now + LOBBY_LIST_REFRESH_MS;
		}
	}

	// Left column: the lobby zone on top, the challenge corner pinned
	// to the bottom- otherwise-unused space that challenges can claim
	// without shoving the rest of the layout around.
	const float challengeBoxH = 98.0f;
	ImGui::BeginChild("left_col", ImVec2(360, 0), false);
	{
		float spacing = ImGui::GetStyle().ItemSpacing.y;
		ImGui::BeginChild("left_pane", ImVec2(0, -(challengeBoxH + spacing)), true);
		if (inLobby) {
			DrawLobbyPanel();
		}
		else {
			DrawLobbyBrowser();
		}
		ImGui::EndChild();
		ImGui::BeginChild("challenge_box", ImVec2(0, challengeBoxH), true);
		DrawChallengeBox();
		ImGui::EndChild();
	}
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("chat_pane", ImVec2(-230, 0), true);
	DrawChatPane();
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("players_pane", ImVec2(0, 0), true);
	DrawPlayersPane();
	ImGui::EndChild();
}

// -------------------------------------------------------------------
// Logging

static void SetUpLogging(bool bShowConsole) {
	std::vector<spdlog::sink_ptr> sinks;

	PWSTR path;
	if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &path) == S_OK) {
		try {
			// Per-process filename: two instances on one machine (the
			// standard local test setup) otherwise truncate each
			// other's logs through rotate-on-open.
			wchar_t logname[64];
			swprintf_s(logname, L"sf4e/logs/LobbyClient-%u.log", GetCurrentProcessId());
			wchar_t logpath[MAX_PATH];
			PathCombineW(logpath, path, logname);
			sinks.push_back(std::shared_ptr<spdlog::sinks::rotating_file_sink_mt>(
				new spdlog::sinks::rotating_file_sink_mt(logpath, 1048576 * 5, 3, true)
			));
		}
		catch (const spdlog::spdlog_ex&) {
			// Logging is best-effort; run silent if the file sink fails.
		}
		CoTaskMemFree(path);
	}

	if (bShowConsole && AllocConsole()) {
		sinks.push_back(std::shared_ptr<spdlog::sinks::wincolor_stdout_sink_mt>(
			new spdlog::sinks::wincolor_stdout_sink_mt()
		));
	}

	std::shared_ptr<spdlog::logger> logger(new spdlog::logger("LobbyClient", sinks.begin(), sinks.end()));
	spdlog::set_default_logger(logger);
	spdlog::flush_on(spdlog::level::info);
	spdlog::info("Welcome to the sf4e lobby client");
}

// -------------------------------------------------------------------

int WINAPI wWinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR lpCmdLine,
	_In_ int nShowCmd
) {
	bool bShowConsole = false;
	std::string autoConnect;
	std::string autoName;

	int nGgpoPort = 23457;
	CLI::App app("sf4e lobby client", "LobbyClient");
	app.add_flag("--console", bShowConsole, "Show a console with live logging");
	app.add_option("--connect", autoConnect, "Connect to this server (ip:port) on launch");
	app.add_option("--name", autoName, "Name to connect with on launch");
	app.add_option("--ggpo-port", nGgpoPort, "Local UDP port the launched game uses for GGPO traffic")->check(CLI::Range(1024, 65535));
	int argc;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	CLI11_PARSE(app, argc, argv);

	SetUpLogging(bShowConsole);
	InitDisplayOrders();
	LoadConfig();

	SteamDatagramErrMsg errMsg;
	if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
		spdlog::error("GameNetworkingSockets_Init failed: {}", errMsg);
		MessageBoxA(NULL, "Could not initialize networking!", NULL, MB_OK);
		return 1;
	}

	g_app.ggpoPort = (uint16_t)nGgpoPort;
	if (!autoName.empty()) {
		strncpy_s(g_app.szName, autoName.c_str(), _TRUNCATE);
	}
	if (!autoConnect.empty()) {
		strncpy_s(g_app.szServerAddr, autoConnect.c_str(), _TRUNCATE);
		if (g_app.szName[0] != 0) {
			ConnectToServer();
		}
	}

	ImGui_ImplWin32_EnableDpiAwareness();
	WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"sf4e lobby client", nullptr };
	::RegisterClassExW(&wc);
	HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"sf4e lobby", WS_OVERLAPPEDWINDOW, 100, 100, 1024, 640, nullptr, nullptr, wc.hInstance, nullptr);
	g_hWnd = hwnd;

	if (!CreateDeviceD3D(hwnd))
	{
		spdlog::error("CreateDeviceD3D failed");
		CleanupDeviceD3D();
		::UnregisterClassW(wc.lpszClassName, wc.hInstance);
		return 1;
	}
	spdlog::info("D3D device created");

	::ShowWindow(hwnd, SW_SHOWDEFAULT);
	::UpdateWindow(hwnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	SetupTheme();

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX9_Init(g_pd3dDevice);

	ImVec4 clear_color = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);

	bool done = false;
	while (!done)
	{
		// Network pump: process GNS callbacks, then the client. A failed
		// Step means the connection is gone- drop back to the login
		// screen.
		if (g_app.client) {
			g_app.client->PrepareForCallbacks();
		}
		SteamNetworkingSockets()->RunCallbacks();
		if (g_app.client) {
			if (g_app.client->Step() != 0) {
				DropToLogin("Disconnected from server");
			}
		}
		TickGameWatch();
		TickChallengeExpiry();

		MSG msg;
		while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
			if (msg.message == WM_QUIT)
				done = true;
		}
		if (done)
			break;

		if (g_DeviceLost)
		{
			// The device is lost for the whole match while SF4 holds
			// exclusive fullscreen; the entry and recovery are logged
			// once each (below and at "Present reported device lost"),
			// not every frame- that spam was burying useful lines.
			HRESULT hr = g_pd3dDevice->TestCooperativeLevel();
			if (hr == D3DERR_DEVICELOST)
			{
				::Sleep(10);
				continue;
			}
			if (hr == D3DERR_DEVICENOTRESET)
				ResetDevice();
			g_DeviceLost = false;
			spdlog::info("D3D device recovered");
		}

		if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
		{
			g_d3dpp.BackBufferWidth = g_ResizeWidth;
			g_d3dpp.BackBufferHeight = g_ResizeHeight;
			g_ResizeWidth = g_ResizeHeight = 0;
			ResetDevice();
		}

		ImGui_ImplDX9_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		// One fullscreen host window.
		ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(viewport->WorkSize);
		ImGui::Begin(
			"##main",
			nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus
		);
		if (g_app.screen == SCREEN_LOGIN || !g_app.client) {
			DrawLoginScreen();
		}
		else {
			DrawSessionScreen();
		}
		ImGui::End();

		ImGui::EndFrame();
		g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
		g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		g_pd3dDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		D3DCOLOR clear_col_dx = D3DCOLOR_RGBA((int)(clear_color.x * clear_color.w * 255.0f), (int)(clear_color.y * clear_color.w * 255.0f), (int)(clear_color.z * clear_color.w * 255.0f), (int)(clear_color.w * 255.0f));
		g_pd3dDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clear_col_dx, 1.0f, 0);
		if (g_pd3dDevice->BeginScene() >= 0)
		{
			ImGui::Render();
			ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
			g_pd3dDevice->EndScene();
		}
		HRESULT result = g_pd3dDevice->Present(nullptr, nullptr, nullptr, nullptr);
		if (result == D3DERR_DEVICELOST && !g_DeviceLost)
		{
			g_DeviceLost = true;
			spdlog::warn("Present reported device lost");
		}
	}

	ImGui_ImplDX9_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	CleanupDeviceD3D();
	::DestroyWindow(hwnd);
	::UnregisterClassW(wc.lpszClassName, wc.hInstance);

	g_app.client.reset();
	if (g_app.hGameProcess) {
		CloseHandle(g_app.hGameProcess);
		g_app.hGameProcess = NULL;
	}
	GameNetworkingSockets_Kill();
	spdlog::shutdown();

	return 0;
}

// -------------------------------------------------------------------
// D3D9 window skeleton, from the stock Dear ImGui example

bool CreateDeviceD3D(HWND hWnd)
{
	if ((g_pD3D = Direct3DCreate9(D3D_SDK_VERSION)) == nullptr)
		return false;

	ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
	g_d3dpp.Windowed = TRUE;
	g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	g_d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
	g_d3dpp.EnableAutoDepthStencil = TRUE;
	g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
	g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
	if (g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_d3dpp, &g_pd3dDevice) < 0)
		return false;

	return true;
}

void CleanupDeviceD3D()
{
	if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
	if (g_pD3D) { g_pD3D->Release(); g_pD3D = nullptr; }
}

void ResetDevice()
{
	ImGui_ImplDX9_InvalidateDeviceObjects();
	HRESULT hr = g_pd3dDevice->Reset(&g_d3dpp);
	if (hr == D3DERR_INVALIDCALL)
		IM_ASSERT(0);
	ImGui_ImplDX9_CreateDeviceObjects();
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return true;

	switch (msg)
	{
	case WM_SIZE:
		if (wParam == SIZE_MINIMIZED)
			return 0;
		g_ResizeWidth = (UINT)LOWORD(lParam);
		g_ResizeHeight = (UINT)HIWORD(lParam);
		return 0;
	case WM_SYSCOMMAND:
		if ((wParam & 0xfff0) == SC_KEYMENU)
			return 0;
		break;
	case WM_DESTROY:
		::PostQuitMessage(0);
		return 0;
	}
	return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
