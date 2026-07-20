#include <chrono>
#include <memory>

#include <windows.h>
#include <detours/detours.h>

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Event.hxx"
#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Math.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../Dimps/Dimps__UserApp.hxx"
#include "../session/sf4e__Resolve.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "../session/sf4e__SessionProtocol.hxx"
#include "../session/sf4e__SessionServer.hxx"

#include "sf4e__Event.hxx"
#include "sf4e__Game__Battle.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__Overlay.hxx"
#include "sf4e__UserApp.hxx"

namespace SessionProtocol = sf4e::SessionProtocol;
using Dimps::App;
using Dimps::Event::EventBase;
using Dimps::Event::EventBaseWithEC;
using Dimps::Event::EventController;
using Dimps::Game::ProgressData;
using Dimps::GameEvents::RootEvent;
using Dimps::Math::FixedPoint;
using rMainMenu = Dimps::GameEvents::MainMenu;
using rVsMode = Dimps::GameEvents::VsMode;
using rUserApp = Dimps::UserApp;
using fSystem = sf4e::Game::Battle::System;
using fUserApp = sf4e::UserApp;
using fMainMenu = sf4e::GameEvents::MainMenu;
using fVsBattle = sf4e::GameEvents::VsBattle;
using fVsPreBattle = sf4e::GameEvents::VsPreBattle;
using sf4e::Game::Battle::Sound::SoundPlayerManager;
using sf4e::SessionClient;
using sf4e::SessionServer;

std::unique_ptr<fUserApp::Netplay> fUserApp::netplay;
std::unique_ptr<SessionServer> fUserApp::server;

sf4e::UserApp::Netplay::Netplay(
    const SessionClient::Callbacks& callbacks,
    std::string sidecarHash,
    uint16_t ggpoPort,
    std::string& name,
    uint8_t _deviceType,
    uint8_t _deviceIdx,
    uint8_t _delay
):
    client(callbacks, sidecarHash, ggpoPort, name),
    deviceType(_deviceType),
    deviceIdx(_deviceIdx),
    delay(_delay)
{}

void fUserApp::_OnVsBattleTasksRegistered()
{
    // Start the GGPO connection
    bool isPlayer = false;
    for (int i = 0; i < 2; i++) {
        if (netplay->client._lobbyData.members[i].name == netplay->client._name) {
            isPlayer = true;
            break;
        }
    }
    if (isPlayer) {
        GGPOPlayer players[MAX_SF4E_PROTOCOL_USERS];
        for (int i = 0; i < 2 && i < netplay->client._lobbyData.members.size(); i++) {
            SessionProtocol::MemberData& memberData = netplay->client._lobbyData.members[i];
            GGPOPlayer& player = players[i];
            player.size = sizeof(GGPOPlayer);
            player.player_num = i + 1;
            if (netplay->client._lobbyData.members[i].name == netplay->client._name) {
                player.type = GGPO_PLAYERTYPE_LOCAL;

                // Inject the chosen device into this player's side
                Dimps::Pad::System* padSys = Dimps::Pad::System::staticMethods.GetSingleton();
                Dimps::Pad::System::__publicMethods& padSysMethods = Dimps::Pad::System::publicMethods;
                (padSys->*padSysMethods.AssociatePlayerAndGamepad)(i, netplay->deviceIdx);
                (padSys->*padSysMethods.SetDeviceTypeForPlayer)(i, netplay->deviceType);
                (padSys->*padSysMethods.SetSideHasAssignedController)(i, 1);
                (padSys->*padSysMethods.SetActiveButtonMapping)(Dimps::Pad::System::BUTTON_MAPPING_FIGHT);
            }
            else {
                SessionProtocol::MemberData& memberData = netplay->client._lobbyData.members[i];
                player.type = GGPO_PLAYERTYPE_REMOTE;
                if (memberData.ip.empty()) {
                    char szAddr[SteamNetworkingIPAddr::k_cchMaxString];
                    netplay->client._serverAddr.ToString(szAddr, sizeof(szAddr), false);
                    strcpy_s(player.u.remote.ip_address, 32, szAddr);
                }
                else {
                    strcpy_s(player.u.remote.ip_address, 32, memberData.ip.c_str());
                }

                player.u.remote.port = memberData.port;
            }
        }
        for (int i = 2; i < netplay->client._lobbyData.members.size(); i++) {
            SessionProtocol::MemberData& memberData = netplay->client._lobbyData.members[i];
            GGPOPlayer& player = players[i];
            player.type = GGPO_PLAYERTYPE_SPECTATOR;
            player.u.remote.port = memberData.port;

            if (memberData.ip.empty()) {
                char szAddr[SteamNetworkingIPAddr::k_cchMaxString];
                netplay->client._serverAddr.ToString(szAddr, sizeof(szAddr), false);
                strcpy_s(player.u.remote.ip_address, 32, szAddr);
            }
            else {
                strcpy_s(player.u.remote.ip_address, 32, memberData.ip.c_str());
            }
        }
        fSystem::StartGGPO(
            players,
            netplay->client._lobbyData.members.size(),
            netplay->client._ggpoPort,
            netplay->delay,
            netplay->client._matchData.rngSeed
        );
    }
    else {
        // Always spectate from	P1 for now- the protocol has
        // limited enough players that there's marginal bandwidth
        // differences.	
        // 
        char szAddr[SteamNetworkingIPAddr::k_cchMaxString];
        char* hostIP;
        if (netplay->client._lobbyData.members[0].ip.empty()) {
            netplay->client._serverAddr.ToString(szAddr, sizeof(szAddr), false);
            hostIP = szAddr;
        }
        else {
            // Safe-_ish_ removal of const. This gets passed through
            // to an inet_pton() call and never modified.
            hostIP = (char*)netplay->client._lobbyData.members[0].ip.c_str();
        }

        fSystem::StartSpectating(
            netplay->client._ggpoPort,
            2,
            hostIP,
            netplay->client._lobbyData.members[0].port,
            netplay->client._matchData.rngSeed
        );
    }
}

