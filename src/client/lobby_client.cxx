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

#include <deque>
#include <memory>
#include <random>
#include <string>
#include <time.h>

#include <windows.h>
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
	char szServerAddr[64] = "127.0.0.1:23450";
	char szChatInput[SessionProtocol::CHAT_TEXT_MAX] = { 0 };
	char szNewLobbyName[64] = { 0 };
	int newLobbyRoundCountIdx = 1;
	int newLobbyRoundTimeIdx = 2;
	bool newLobbyEditionSelect = true;

	rVsMode::ConfirmedCharaConditions myConditions;
	int myCharaID = 0;
	int stageID = 0;

	std::deque<SessionProtocol::ChatEvent> chatLog;
	std::deque<std::string> alerts;
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
// Session client callbacks

static void OnError(SessionClient::ErrorType errType, SessionClient* const c, const SessionClient::Callbacks& callbacks) {
	switch (errType) {
	case SessionClient::SCE_JOIN_REJECTED_HASH_INVALID:
		g_app.alerts.push_back("Rejected by server: version mismatch");
		break;
	case SessionClient::SCE_JOIN_REJECTED_NAME_TAKEN:
		g_app.alerts.push_back("Rejected by server: that name is taken");
		break;
	case SessionClient::SCE_JOIN_REJECTED_LOBBY_FULL:
		g_app.alerts.push_back("Could not join: lobby is full");
		break;
	case SessionClient::SCE_JOIN_REJECTED_NO_SUCH_LOBBY:
		g_app.alerts.push_back("Could not join: lobby no longer exists");
		// No sense retrying a rejoin into a dead lobby.
		g_app.rejoinAttempts = 0;
		break;
	case SessionClient::SCE_JOIN_REJECTED_ALREADY_IN_LOBBY:
		g_app.alerts.push_back("Could not join: already in a lobby");
		break;
	case SessionClient::SCE_JOIN_REJECTED_REQUEST_INVALID:
		g_app.alerts.push_back("Rejected by server: bad request- version mismatch?");
		break;
	case SessionClient::SCE_JOIN_REJECTED_SERVER_FULL:
		g_app.alerts.push_back("The server is full- try again later");
		break;
	default:
		g_app.alerts.push_back("Unknown session error");
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
		g_app.alerts.push_back("Could not create lobby");
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
		g_app.alerts.push_back("Match is ready, but Launcher.exe isn't next to this app!");
		return;
	}

	char cmdA[1024];
	snprintf(
		cmdA,
		sizeof(cmdA),
		"\"%ws\" --join-server \"%s\" --join-lobby-host \"%s\" --join-lobby-key \"%s\" --join-token \"%s\" --join-name \"%s\" --ggpo-port %u",
		szLauncher,
		g_app.szServerAddr,
		handoff.lobby.host.c_str(),
		handoff.lobby.key.c_str(),
		handoff.token.c_str(),
		c->_name.c_str(),
		(unsigned)g_app.ggpoPort
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
		g_app.alerts.push_back("Match ready- launching the game! Press a button in-game to bind your controller.");
		spdlog::info("Launched game for lobby {} handoff", handoff.lobby.key);
	}
	else {
		g_app.alerts.push_back("Match is ready, but the game could not be launched");
		spdlog::error("CreateProcess for Launcher.exe failed: {}", GetLastError());
	}
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
};

// -------------------------------------------------------------------
// Actions