void fUserApp::_OnVsPreBattleTasksRegistered()
{
    size_t charaConditionSize = sizeof(rVsMode::ConfirmedCharaConditions);

    // XXX (adanducci): this is a little fragile- it's technically possible
    // that the pre-battle event is constructed in another context, but
    // practically speaking the VsPreBattle event will always be used in
    // the context of VsMode.
    char* vsModeQuery[] = { "VSMode" };
    rVsMode* mode = (rVsMode*)EventBaseWithEC::FindForegroundEvent(App::GetRootEvent(), vsModeQuery, 1);
    if (!mode) {
        spdlog::error("VsPreBattle tasks registered, but the current foreground event isn't VSMode!");
        return;
    }

    Dimps::Platform::dString* stageName = rVsMode::GetStageName(mode);
    rVsMode::ConfirmedPlayerConditions* conditions = rVsMode::GetConfirmedPlayerConditions(mode);
    for (int i = 0; i < 2; i++) {
        *(rVsMode::ConfirmedPlayerConditions::GetCharaID(&conditions[i])) = netplay->client._matchData.chara[i].charaID;
        *(rVsMode::ConfirmedPlayerConditions::GetSideActive(&conditions[i])) = 1;
        rVsMode::ConfirmedCharaConditions* charaConditions = rVsMode::ConfirmedPlayerConditions::GetCharaConditions(&conditions[i]);
        memcpy_s(charaConditions, charaConditionSize, &netplay->client._matchData.chara[i], charaConditionSize);
    }

    (stageName->*Dimps::Platform::dString::publicMethods.assign)(Dimps::stageCodes[netplay->client._matchData.stageID], 4);
    *(rVsMode::GetStageCode(mode)) = netplay->client._matchData.stageID;
}

void OnReady(sf4e::SessionClient* const client, const sf4e::SessionClient::Callbacks& c) {
    // A finished battle holding its teardown means this ALLREADY is a
    // rematch: roll straight into the next battle, no menu travel.
    if (fVsBattle::bRematchPending) {
        fUserApp::StartRematch();
        return;
    }

    // Since handling a request forces the process to load into a battle,
    // handling the request can only reasonably be done if the process is
    // currently on the main menu.
    RootEvent* root = App::GetRootEvent();
    char* mainMenuQuery[1] = { "MainMenu" };
    rMainMenu* mainMenu = (rMainMenu*)EventBaseWithEC::FindForegroundEvent(
        root,
        mainMenuQuery,
        1
    );
    if (!mainMenu) {
        spdlog::info("Client: ignoring that both clients are ready because we're not on the main menu");
        return;
    }

    ProgressData* progressData = *RootEvent::GetProgressData(root);
    ProgressData::BattleTypeSettings* BattleTypeSettings = &(ProgressData::GetBattleTypeSettings(progressData)[ProgressData::NBT_PVP]);
    *ProgressData::GetNextBattleType(progressData) = ProgressData::NBT_PVP;
    BattleTypeSettings->editionSelect = client->_lobbyData.editionSelect;
    BattleTypeSettings->rounds = client->_lobbyData.roundCount;
    BattleTypeSettings->timeLimit = client->_lobbyData.roundTime;
    fVsPreBattle::bSkipToVersus = true;
    fVsPreBattle::OnTasksRegistered = fUserApp::_OnVsPreBattleTasksRegistered;
    fVsBattle::OnTasksRegistered = fUserApp::_OnVsBattleTasksRegistered;
    (rMainMenu::ToItemObserver(mainMenu)->*rMainMenu::itemObserverMethods.GoToVersusMode)();
}

void OnBattleSynced(SessionClient* const client, const sf4e::SessionClient::Callbacks& callbacks) {
    fVsBattle::bSessionSynced = true;
}

sf4e::SessionClient::Callbacks clientCallbacks = {
    nullptr,
    sf4e::Overlay::OnClientError,
    OnReady,
    OnBattleSynced,
};

void fUserApp::Install() {
    DetourAttach((PVOID*)&rUserApp::staticMethods.Steam_PostUpdate, Steam_PostUpdate);
}

static DWORD WINAPI AbortNoticeThread(LPVOID pReason) {
    MessageBoxA(NULL, (const char*)pReason, "sf4e netplay", MB_OK | MB_ICONERROR);
    return 0;
}

void fUserApp::AbortNetplay(const char* szReason) {
    spdlog::error("Netplay aborted: {}", szReason);

    // Tear the session down first so the server frees this seat right
    // away instead of after a timeout, and give the networking thread
    // a beat to flush the close before the process is torn out from
    // under it.
    if (netplay) {
        delete netplay.release();
    }
    if (server) {
        delete server.release();
    }
    Sleep(250);
    GameNetworkingSockets_Kill();

    // Show the reason without demanding a click: the notice dismisses
    // itself with the process after a few seconds. Stacks of must-click
    // boxes were a playtest complaint.
    static char szNotice[512];
    strncpy_s(szNotice, szReason, _TRUNCATE);
    HANDLE hNotice = CreateThread(NULL, 0, AbortNoticeThread, szNotice, 0, NULL);
    if (hNotice) {
        WaitForSingleObject(hNotice, 8000);
        CloseHandle(hNotice);
    }

    spdlog::shutdown();
    ExitProcess(1);
}

void fUserApp::StartSession(
    char* joinAddr,
    uint16_t port,
    std::string& sidecarHash,
    std::string& name,
    uint8_t deviceType,
    uint8_t deviceIdx,
    uint8_t delay,
    const char* lobbyHost,
    const char* lobbyKey,
    const char* handoffToken
) {
    SteamNetworkingIPAddr addr;
    if (!sf4e::Net::ResolveHostPort(joinAddr, addr)) {
        spdlog::error("StartSession: could not resolve session server \"{}\"", joinAddr);
    }
    netplay.reset(new Netplay(
        clientCallbacks,
        sidecarHash,
        port,
        name,
        deviceType,
        deviceIdx,
        delay
    ));
    netplay->client._autoJoinLobby = { lobbyHost, lobbyKey };
    netplay->client._autoJoinHandoff = handoffToken;
    netplay->client.Connect(addr);
}