static void ConnectToServer() {
	SteamNetworkingIPAddr addr;
	addr.Clear();
	if (!addr.ParseString(g_app.szServerAddr)) {
		g_app.alerts.push_back("Server address must look like ip:port");
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
	g_app.nextListRefresh = 0;
	g_app.screen = SCREEN_SESSION;
}

static void DropToLogin(const char* reason) {
	g_app.client.reset();
	g_app.screen = SCREEN_LOGIN;
	g_app.bothReady = false;
	g_app.rejoinAttempts = 0;
	if (reason) {
		g_app.alerts.push_back(reason);
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
		g_app.alerts.push_back("The game closed- returning you to the lobby.");
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

// -------------------------------------------------------------------
// UI

static void DrawAlerts() {
	if (g_app.alerts.empty()) {
		return;
	}
	ImU32 red = IM_COL32(255, 80, 80, 255);
	ImGui::PushStyleColor(ImGuiCol_Text, red);
	ImGui::TextWrapped("%s", g_app.alerts.front().c_str());
	ImGui::PopStyleColor();
	ImGui::SameLine();
	if (ImGui::SmallButton("OK")) {
		g_app.alerts.pop_front();
	}
	ImGui::Separator();
}

static void DrawLoginScreen() {
	ImGui::TextUnformatted("sf4e lobby");
	ImGui::Separator();
	DrawAlerts();

	ImGui::InputText("Name", g_app.szName, sizeof(g_app.szName));
	ImGui::InputText("Server", g_app.szServerAddr, sizeof(g_app.szServerAddr));

	bool valid = g_app.szName[0] != 0;
	ImGui::BeginDisabled(!valid);
	if (ImGui::Button("Connect", ImVec2(120, 0))) {
		ConnectToServer();
	}
	ImGui::EndDisabled();
	if (!valid) {
		ImGui::TextDisabled("Pick a name first");
	}
}

static void DrawLobbyBrowser() {
	SessionClient& c = *g_app.client;

	ImGui::TextUnformatted("Lobbies");
	ImGui::SameLine();
	if (ImGui::SmallButton("Refresh")) {
		c.Lobby_RequestList();
	}

	if (ImGui::BeginTable("lobbies", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, 60.0f);
		ImGui::TableSetupColumn("##join", ImGuiTableColumnFlags_WidthFixed, 50.0f);
		for (int i = 0; i < (int)c._lobbyListing.size(); i++) {
			SessionProtocol::LobbyListEntry& entry = c._lobbyListing[i];
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(entry.name.c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%d/%d", entry.playerCount, entry.capacity);
			ImGui::TableNextColumn();
			bool full = entry.playerCount >= entry.capacity;
			ImGui::BeginDisabled(full);
			ImGui::PushID(i);
			if (ImGui::SmallButton("Join")) {
				c.Lobby_Join(entry.id);
			}
			ImGui::PopID();
			ImGui::EndDisabled();
		}
		ImGui::EndTable();
	}
	if (c._lobbyListing.empty()) {
		ImGui::TextDisabled("No open lobbies- create one!");
	}

	ImGui::Separator();
	ImGui::TextUnformatted("Create a lobby");
	ImGui::InputText("Lobby name", g_app.szNewLobbyName, sizeof(g_app.szNewLobbyName));
	ImGui::Combo("Rounds", &g_app.newLobbyRoundCountIdx, "1\0003\0005\0007\000");
	ImGui::Combo("Round time", &g_app.newLobbyRoundTimeIdx, "30\00060\00099\000");
	ImGui::Checkbox("Edition select", &g_app.newLobbyEditionSelect);
	if (ImGui::Button("Create")) {
		c.Lobby_Create(
			std::string(g_app.szNewLobbyName),
			g_app.newLobbyEditionSelect,
			kRoundCounts[g_app.newLobbyRoundCountIdx].first,
			kRoundTimes[g_app.newLobbyRoundTimeIdx].first
		);
	}
}

static void DrawLobbyPanel() {
	SessionClient& c = *g_app.client;
	std::vector<SessionProtocol::MemberData>& members = c._lobbyData.members;
	int mySide = MySide();

	ImGui::Text("Lobby: %s", c._lobbyData.name.c_str());
	ImGui::TextDisabled(
		"%d rounds / %ds / edition select %s",
		c._lobbyData.roundCount,
		(int)c._lobbyData.roundTime.integral,
		c._lobbyData.editionSelect ? "on" : "off"
	);
	if (ImGui::Button("Leave lobby")) {
		c.Lobby_Leave();
		g_app.bothReady = false;
		// Leaving on purpose also cancels any pending auto-rejoin.
		g_app.rejoinAttempts = 0;
		g_app.rejoinTarget = { "", "" };
		return;
	}
	ImGui::Separator();

	for (int i = 0; i < 2; i++) {
		const char* label = i == 0 ? "P1" : "P2";
		if (i < (int)members.size()) {
			const char* isMe = members[i].connId == c._cid ? " (you)" : "";
			const char* readyState = c._matchData.readyMessageNum[i] > -1 ? "Ready!" : "picking...";
			ImGui::Text("%s: %s%s - %s", label, members[i].name.c_str(), isMe, readyState);
		}
		else {
			ImGui::TextDisabled("%s: waiting for opponent", label);
		}
	}
	ImGui::Separator();

	if (g_app.bothReady) {
		if (g_app.gameLaunched) {
			ImGui::TextWrapped(
				"Game launched! Press a button on your controller at the "
				"title screen to bind it, then the match starts on its own."
			);
		}
		else {
			ImGui::TextWrapped("Both players are ready! Waiting for the game to launch...");
		}
		return;
	}

	if (mySide < 0 || mySide > 1) {
		ImGui::TextDisabled("Spectating- waiting for players");
		return;
	}

	if (c._outstandingReadyRequestNumber > -1) {
		ImGui::TextUnformatted("Waiting for server...");
		return;
	}
	if (c._matchData.readyMessageNum[mySide] > -1) {
		ImGui::TextUnformatted("Ready! Waiting for opponent.");
		return;
	}

	ImGui::Combo("Character", &g_app.myCharaID, Roster::characterNames, Roster::NUM_CHARACTERS);
	static const int stepSize = 1;
	ImGui::InputScalar("Color", ImGuiDataType_U8, &g_app.myConditions.color, &stepSize);
	ImGui::InputScalar("Costume", ImGuiDataType_U8, &g_app.myConditions.costume, &stepSize);
	ImGui::InputScalar("Ultra Combo", ImGuiDataType_U8, &g_app.myConditions.ultraCombo, &stepSize);
	if (mySide == 0) {
		ImGui::Combo("Stage", &g_app.stageID, Roster::stageNames, Roster::NUM_STAGES);
	}

	if (ImGui::Button("Ready", ImVec2(120, 0))) {
		g_app.myConditions.charaID = g_app.myCharaID;
		bool ok = c.PreBattle_SetChara(g_app.myConditions) == k_EResultOK;
		if (ok && mySide == 0) {
			ok = ok && c.PreBattle_SetEnv(g_app.rand()) == k_EResultOK;
			ok = ok && c.PreBattle_SetStage(g_app.stageID) == k_EResultOK;
		}
		if (ok) {
			c.Lobby_Ready();
		}
		else {
			g_app.alerts.push_back("Could not send match setup");
		}
	}
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

	ImGui::Text("Connected as %s", c._name.c_str());
	ImGui::SameLine();
	if (ImGui::SmallButton("Disconnect")) {
		DropToLogin(nullptr);
		return;
	}
	ImGui::Separator();
	DrawAlerts();

	// Refresh the lobby list on a timer while browsing.
	if (!inLobby) {
		uint64_t now = GetTickCount64();
		if (now >= g_app.nextListRefresh) {
			c.Lobby_RequestList();
			g_app.nextListRefresh = now + LOBBY_LIST_REFRESH_MS;
		}
	}

	ImGui::BeginChild("left_pane", ImVec2(340, 0), true);
	if (inLobby) {
		DrawLobbyPanel();
	}
	else {
		DrawLobbyBrowser();
	}
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("right_pane", ImVec2(0, 0), true);
	DrawChatPane();
	ImGui::EndChild();
}

// -------------------------------------------------------------------
// Logging

static void SetUpLogging(bool bShowConsole) {
	std::vector<spdlog::sink_ptr> sinks;

	PWSTR path;
	if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &path) == S_OK) {
		try {
			wchar_t logpath[MAX_PATH];
			PathCombineW(logpath, path, L"sf4e/logs/LobbyClient.log");
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

	if (!CreateDeviceD3D(hwnd))
	{
		CleanupDeviceD3D();
		::UnregisterClassW(wc.lpszClassName, wc.hInstance);
		return 1;
	}

	::ShowWindow(hwnd, SW_SHOWDEFAULT);
	::UpdateWindow(hwnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	ImGui::StyleColorsDark();

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
			HRESULT hr = g_pd3dDevice->TestCooperativeLevel();
			if (hr == D3DERR_DEVICELOST)
			{
				::Sleep(10);
				continue;
			}
			if (hr == D3DERR_DEVICENOTRESET)
				ResetDevice();
			g_DeviceLost = false;
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
		if (result == D3DERR_DEVICELOST)
			g_DeviceLost = true;
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