void fUserApp::StartServer(uint16 hostPort, std::string& identity, std::string& sidecarHash, bool editionSelect, int roundCount, FixedPoint roundTime) {
    server.reset(new SessionServer(identity, sidecarHash, editionSelect, roundCount, roundTime));
    server->Listen(hostPort);
}

// How long the post-battle hold waits for the rematch cycle before
// giving up and returning the player to the lobby app.
static const uint64_t REMATCH_WAIT_TIMEOUT_MS = 60 * 1000;

void fUserApp::StartRematch() {
    spdlog::info("Rematch: both sides ready, starting the next battle");

    // The same drive as OnReady's main-menu path, minus the menu
    // travel: re-apply settings and conditions from the fresh match
    // data, re-arm the per-battle hooks, and route the event flow
    // directly into the next battle.
    RootEvent* root = App::GetRootEvent();
    ProgressData* progressData = *RootEvent::GetProgressData(root);
    ProgressData::BattleTypeSettings* BattleTypeSettings = &(ProgressData::GetBattleTypeSettings(progressData)[ProgressData::NBT_PVP]);
    *ProgressData::GetNextBattleType(progressData) = ProgressData::NBT_PVP;
    BattleTypeSettings->editionSelect = netplay->client._lobbyData.editionSelect;
    BattleTypeSettings->rounds = netplay->client._lobbyData.roundCount;
    BattleTypeSettings->timeLimit = netplay->client._lobbyData.roundTime;
    _OnVsPreBattleTasksRegistered();
    fVsBattle::OnTasksRegistered = fUserApp::_OnVsBattleTasksRegistered;
    sf4e::Event::EventController::ReplaceNextEvent("VersusFromChr");
    fVsBattle::bRematchPending = false;
    fVsBattle::bBlockTermination = false;
}

void fUserApp::TickRematch() {
    if (!fVsBattle::bRematchPending) {
        return;
    }

    if (!netplay) {
        AbortNetplay(
            "Lost the session server while waiting for the rematch.\n\n"
            "The game will now close- your lobby app will put you back "
            "in the lobby."
        );
        return;
    }
    SessionClient& client = netplay->client;
    if (client._lobbyData.members.size() < 2) {
        AbortNetplay(
            "Your opponent left the match.\n\n"
            "The game will now close- your lobby app will put you back "
            "in the lobby."
        );
        return;
    }
    uint64_t now = GetTickCount64();
    if (now - fVsBattle::nRematchHoldStartMs > REMATCH_WAIT_TIMEOUT_MS) {
        AbortNetplay(
            "Timed out waiting for the rematch to start.\n\n"
            "The game will now close- your lobby app will put you back "
            "in the lobby."
        );
        return;
    }

    // Keep this seat readied while the server shows it unready. Both
    // games send battle_ended, and the later reset can wipe a ready
    // that landed between them- so nudge off the authoritative match
    // data, rate-limited.
    static uint64_t nLastReadySentMs = 0;
    int side = -1;
    for (int i = 0; i < (int)client._lobbyData.members.size() && i < 2; i++) {
        if (client._lobbyData.members[i].connId == client._cid) {
            side = i;
            break;
        }
    }
    if (
        side >= 0 &&
        client._matchData.readyMessageNum[side] == -1 &&
        now - nLastReadySentMs > 1000
    ) {
        nLastReadySentMs = now;
        client.Lobby_Ready();
    }
}

void fUserApp::Steam_PostUpdate() {
    if (netplay) {
        netplay->client.PrepareForCallbacks();
    }
    if (server) {
        server->PrepareForCallbacks();
    }
    SteamNetworkingSockets()->RunCallbacks();

    if (netplay) {
        if (netplay->client.Step()) {
            delete netplay.release();
        }
    }

    if (server) {
        if (server->Step()) {
            delete server.release();
        }
    }

    TickRematch();

    if (fSystem::ggpo) {
        ggpo_idle(fSystem::ggpo, 1);
    }

    rUserApp::staticMethods.Steam_PostUpdate();
}
