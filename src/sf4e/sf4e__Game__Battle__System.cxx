#include <algorithm>
#include <string.h>
#include <utility>
#include <vector>

#include <windows.h>
#include <detours/detours.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <ggponet.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Game__Battle__Camera.hxx"
#include "../Dimps/Dimps__Game__Battle__Chara.hxx"
#include "../Dimps/Dimps__Game__Battle__Command.hxx"
#include "../Dimps/Dimps__Game__Battle__Effect.hxx"
#include "../Dimps/Dimps__Game__Battle__Hud.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__Game__Battle__Training.hxx"
#include "../Dimps/Dimps__Game__Battle__Vfx.hxx"
#include "../Dimps/Dimps__Math.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../Dimps/Dimps__Platform.hxx"

#include "../session/sf4e__SessionProtocol.hxx"

#include "sf4e.hxx"
#include "sf4e__Game.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__Game__Battle.hxx"
#include "sf4e__Game__Battle__Hud.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "sf4e__Pad.hxx"
#include "sf4e__Platform.hxx"
#include "sf4e__UserApp.hxx"

using Dimps::Platform::WithReleaser;

namespace rHud = Dimps::Game::Battle::Hud;
using CameraUnit = Dimps::Game::Battle::Camera::Unit;
using CharaActor = Dimps::Game::Battle::Chara::Actor;
using CharaUnit = Dimps::Game::Battle::Chara::Unit;
using CommandUnit = Dimps::Game::Battle::Command::Unit;
using EffectUnit = Dimps::Game::Battle::Effect::Unit;
using GameManager = Dimps::Game::Battle::GameManager;
using HudUnit = Dimps::Game::Battle::Hud::Unit;
using NetworkUnit = Dimps::Game::Battle::Network::Unit;
using rSoundPlayerManager = Dimps::Game::Battle::Sound::SoundPlayerManager;
using rSystem = Dimps::Game::Battle::System;
using PauseUnit = Dimps::Game::Battle::Pause::Unit;
using TrainingManager = Dimps::Game::Battle::Training::Manager;
using VfxUnit = Dimps::Game::Battle::Vfx::Unit;
using rKey = Dimps::Game::GameMementoKey;
using FixedPoint = Dimps::Math::FixedPoint;
using fKey = sf4e::Game::GameMementoKey;
using rPadSystem = Dimps::Pad::System;
using fPadSystem = sf4e::Pad::System;
using StateSnapshot = sf4e::SessionProtocol::StateSnapshot;

namespace fHud = sf4e::Game::Battle::Hud;
using fSoundPlayerManager = sf4e::Game::Battle::Sound::SoundPlayerManager;
using fSystem = sf4e::Game::Battle::System;
using fVsBattle = sf4e::GameEvents::VsBattle;

bool fSystem::bHaltAfterNext = false;
bool fSystem::bUpdateAllowed = true;
int fSystem::nExtraFramesToSimulate = 0;
int fSystem::nNextBattleStartFlowTarget = -1;
int fSystem::nRandomizeLocalInputsEveryXFramesInGGPO = 0;
uint64_t fSystem::nGgpoWaitStartMs = 0;
bool fSystem::bGgpoEverRan = false;
bool fSystem::bGgpoConnectionInterrupted = false;
bool fSystem::bSynctestSession = false;
bool fSystem::bNatSymmetricHint = false;
bool fSystem::bSynctestPending = false;
int fSystem::nSynctestPendingDistance = 1;
DWORD fSystem::nSynctestPendingSeed = 0;

// Frames verified by the running sync-test session, for progress logs
// and the end-of-run summary.
static int s_nSynctestFramesVerified = 0;

// Randomized synctest inputs come from a dedicated generator seeded
// with the run's seed, so a logged run replays with identical inputs-
// localRand is wall-clock seeded and would shift the divergence frame
// between runs.
static std::mt19937 s_synctestInputRand;

// What bUpdateAllowed held before a connection interruption paused the
// simulation, restored when the connection resumes.
static bool s_bUpdateAllowedBeforeInterrupt = true;

// How long GGPO gets to complete its initial peer synchronization
// before the match is abandoned- the peer may be unreachable, ex.
// blocked by NAT or a firewall.
static const uint64_t GGPO_SYNC_TIMEOUT_MS = 45 * 1000;

GGPOPlayerHandle fSystem::localPlayerHandle = GGPO_INVALID_HANDLE;
GGPOSession* fSystem::ggpo = nullptr;
fSystem::PlayerConnectionInfo fSystem::players[MAX_SF4E_PROTOCOL_USERS];
fSystem::SaveState fSystem::saveStates[NUM_SAVE_STATES];

rKey::MementoID GGPO_MEMENTO_ID = { 1, 1 };

// Byte-wise Fletcher-32: cheap, order-sensitive, and enough to catch
// any state divergence. The block size keeps the 32-bit accumulators
// from overflowing before reduction.
static void Fletcher32(uint32_t& sum1, uint32_t& sum2, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    while (len > 0) {
        size_t block = len > 5802 ? 5802 : len;
        len -= block;
        while (block--) {
            sum1 += *p++;
            sum2 += sum1;
        }
        sum1 %= 65535;
        sum2 %= 65535;
    }
}

// Hash the bytes a memento key's saved copy owns. Keys allocate
// numMementos equal slots; only the slots recorded so far hold
// meaningful data- allocation slack past them is heap garbage and
// would false-positive.
static uint32_t ChecksumMementoKey(const rKey* key) {
    if (!key->mementos || key->numMementos <= 0 || key->sizeAllocated <= 0) {
        return 0;
    }
    int slotSize = key->sizeAllocated / key->numMementos;
    int used = key->nextMementoIndex;
    if (used < 0) {
        used = 0;
    }
    if (used > key->numMementos) {
        used = key->numMementos;
    }
    uint32_t s1 = 0, s2 = 0;
    Fletcher32(s1, s2, key->mementos, (size_t)slotSize * (size_t)used);
    return (s2 << 16) | s1;
}

// Full-state checksum for the sync-test session. Real matches skip
// this- hashing every save on every frame is too expensive there.
// outParts, when given, receives one hash per component in a stable
// order (memento keys, then sound entries, then flow globals) so a
// diverging component can be pinpointed.
static int ChecksumSaveState(const fSystem::SaveState* s, std::vector<uint32_t>* outParts) {
    uint32_t s1 = 0, s2 = 0;
    for (auto iter = s->keys.begin(); iter != s->keys.end(); iter++) {
        uint32_t key = ChecksumMementoKey(&iter->second);
        if (outParts) {
            outParts->push_back(key);
        }
        Fletcher32(s1, s2, &key, sizeof(key));
    }
    // The sound maps (criPlayerState/managerState) are deliberately NOT
    // hashed: they capture what the audio hardware is currently playing
    // so sounds can resume after a rollback- wall-clock state, not
    // deterministic simulation state. A first synctest run confirmed
    // they differ between a frame and its re-simulation while every
    // memento key matched.
    //
    // The flow globals hash as three parts so a divergence names the
    // field group: ids/frames, the flow callback pointers, and the
    // GameManager copy.
    auto pushPart = [&](const void* p, size_t len) {
        uint32_t e1 = 0, e2 = 0;
        Fletcher32(e1, e2, p, len);
        uint32_t entry = (e2 << 16) | e1;
        if (outParts) {
            outParts->push_back(entry);
        }
        Fletcher32(s1, s2, &entry, sizeof(entry));
    };
    const fSystem::SaveState::GlobalData& d = s->d;
    pushPart(
        &d.CurrentBattleFlow,
        (const char*)&d.BattleFlowSubstateCallable_aa9258 - (const char*)&d.CurrentBattleFlow
    );
    pushPart(
        &d.BattleFlowSubstateCallable_aa9258,
        sizeof(d.BattleFlowSubstateCallable_aa9258) + sizeof(d.BattleFlowCallback_CallEveryFrame_aa9254)
    );
    pushPart(&d.gameManager, sizeof(d.gameManager));
    return (int)((s2 << 16) | s1);
}

// Per-frame component hashes recorded during a sync-test run. The
// second save of a frame is the rollback re-simulation; comparing it
// against the first reports exactly which components diverged. This
// runs before GGPO's own checksum assert ends the run, and doesn't
// depend on GGPO's buffer lifetimes (which made the log_game_state
// dump unreliable).
struct SynctestFrame {
    std::vector<uint32_t> parts;
    fSystem::SaveState::GlobalData d;
};
static std::map<int, SynctestFrame> s_synctestFrameHashes;

// On a flow-globals divergence, dump the actual field values (not just
// hashes) of the original vs the re-simulated frame- that names the
// exact uncaptured field instead of "something in the globals".
static void DumpGlobalsDiff(const fSystem::SaveState::GlobalData& a, const fSystem::SaveState::GlobalData& b) {
#define DIFF_DW(field) if (a.field != b.field) spdlog::error("    {}: original {:#x} resim {:#x}", #field, (uint32_t)a.field, (uint32_t)b.field)
    DIFF_DW(CurrentBattleFlow);
    DIFF_DW(PreviousBattleFlow);
    DIFF_DW(CurrentBattleFlowSubstate);
    DIFF_DW(PreviousBattleFlowSubstate);
#undef DIFF_DW
#define DIFF_FP(field) if (a.field.integral != b.field.integral || a.field.fractional != b.field.fractional) \
        spdlog::error("    {}: original {}.{:04x} resim {}.{:04x}", #field, a.field.integral, (uint16_t)a.field.fractional, b.field.integral, (uint16_t)b.field.fractional)
    DIFF_FP(CurrentBattleFlowFrame);
    DIFF_FP(CurrentBattleFlowSubstateFrame);
    DIFF_FP(PreviousBattleFlowFrame);
    DIFF_FP(PreviousBattleFlowSubstateFrame);
#undef DIFF_FP
    if (a.BattleFlowSubstateCallable_aa9258 != b.BattleFlowSubstateCallable_aa9258) {
        spdlog::error("    SubstateCallable: original {:#x} resim {:#x}",
            (uint32_t)(uintptr_t)a.BattleFlowSubstateCallable_aa9258,
            (uint32_t)(uintptr_t)b.BattleFlowSubstateCallable_aa9258);
    }
    if (a.BattleFlowCallback_CallEveryFrame_aa9254 != b.BattleFlowCallback_CallEveryFrame_aa9254) {
        spdlog::error("    CallEveryFrameCallback: original {:#x} resim {:#x}",
            (uint32_t)(uintptr_t)a.BattleFlowCallback_CallEveryFrame_aa9254,
            (uint32_t)(uintptr_t)b.BattleFlowCallback_CallEveryFrame_aa9254);
    }
    // GameManager: name the first differing 4-byte offsets so the field
    // can be located in the RE'd 0x49c-byte blob.
    const uint32_t* ga = (const uint32_t*)&a.gameManager;
    const uint32_t* gb = (const uint32_t*)&b.gameManager;
    int shown = 0;
    for (size_t off = 0; off < sizeof(a.gameManager) / 4 && shown < 8; off++) {
        if (ga[off] != gb[off]) {
            spdlog::error("    gameManager+{:#05x}: original {:#010x} resim {:#010x}", off * 4, ga[off], gb[off]);
            shown++;
        }
    }
}

static void SynctestCompareFrame(int frame, const fSystem::SaveState* state, std::vector<uint32_t>& parts) {
    auto prior = s_synctestFrameHashes.find(frame);
    if (prior == s_synctestFrameHashes.end()) {
        SynctestFrame sf;
        sf.parts = parts;
        sf.d = state->d;
        s_synctestFrameHashes[frame] = sf;
        // A bounded window is plenty- comparisons happen within the
        // rollback distance of the original save.
        while (s_synctestFrameHashes.size() > 64) {
            s_synctestFrameHashes.erase(s_synctestFrameHashes.begin());
        }
        return;
    }

    if (prior->second.parts != parts) {
        size_t nKeys = state->keys.size();
        spdlog::error("SYNCTEST DIVERGENCE at frame {}: re-simulation differs from the original run", frame);
        bool flowDiverged = false;
        size_t n = parts.size() < prior->second.parts.size() ? parts.size() : prior->second.parts.size();
        for (size_t k = 0; k < n; k++) {
            if (prior->second.parts[k] == parts[k]) {
                continue;
            }
            if (k < nKeys) {
                spdlog::error(
                    "  key {:3} (mementoable {:08x}): original {:08x} resim {:08x}",
                    k,
                    (uint32_t)(uintptr_t)state->keys[k].second.mementoableObject,
                    prior->second.parts[k],
                    parts[k]
                );
            }
            else {
                size_t tail = k - nKeys;
                spdlog::error(
                    "  part {:3} ({}): original {:08x} resim {:08x}",
                    k,
                    tail == 0 ? "battle flow ids/frames" : tail == 1 ? "flow callables" : "game manager",
                    prior->second.parts[k],
                    parts[k]
                );
                flowDiverged = true;
            }
        }
        if (flowDiverged) {
            DumpGlobalsDiff(prior->second.d, state->d);
        }
        if (parts.size() != prior->second.parts.size()) {
            spdlog::error("  component count differs: original {} resim {}", prior->second.parts.size(), parts.size());
        }
    }
    s_synctestFrameHashes.erase(prior);
}

bool fSystem::extendedLoadRequest = false;
bool fSystem::extendedSaveRequest = false;
GameMementoKey::MementoID fSystem::mementoLoadRequest = { 0xffffffff, 0xffffffff };
GameMementoKey::MementoID fSystem::mementoSaveRequest = { 0xffffffff, 0xffffffff };

void fSystem::Install() {
    void (fSystem:: * _fBattleUpdate)() = &BattleUpdate;
    void (fSystem:: * _fCloseBattle)() = &CloseBattle;
    void (fSystem:: * _fSysMain_HandleTrainingModeFeatures)() = &SysMain_HandleTrainingModeFeatures;
    void (fSystem:: * _fSysMain_UpdatePauseState)() = &SysMain_UpdatePauseState;
    int (fSystem:: * _fGetMementoSize)() = &GetMementoSize;
    int (fSystem:: * _fRecordToMemento)(Memento * m, GameMementoKey::MementoID * id) = &RecordToMemento;
    int (fSystem:: * _fRestoreFromMemento)(Memento * m, GameMementoKey::MementoID * id) = &RestoreFromMemento;

    DetourAttach((PVOID*)&rSystem::mementoableMethods.GetMementoSize, *(PVOID*)&_fGetMementoSize);
    DetourAttach((PVOID*)&rSystem::mementoableMethods.RecordToMemento, *(PVOID*)&_fRecordToMemento);
    DetourAttach((PVOID*)&rSystem::mementoableMethods.RestoreFromMemento, *(PVOID*)&_fRestoreFromMemento);

    DetourAttach((PVOID*)&rSystem::publicMethods.BattleUpdate, *(PVOID*)&_fBattleUpdate);
    DetourAttach((PVOID*)&rSystem::publicMethods.CloseBattle, *(PVOID*)&_fCloseBattle);
    DetourAttach((PVOID*)&rSystem::publicMethods.SysMain_HandleTrainingModeFeatures, *(PVOID*)&_fSysMain_HandleTrainingModeFeatures);
    DetourAttach((PVOID*)&rSystem::publicMethods.SysMain_UpdatePauseState, *(PVOID*)&_fSysMain_UpdatePauseState);
    DetourAttach((PVOID*)&rSystem::staticMethods.OnBattleFlow_BattleStart, OnBattleFlow_BattleStart);
}

int fSystem::GetMementoSize() {
    return (this->*rSystem::mementoableMethods.GetMementoSize)() + sizeof(AdditionalMemento);
}

int fSystem::RecordToMemento(Memento* m, GameMementoKey::MementoID* id) {
    AdditionalMemento* additional = (AdditionalMemento*)((unsigned int)m + sizeof(Memento));
    rSystem* _this = rSystem::FromMementoable(this);
    additional->nFirstCharaToSimulate = *rSystem::GetFirstCharaToSimulate(_this);
    additional->skipRelatedFlags_0xd8c = *rSystem::GetSkipRelatedFlags_0xd8c(_this);
    additional->simulationFlags = *rSystem::GetSimulationFlags(_this);
    additional->transitionProgress  = *rSystem::GetTransitionProgress(_this);
    additional->transitionSpeed = *rSystem::GetTransitionSpeed(_this);
    additional->transitionType = *rSystem::GetTransitionType(_this);
    additional->network = *(NetworkUnit*)(_this->*rSystem::publicMethods.GetUnitByIndex)(System::U_NETWORK);

    HudUnit* hud = (HudUnit*)(_this->*rSystem::publicMethods.GetUnitByIndex)(System::U_HUD);
    fHud::Announce::Unit::RecordToAdditionalMemento(*HudUnit::GetAnnounce(hud), additional->announce);

    rHud::Notice::View* noticeView = *rHud::Notice::Unit::GetView(*HudUnit::GetNotice(hud));
    WithReleaser<rHud::Notice::Player>* noticePlayers = rHud::Notice::View::GetPlayers(noticeView);
    for (int playerIdx = 0; playerIdx < (_this->*rSystem::publicMethods.GetNumCharasToSimulateThisFrame)(); playerIdx++) {
        fHud::Notice::Player::RecordToAdditionalMemento(
            noticePlayers[playerIdx].obj,
            additional->playerNotices[playerIdx]
        );
    }

    Platform::GFxApp::RecordToAdditionalMemento(
        Dimps::Platform::GFxApp::staticMethods.GetSingleton(),
        additional->gfxApp
    );

    Eva::TaskCore::RecordToAdditionalMemento(
        (_this->*rSystem::publicMethods.GetTaskCore)(System::TCI_UPDATE),
        additional->updateCore
    );

    return (this->*rSystem::mementoableMethods.RecordToMemento)(m, id);
}

int fSystem::RestoreFromMemento(Memento* m, GameMementoKey::MementoID* id) {
    AdditionalMemento* additional = (AdditionalMemento*)((unsigned int)m + sizeof(Memento));
    rSystem* _this = rSystem::FromMementoable(this);
    *rSystem::GetFirstCharaToSimulate(_this) = additional->nFirstCharaToSimulate;
    *rSystem::GetSkipRelatedFlags_0xd8c(_this) = additional->skipRelatedFlags_0xd8c;
    *rSystem::GetSimulationFlags(_this) = additional->simulationFlags;
    *rSystem::GetTransitionProgress(_this) = additional->transitionProgress;
    *rSystem::GetTransitionSpeed(_this) = additional->transitionSpeed;
    *rSystem::GetTransitionType(_this) = additional->transitionType;
    *(NetworkUnit*)(_this->*rSystem::publicMethods.GetUnitByIndex)(System::U_NETWORK) = additional->network;

    HudUnit* hud = (HudUnit*)(_this->*rSystem::publicMethods.GetUnitByIndex)(System::U_HUD);
    rHud::Announce::Unit* announce = *HudUnit::GetAnnounce(hud);
    fHud::Announce::Unit::RestoreFromAdditionalMemento(announce, additional->announce);

    rHud::Notice::View* noticeView = *rHud::Notice::Unit::GetView(*HudUnit::GetNotice(hud));
    WithReleaser<rHud::Notice::Player>* noticePlayers = rHud::Notice::View::GetPlayers(noticeView);
    for (int playerIdx = 0; playerIdx < (_this->*rSystem::publicMethods.GetNumCharasToSimulateThisFrame)(); playerIdx++) {
        fHud::Notice::Player::RestoreFromAdditionalMemento(
            noticePlayers[playerIdx].obj,
            additional->playerNotices[playerIdx]
        );
    }

    Platform::GFxApp::RestoreFromAdditionalMemento(
        Dimps::Platform::GFxApp::staticMethods.GetSingleton(),
        additional->gfxApp
    );

    Dimps::Eva::TaskCore* updateCore = (_this->*rSystem::publicMethods.GetTaskCore)(System::TCI_UPDATE);
    Eva::TaskCore::RestoreFromAdditionalMemento(updateCore, additional->updateCore);

    // Now that the task core is restored, update all the handles.
    CameraUnit* cam = (CameraUnit*)(_this->*rSystem::publicMethods.GetUnitByIndex)(U_CAMERA);
    PauseUnit* pause = (PauseUnit*)(_this->*rSystem::publicMethods.GetUnitByIndex)(U_PAUSE);
    *PauseUnit::GetPauseTask(pause) = nullptr;
    *CameraUnit::GetCamShakeTask(cam) = nullptr;
    *rHud::Announce::Unit::GetHudAnnounceUpdateTask(announce) = nullptr;
    *rHud::Cockpit::Unit::GetHudCockpitUpdateTask(*HudUnit::GetCockpit(hud)) = nullptr;
    if (*HudUnit::GetContinue(hud)) {
        *rHud::Continue::Unit::GetHudContinueUpdateTask(*HudUnit::GetContinue(hud)) = nullptr;
    }
    *rHud::Cursor::Unit::GetHudCursorUpdateTask(*HudUnit::GetCursor(hud)) = nullptr;
    *rHud::Notice::Unit::GetHudNoticeUpdateTask(*HudUnit::GetNotice(hud)) = nullptr;
    if (*HudUnit::GetResult(hud)) {
        *rHud::Result::Unit::GetHudResultUpdateTask(*HudUnit::GetResult(hud)) = nullptr;
    }
    if (*HudUnit::GetSubtitle(hud)) {
        *rHud::Subtitle::Unit::GetHudSubtitleUpdateTask(*HudUnit::GetSubtitle(hud)) = nullptr;
    }
    if (*HudUnit::GetTraining(hud)) {
        *rHud::Training::Unit::GetHudTrainingUpdateTask(*HudUnit::GetTraining(hud)) = nullptr;
    }

    Dimps::Eva::Task* cursor;
    for (
        cursor = Dimps::Eva::TaskCore::GetTaskHead(updateCore);
        cursor != nullptr;
        cursor = *Dimps::Eva::Task::GetNext(cursor)
    ) {
        char* name = (updateCore->*Dimps::Eva::TaskCore::publicMethods.GetTaskName)(&cursor);
        if (strcmp(name, "PAUSE") == 0) {
            *PauseUnit::GetPauseTask(pause) = cursor;
        } else if (strcmp(name, "CAM SHAKE") == 0) {
            *CameraUnit::GetCamShakeTask(cam) = cursor;
        }
        else if (strcmp(name, "HUD ANNOUNCE") == 0) {
            *rHud::Announce::Unit::GetHudAnnounceUpdateTask(announce) = cursor;
        }
        else if (strcmp(name, "HUD COCKPIT") == 0) {
            *rHud::Cockpit::Unit::GetHudCockpitUpdateTask(*HudUnit::GetCockpit(hud)) = cursor;
        }
        else if (strcmp(name, "HUD CONTINUE") == 0) {
            *rHud::Continue::Unit::GetHudContinueUpdateTask(*HudUnit::GetContinue(hud)) = cursor;
        }
        else if (strcmp(name, "HUD CURSOR") == 0) {
            *rHud::Cursor::Unit::GetHudCursorUpdateTask(*HudUnit::GetCursor(hud)) = cursor;
        }
        else if (strcmp(name, "HUD NOTICE") == 0) {
            *rHud::Notice::Unit::GetHudNoticeUpdateTask(*HudUnit::GetNotice(hud)) = cursor;
        }
        else if (strcmp(name, "HUD RESULT") == 0) {
            *rHud::Result::Unit::GetHudResultUpdateTask(*HudUnit::GetResult(hud)) = cursor;
        }
        else if (strcmp(name, "HUD SUBTITLE") == 0) {
            *rHud::Subtitle::Unit::GetHudSubtitleUpdateTask(*HudUnit::GetSubtitle(hud)) = cursor;
        }
        else if (strcmp(name, "HUD TRAINING") == 0) {
            if (*HudUnit::GetTraining(hud)) {
                *rHud::Training::Unit::GetHudTrainingUpdateTask(*HudUnit::GetTraining(hud)) = nullptr;
            }
        }
    }

    return (this->*rSystem::mementoableMethods.RestoreFromMemento)(m, id);
}

void fSystem::BattleUpdate() {
    rSystem* _this = (rSystem*)this;
    rSystem::__publicMethods& sysMethods = rSystem::publicMethods;
    rPadSystem* p = rPadSystem::staticMethods.GetSingleton();
    rPadSystem::__publicMethods& padMethods = rPadSystem::publicMethods;
    static int nLastRandomInputFrame = -1;
    static fPadSystem::Inputs randomInputs[2] = { { 0, 0 }, { 0, 0 } };

    if (!bUpdateAllowed) {
        // If the battle wants to leave (peer disconnected, match
        // aborted), run the native update GGPO-free so the leaving flow
        // can process. Returning here instead would freeze the game on
        // its final frame forever: the pause taken at
        // CONNECTION_INTERRUPTED is only undone by a RESUMED event,
        // which a peer that already exited will never send.
        if (*rSystem::GetReadyState(_this) == rSystem::RS_ISLEAVING) {
            if (fSoundPlayerManager::bUsePureSounds) {
                fSoundPlayerManager::SyncState();
            }
            (_this->*sysMethods.BattleUpdate)();
            return;
        }

        // A GGPO session that has never reached its running state is
        // still in the initial peer handshake; give up if it takes
        // implausibly long.
        if (ggpo && !bGgpoEverRan) {
            uint64_t now = GetTickCount64();
            if (nGgpoWaitStartMs == 0) {
                nGgpoWaitStartMs = now;
            }
            else if (now - nGgpoWaitStartMs > GGPO_SYNC_TIMEOUT_MS) {
                sf4e::UserApp::AbortNetplay(
                    bNatSymmetricHint
                    ? "Could not reach the opponent to start the match.\n\n"
                      "This machine is behind a symmetric NAT, which "
                      "usually cannot connect directly. Forwarding the "
                      "GGPO UDP port (23457 by default) on the router "
                      "fixes it.\n\n"
                      "The game will now close- your lobby app will put "
                      "you back in the lobby."
                    : "Couldn't reach the opponent for a direct connection.\n\n"
                      "Head back and ready up again- when a direct match "
                      "between two players fails, the server automatically "
                      "routes their next one through the relay.\n\n"
                      "The game will now close- your lobby app will put "
                      "you back in the lobby."
                );
            }
        }
        return;
    }

    // Every frame that runs the native simulation under an active GGPO
    // session MUST advance GGPO too- rollback requires a 1:1 map between
    // GGPO frames and simulation steps. The old gate also excluded
    // BF__IDLE, so the game advanced state during round-transition IDLE
    // frames WITHOUT advancing GGPO; on rollback those steps were lost
    // and the battle-flow state machine diverged (found via synctest:
    // the re-simulation landed in a different flow state at the first
    // round transition, ~frame 2728). Gating on `ggpo` alone closes the
    // hole; the else branch now runs only offline (no GGPO).
    if (ggpo) {
        GGPOErrorCode result = GGPO_OK;
        if (localPlayerHandle != GGPO_INVALID_HANDLE) {
            for (int i = 0; i < 2; i++) {
                if (players[i].type == GGPO_PLAYERTYPE_LOCAL) {
                    fPadSystem::Inputs inputs;
                    if (nRandomizeLocalInputsEveryXFramesInGGPO != 0) {
                        int currentFrame = rSystem::GetNumFramesSimulated_FixedPoint(_this)->integral;
                        if (
                            nLastRandomInputFrame < 0 ||
                            (currentFrame - nLastRandomInputFrame) > nRandomizeLocalInputsEveryXFramesInGGPO
                        ) {
                            std::mt19937& gen = bSynctestSession ? s_synctestInputRand : sf4e::localRand;
                            randomInputs[0] = { gen(), gen() };
                            randomInputs[1] = { gen(), gen() };
                            nLastRandomInputFrame = currentFrame;
                        }
                        inputs = randomInputs[i];
                    }
                    else {
                        inputs = { (p->*padMethods.GetButtons_MappedOn)(i), (p->*padMethods.GetButtons_RawOn)(i) };
                    }
                    result = ggpo_add_local_input(ggpo, players[i].handle, &inputs, sizeof(fPadSystem::Inputs));
                    // A sync-test session drives both sides locally; a
                    // real match has exactly one local player.
                    if (!bSynctestSession) {
                        break;
                    }
                }
            }
        }

        if (GGPO_SUCCEEDED(result)) {
            fPadSystem::Inputs ggpoInputs[2] = { {0, 0}, {0, 0} };
            int disconnect_flags = 0;
            result = ggpo_synchronize_input(ggpo, (void*)ggpoInputs, sizeof(fPadSystem::Inputs) * 2, &disconnect_flags);
            if (GGPO_SUCCEEDED(result)) {
                fPadSystem::playbackFrame = 0;
                fPadSystem::playbackData[0][0] = ggpoInputs[0];
                fPadSystem::playbackData[0][1] = ggpoInputs[1];
                if (fSoundPlayerManager::bUsePureSounds) {
                    fSoundPlayerManager::SyncState();
                }
                (_this->*sysMethods.BattleUpdate)();
                fPadSystem::playbackFrame = -1;
                GGPOErrorCode err = ggpo_advance_frame(ggpo);
                if (!GGPO_SUCCEEDED(err)) {
                    AbortGgpoMatch("GGPO could not advance the frame after normal simulation");
                }
                else {
                    if (fSoundPlayerManager::bUsePureSounds) {
                        fSoundPlayerManager::SyncState();
                    }
                    CaptureSnapshot(_this);
                    if (bSynctestSession && (++s_nSynctestFramesVerified % 600) == 0) {
                        spdlog::info("Synctest: {} frames verified clean", s_nSynctestFramesVerified);
                    }
                }
            }
        }
    }
    else {
        if (fSoundPlayerManager::bUsePureSounds) {
            fSoundPlayerManager::SyncState();
        }
        (_this->*rSystem::publicMethods.BattleUpdate)();
    }
    
    if (nExtraFramesToSimulate > 0) {
        for (int i = 0; i < nExtraFramesToSimulate; i++) {
            fPadSystem::playbackFrame = i;
            (_this->*sysMethods.BattleUpdate)();
        }
        fPadSystem::playbackFrame = -1;
        nExtraFramesToSimulate = 0;
    }

    if (bHaltAfterNext) {
        bHaltAfterNext = false;
        bUpdateAllowed = false;
    }
}

void fSystem::CloseBattle() {
    rSystem* _this = (rSystem*)this;
    if (ggpo) {
        ggpo_close_session(ggpo);
        ggpo = nullptr;
    }
    if (bSynctestSession) {
        spdlog::info(
            "Synctest ended: {} frames verified without divergence",
            s_nSynctestFramesVerified
        );
        bSynctestSession = false;
        s_synctestFrameHashes.clear();
    }
    nGgpoWaitStartMs = 0;
    bGgpoEverRan = false;
    bGgpoConnectionInterrupted = false;
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        if (saveStates[i].used) {
            SaveState::Free(&saveStates[i]);
        }
    }

    // Local snapshots are per-battle; letting them survive into the
    // next game trips false desync alarms on rematch (upstream #9).
    snapshotMap.clear();

    (_this->*rSystem::publicMethods.CloseBattle)();

}

void fSystem::OnBattleFlow_BattleStart(System* s) {
    if (bSynctestPending) {
        // Late, reliable start point: the battle flow starting means
        // the battle is fully built, safely after the loading screen.
        // (VsBattle::HasInitialized also consumes the pending start,
        // but was not observed to run in the synctest drive's flow.)
        bSynctestPending = false;
        StartSynctest(nSynctestPendingDistance, nSynctestPendingSeed);
    }
    if (sf4e::UserApp::netplay && !ggpo) {
        // A lobby battle is starting without a GGPO session: some flow
        // (a native rematch, an unexpected menu path) reached a battle
        // the netplay machinery never armed. Never play it out- each
        // side would simulate alone.
        sf4e::UserApp::AbortNetplay(
            "The match restarted without netplay attached.\n\n"
            "The game will now close- your lobby app will put you back "
            "in the lobby."
        );
        return;
    }
    if (nNextBattleStartFlowTarget > -1) {
        rSystem::staticMethods.SetBattleFlow(s, nNextBattleStartFlowTarget);
        nNextBattleStartFlowTarget = -1;
        return;
    }

    return rSystem::staticMethods.OnBattleFlow_BattleStart(s);
}

void fSystem::SysMain_HandleTrainingModeFeatures() {
    rSystem* _this = (rSystem*)this;
    void* (rSystem:: * GetUnitByIndex)(unsigned int) = rSystem::publicMethods.GetUnitByIndex;
    CharaUnit* charaUnit = (CharaUnit*)(_this->*GetUnitByIndex)(rSystem::U_CHARA);

    if (mementoLoadRequest.lo != -1 && mementoLoadRequest.hi != -1) {
        fSystem::RestoreAllFromInternalMementos(_this, &mementoLoadRequest);
        mementoLoadRequest.lo = -1;
        mementoLoadRequest.hi = -1;
    }

    if (mementoSaveRequest.lo != -1 && mementoSaveRequest.hi != -1) {
        fSystem::RecordAllToInternalMementos(_this, &mementoSaveRequest);

        mementoSaveRequest.lo = -1;
        mementoSaveRequest.hi = -1;
    }

    if (extendedLoadRequest) {
        if (saveStates[0].used) {
            fSystem::SaveState::Load(&saveStates[0]);
        }
        extendedLoadRequest = false;
    }

    if (extendedSaveRequest) {
        if (saveStates[0].used) {
            fSystem::SaveState::Free(&saveStates[0]);
        }
        fSystem::SaveState::Save(&saveStates[0]);
        extendedSaveRequest = false;
    }

    (_this->*rSystem::publicMethods.SysMain_HandleTrainingModeFeatures)();
}

void fSystem::SysMain_UpdatePauseState() {
    if (!ggpo) {
        (this->*rSystem::publicMethods.SysMain_UpdatePauseState)();
    }
}

void fSystem::RestoreAllFromInternalMementos(rSystem* system, rKey::MementoID * id) {
    void* (rSystem:: * GetUnitByIndex)(unsigned int) = rSystem::publicMethods.GetUnitByIndex;
    CharaUnit* charaUnit = (CharaUnit*)(system->*GetUnitByIndex)(rSystem::U_CHARA);

    (system->*rSystem::publicMethods.RestoreFromInternalMementoKey)(id);
    (charaUnit->*CharaUnit::publicMethods.RestoreFromInternalMementoKey)(id);
    (
        ((EffectUnit*)(system->*GetUnitByIndex)(rSystem::U_EFFECT))->*
        EffectUnit::publicMethods.RestoreFromInternalMementoKey
        )(id);

    (
        ((VfxUnit*)(system->*GetUnitByIndex)(rSystem::U_VFX))->*
        VfxUnit::publicMethods.RestoreFromInternalMementoKey
        )(id);


    (
        ((CommandUnit*)(system->*GetUnitByIndex)(rSystem::U_COMMAND))->*
        CommandUnit::publicMethods.RestoreFromInternalMementoKey
        )(id);


    (
        ((HudUnit*)(system->*GetUnitByIndex)(rSystem::U_HUD))->*
        HudUnit::publicMethods.RestoreFromInternalMementoKey
        )(id);

    (
        ((CameraUnit*)(system->*GetUnitByIndex)(rSystem::U_CAMERA))->*
        CameraUnit::publicMethods.RestoreFromInternalMementoKey
        )(id);

    (
        TrainingManager::staticMethods.GetSingleton()->*
        TrainingManager::publicMethods.RestoreFromInternalMementoKey
        )(id);

    CharaActor::staticMethods.ResetAfterMemento((charaUnit->*CharaUnit::publicMethods.GetActorByIndex)(0));
    CharaActor::staticMethods.ResetAfterMemento((charaUnit->*CharaUnit::publicMethods.GetActorByIndex)(1));

    // Intentionally omit the reset of the Network unit. All in-game inputs
    // are passed into and read back out of the network unit, regardless
    // of whether or not the match is local or network. The network unit's
    // reset is used to zero the inputs of the first frame after a memento
    // is loaded in training mode, for no real practical reason.
}

void fSystem::RecordAllToInternalMementos(rSystem* system, GameMementoKey::MementoID* id) {
    // This method exists entirely to work around the check that actors are
    // movable before the training mode mementos are saveable. This could be
    // replaced just by no-oping the `jz` instruction at 0x5d7fa0, but this
    // is probably more legible.
    void* (rSystem:: * GetUnitByIndex)(unsigned int) = rSystem::publicMethods.GetUnitByIndex;
    (system->*rSystem::publicMethods.RecordToInternalMementoKey)(id);

    (
        ((CharaUnit*)(system->*GetUnitByIndex)(rSystem::U_CHARA))->*
        CharaUnit::publicMethods.RecordToInternalMementoKey
        )(id);

    (
        ((EffectUnit*)(system->*GetUnitByIndex)(rSystem::U_EFFECT))->*
        EffectUnit::publicMethods.RecordToInternalMementoKey
        )(id);

    (
        ((VfxUnit*)(system->*GetUnitByIndex)(rSystem::U_VFX))->*
        VfxUnit::publicMethods.RecordToInternalMementoKey
        )(id);

    (
        ((CommandUnit*)(system->*GetUnitByIndex)(rSystem::U_COMMAND))->*
        CommandUnit::publicMethods.RecordToInternalMementoKey
        )(id);

    (
        ((HudUnit*)(system->*GetUnitByIndex)(rSystem::U_HUD))->*
        HudUnit::publicMethods.RecordToInternalMementoKey
        )(id);

    (
        ((CameraUnit*)(system->*GetUnitByIndex)(rSystem::U_CAMERA))->*
        CameraUnit::publicMethods.RecordToInternalMementoKey
        )(id);

    (
        TrainingManager::staticMethods.GetSingleton()->*
        TrainingManager::publicMethods.RecordToInternalMementoKey
        )(id);
}


void fSystem::AbortGgpoMatch(const char* szReason) {
    // A dead match (desync, GGPO failure) cannot be resumed, and the
    // first playtest that hit one proved that flowing the abort into
    // the post-battle rematch path is fragile: the native rematch menu
    // raced the in-process rematch machinery and restarted the battle
    // WITHOUT a GGPO session- both players ghost-fighting alone. Exit
    // the process cleanly instead: the notice dismisses itself and the
    // lobby app re-seats the player for a fresh pairing.
    spdlog::error("GGPO match aborted: {}", szReason);
    static char szNotice[512];
    snprintf(
        szNotice,
        sizeof(szNotice),
        "The match had to stop: %s.\n\n"
        "The game will now close- your lobby app will put you back in "
        "the lobby.",
        szReason
    );
    sf4e::UserApp::AbortNetplay(szNotice);
}

void fSystem::StartGGPO(GGPOPlayer* inPlayers, int numPlayers, int port, int frameDelay, DWORD rngSeed) {
    GGPOSessionCallbacks cb = { 0 };
    cb.begin_game = ggpo_begin_game_callback;
    cb.advance_frame = ggpo_advance_frame_callback;
    cb.load_game_state = ggpo_load_game_state_callback;
    cb.save_game_state = ggpo_save_game_state_callback;
    cb.free_buffer = ggpo_free_buffer;
    cb.on_event = ggpo_on_event_callback;
    cb.log_game_state = ggpo_log_game_state;

    GGPOErrorCode result = ggpo_start_session(
        &ggpo,
        &cb,
        "sf4e",
        2,
        sizeof(fPadSystem::Inputs),
        port
    );
    if (result != GGPO_OK) {
        spdlog::error("GGPO session could not start: {}", (int)result);
        sf4e::UserApp::AbortNetplay(
            "GGPO could not start a session for this match; see the "
            "sf4e log for details.\n\n"
            "The game will now close- your lobby app will put you back "
            "in the lobby."
        );
    }
    // Upstream's 1000/500 proved too aggressive for WAN play- transient
    // hiccups became disconnects. These values are field-tested in
    // Confetti3's SF4-Netplay-Launcher.
    ggpo_set_disconnect_timeout(ggpo, 3000);
    ggpo_set_disconnect_notify_start(ggpo, 1500);

    int localPlayerIdx = -1;
    for (int i = 0; i < 2; i++) {
        if (inPlayers[i].type == GGPO_PLAYERTYPE_REMOTE) {
            // The single most important diagnostic for a failed
            // handshake: what this side actually fired at.
            spdlog::info(
                "GGPO: firing at peer {} at {}:{}",
                i, inPlayers[i].u.remote.ip_address, inPlayers[i].u.remote.port
            );
        }
        players[i].type = inPlayers[i].type;
        result = ggpo_add_player(ggpo, inPlayers + i, &players[i].handle);
        if (!GGPO_SUCCEEDED(result)) {
            spdlog::error("GGPO session could not add player: {}", (int)result);
            continue;
        }

        if (players[i].type == GGPO_PLAYERTYPE_LOCAL) {
            ggpo_set_frame_delay(ggpo, players[i].handle, frameDelay);
            localPlayerHandle = players[i].handle;
            localPlayerIdx = i;
        }
    }
    if (localPlayerIdx == 0) {
        for (int i = 2; i < numPlayers; i++) {
            players[i].type = inPlayers[i].type;
            result = ggpo_add_player(ggpo, inPlayers + i, &players[i].handle);
            if (!GGPO_SUCCEEDED(result)) {
                spdlog::error("GGPO session could not add spectator: {}", (int)result);
                continue;
            }
        }
    }

    // A stale leaving state from the previous game in this process (a
    // disconnect or abort) must not leak into this one- BattleUpdate
    // treats RS_ISLEAVING as "wind the battle down".
    rSystem* system = rSystem::staticMethods.GetSingleton();
    if (system) {
        *rSystem::GetReadyState(system) = rSystem::RS_START;
    }

    nNextBattleStartFlowTarget = BF__MATCH_START;
    bUpdateAllowed = false;
    nGgpoWaitStartMs = 0;
    bGgpoEverRan = false;
    bGgpoConnectionInterrupted = false;
    // Played matches hold their teardown for the in-process rematch
    // cycle (see fVsBattle::ExitForeground) instead of terminating the
    // event tree; spectators still terminate.
    fVsBattle::bOverrideNextRandomSeed = true;
    fVsBattle::nextMatchRandomSeed = rngSeed;
}

void fSystem::StartSpectating(unsigned short localport, int num_players, char* host_ip, unsigned short host_port, DWORD rngSeed) {
    localPlayerHandle = GGPO_INVALID_HANDLE;
    GGPOSessionCallbacks cb = { 0 };
    cb.begin_game = ggpo_begin_game_callback;
    cb.advance_frame = ggpo_advance_frame_callback;
    cb.load_game_state = ggpo_load_game_state_callback;
    cb.save_game_state = ggpo_save_game_state_callback;
    cb.free_buffer = ggpo_free_buffer;
    cb.on_event = ggpo_on_event_callback;
    cb.log_game_state = ggpo_log_game_state;

    GGPOErrorCode result = ggpo_start_spectating(
        &ggpo,
        &cb,
        "sf4e",
        num_players,
        sizeof(fPadSystem::Inputs),
        localport,
        host_ip,
        host_port
    );
    if (result != GGPO_OK) {
        spdlog::error("GGPO session could not start: {}", (int)result);
        sf4e::UserApp::AbortNetplay(
            "GGPO could not start a session for this match; see the "
            "sf4e log for details.\n\n"
            "The game will now close- your lobby app will put you back "
            "in the lobby."
        );
    }

    nNextBattleStartFlowTarget = BF__MATCH_START;
    bUpdateAllowed = false;
    nGgpoWaitStartMs = 0;
    bGgpoEverRan = false;
    bGgpoConnectionInterrupted = false;
    fVsBattle::bTerminateOnNextLeftBattle = true;
    fVsBattle::bOverrideNextRandomSeed = true;
    fVsBattle::nextMatchRandomSeed = rngSeed;
}

void fSystem::StartSynctest(int nDistance, DWORD rngSeed) {
    GGPOSessionCallbacks cb = { 0 };
    cb.begin_game = ggpo_begin_game_callback;
    cb.advance_frame = ggpo_advance_frame_callback;
    cb.load_game_state = ggpo_load_game_state_callback;
    cb.save_game_state = ggpo_save_game_state_callback;
    cb.free_buffer = ggpo_free_buffer;
    cb.on_event = ggpo_on_event_callback;
    cb.log_game_state = ggpo_log_game_state;

    bSynctestSession = true;
    s_nSynctestFramesVerified = 0;
    s_synctestFrameHashes.clear();
    s_synctestInputRand.seed(rngSeed);
    GGPOErrorCode result = ggpo_start_synctest(
        &ggpo,
        &cb,
        "sf4e",
        2,
        sizeof(fPadSystem::Inputs),
        nDistance
    );
    if (result != GGPO_OK) {
        spdlog::error("GGPO synctest session could not start: {}", (int)result);
        sf4e::UserApp::AbortNetplay(
            "The synctest session could not start; see the sf4e log."
        );
        return;
    }

    for (int i = 0; i < 2; i++) {
        GGPOPlayer p = { 0 };
        p.size = sizeof(GGPOPlayer);
        p.type = GGPO_PLAYERTYPE_LOCAL;
        p.player_num = i + 1;
        players[i].type = GGPO_PLAYERTYPE_LOCAL;
        result = ggpo_add_player(ggpo, &p, &players[i].handle);
        if (!GGPO_SUCCEEDED(result)) {
            spdlog::error("Synctest could not add player {}: {}", i, (int)result);
        }
    }
    localPlayerHandle = players[0].handle;

    // Randomized inputs let the soak explore state without a player at
    // the controls; zero from the launcher means "read real pads".
    if (nRandomizeLocalInputsEveryXFramesInGGPO == 0 && sf4e::args.nSynctestInputEvery != 0) {
        nRandomizeLocalInputsEveryXFramesInGGPO = sf4e::args.nSynctestInputEvery;
    }

    spdlog::info(
        "Synctest session up: rollback depth {}, inputs rerolled every {} frames, seed {:#010x}",
        nDistance,
        nRandomizeLocalInputsEveryXFramesInGGPO,
        rngSeed
    );

    nNextBattleStartFlowTarget = BF__MATCH_START;
    bUpdateAllowed = false;
    nGgpoWaitStartMs = 0;
    bGgpoEverRan = false;
    bGgpoConnectionInterrupted = false;
    // A finished synctest battle has nothing to return to- terminate to
    // the title screen. (No netplay session, so no rematch hold.)
    fVsBattle::bTerminateOnNextLeftBattle = true;
    fVsBattle::bOverrideNextRandomSeed = true;
    fVsBattle::nextMatchRandomSeed = rngSeed;
}

bool fSystem::ggpo_begin_game_callback(const char*)
{
    return true;
}

bool fSystem::ggpo_advance_frame_callback(int)
{
    fPadSystem::Inputs inputs[2] = { {0, 0}, {0, 0} };
    int disconnect_flags = 0;

    // Make sure we fetch new inputs from GGPO and use those to update
    // the game state instead of reading from the selected input device.
    GGPOErrorCode result = ggpo_synchronize_input(ggpo, (void*)inputs, sizeof(fPadSystem::Inputs) * 2, &disconnect_flags);
    if (!GGPO_SUCCEEDED(result)) {
        AbortGgpoMatch("GGPO could not synchronize input during forward simulation");
    }
    fPadSystem::playbackFrame = 0;
    fPadSystem::playbackData[0][0] = inputs[0];
    fPadSystem::playbackData[0][1] = inputs[1];

    // Actually update.
    // It's important that this calls the _original_, undetoured method-
    // if it called fSystem::BattleUpdate, it'd be restricted to the same
    // update-halting that the detoured method is.
    rSystem* system = rSystem::staticMethods.GetSingleton();
    (system->*rSystem::publicMethods.BattleUpdate)();

    result = ggpo_advance_frame(ggpo);
    if (!GGPO_SUCCEEDED(result)) {
        AbortGgpoMatch("GGPO could not advance the frame after a rollback callback");
    }
    else {
        CaptureSnapshot(system);
    }

    fPadSystem::playbackFrame = -1;
    return true;
}

bool fSystem::ggpo_load_game_state_callback(unsigned char* buffer, int len)
{
    SaveState* state = (SaveState*)buffer;
    SaveState::Load(state);
    return true;
}

bool fSystem::ggpo_save_game_state_callback(unsigned char** buffer, int* len, int* checksum, int frame)
{
    // No GGPO callback allocates data, then hands ownership to GGPO-
    // sf4e preallocates and manages all its savestates, and the memory
    // allocation all happens internally. Consequently the memory
    // utilization of _GGPO_ is technically zero- but GGPO
    // errors with an assertion if the length is zero.
    *len = 1;

    // Find an empty position in our array, and store if we can
    // find one.
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        if (saveStates[i].used) {
            continue;
        }

        SaveState::Save(&saveStates[i]);
        *buffer = (unsigned char*)&saveStates[i];
        *checksum = 0;
        if (bSynctestSession) {
            // The real checksum is what lets the sync-test backend
            // detect a divergent re-simulation; the shadow compare
            // reports exactly which components diverged.
            std::vector<uint32_t> parts;
            *checksum = ChecksumSaveState(&saveStates[i], &parts);
            SynctestCompareFrame(frame, &saveStates[i], parts);
        }

        return true;
    }

    // No empty position in the array- either there aren't enough available
    // states, or the states aren't being released or tracked correctly.
    // Abort cleanly: the leaving flow stops the GGPO pump, so no more
    // save requests arrive after this one.
    *buffer = nullptr;
    AbortGgpoMatch("could not store a GGPO save state (pool exhausted)");
    return false;
}

bool fSystem::ggpo_log_game_state(char* filename, unsigned char* buffer, int)
{
    // The sync-test backend asks for state logs when a frame's checksum
    // diverged from its rollback re-simulation. It dumps both states-
    // comparing the two per-key lists names the first subsystem that
    // broke determinism.
    if (!bSynctestSession || !buffer) {
        return true;
    }
    SaveState* state = (SaveState*)buffer;
    if (!state->used) {
        // GGPO can hand back a state it already released; the shadow
        // compare above has the reliable report.
        spdlog::error("Synctest: state dump requested for {} but the state was already freed- see the divergence report above", filename ? filename : "?");
        return true;
    }
    spdlog::error("SYNCTEST DIVERGENCE- state dump ({}):", filename ? filename : "?");
    int idx = 0;
    for (auto iter = state->keys.begin(); iter != state->keys.end(); iter++, idx++) {
        spdlog::error(
            "  key {:3}: mementoable {:08x} slots {} checksum {:08x}",
            idx,
            (uint32_t)(uintptr_t)iter->second.mementoableObject,
            iter->second.numMementos,
            ChecksumMementoKey(&iter->second)
        );
    }
    uint32_t g1 = 0, g2 = 0;
    Fletcher32(g1, g2, &state->d, sizeof(state->d));
    spdlog::error(
        "  flow {}/{} frame {} globals checksum {:08x}",
        state->d.CurrentBattleFlow,
        state->d.CurrentBattleFlowSubstate,
        state->d.CurrentBattleFlowFrame.integral,
        (g2 << 16) | g1
    );
    return true;
}

void fSystem::ggpo_free_buffer(void* buffer)
{
    SaveState* victim = (SaveState*)buffer;
    SaveState::Free(victim);
}

bool fSystem::ggpo_on_event_callback(GGPOEvent* info) {
    rSystem* system = rSystem::staticMethods.GetSingleton();
    int progress;

    switch (info->code) {
    case GGPO_EVENTCODE_CONNECTED_TO_PEER:
        spdlog::info("GGPO: Connected!");
        break;
    case GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER:
        progress = 100 * info->u.synchronizing.count / info->u.synchronizing.total;
        spdlog::info("GGPO: Synchronizing: {}", progress);
        break;
    case GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER:
        spdlog::info("GGPO: Synchronized with peer");
        break;
    case GGPO_EVENTCODE_RUNNING:
        bUpdateAllowed = true;
        bGgpoEverRan = true;
        nGgpoWaitStartMs = 0;
        spdlog::info("GGPO: Running");
        break;
    case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
        // Hold the simulation while the peer is unreachable so this
        // side doesn't run ahead during a transient outage. (Adapted
        // from Confetti3's SF4-Netplay-Launcher.)
        spdlog::info("GGPO: GGPO_EVENTCODE_CONNECTION_INTERRUPTED");
        if (!bGgpoConnectionInterrupted) {
            bGgpoConnectionInterrupted = true;
            s_bUpdateAllowedBeforeInterrupt = bUpdateAllowed;
            bUpdateAllowed = false;
        }
        break;
    case GGPO_EVENTCODE_CONNECTION_RESUMED:
        spdlog::info("GGPO: GGPO_EVENTCODE_CONNECTION_RESUMED");
        if (bGgpoConnectionInterrupted) {
            bGgpoConnectionInterrupted = false;
            bUpdateAllowed = s_bUpdateAllowedBeforeInterrupt;
        }
        break;
    case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
        // The peer is gone for good; any interrupted-pause can never be
        // resumed now. Clear it and leave- BattleUpdate drives the
        // leaving flow GGPO-free while updates are paused.
        spdlog::info("GGPO: GGPO_EVENTCODE_DISCONNECTED_FROM_PEER");
        bGgpoConnectionInterrupted = false;
        *rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
        break;
    case GGPO_EVENTCODE_TIMESYNC:
        Sleep(1000 * info->u.timesync.frames_ahead / 60);
        break;
    }
    return true;
}

fSystem::SaveState::SaveState() {
    // There are at least 88 keys in every save state. The upper bound
    // is unclear, but we can minimize memory allocation delays by
    // reserving the lower bound.
    keys.reserve(88);
}

std::map<int, std::pair<StateSnapshot, fSystem::StateSnapshotMeta>> fSystem::snapshotMap;

void fSystem::CaptureSnapshot(rSystem* src) {
    int frameIdx = rSystem::GetNumFramesSimulated_FixedPoint(src)->integral;

    // Only capture snapshots every second.
    if (frameIdx % 60 != 0) {
        return;
    }

    auto iter = snapshotMap.find(frameIdx);
    if (iter != snapshotMap.end()) {
        snapshotMap.erase(iter);
    }
    
    StateSnapshot snapshot;
    snapshot.frameIdx = frameIdx;

    CharaActor::__publicMethods& methods = CharaActor::publicMethods;
    CharaUnit* lpCharaUnit = (src->*rSystem::publicMethods.GetCharaUnit)();
    for (int i = 0; i < 2; i++) {
        CharaActor* a = (lpCharaUnit->*CharaUnit::publicMethods.GetActorByIndex)(i);
        memcpy_s(
            snapshot.chara[i].rootPos,
            sizeof(float) * 4,
            (a->*methods.GetCurrentRootPosition)(),
            sizeof(float) * 4
        );
        snapshot.chara[i].status = (a->*methods.GetStatus)();
        snapshot.chara[i].side = (a->*methods.GetCurrentSide)();

        (a->*methods.GetVitalityAmt_FixedPoint)(&snapshot.chara[i].vit);
        (a->*methods.GetVitalityMax_FixedPoint)(&snapshot.chara[i].vitmax);
        (a->*methods.GetRevengeAmt_FixedPoint)(&snapshot.chara[i].revenge);
        (a->*methods.GetRevengeMax_FixedPoint)(&snapshot.chara[i].revengemax);
        (a->*methods.GetRecoverableVitalityAmt_FixedPoint)(&snapshot.chara[i].recoverable);
        (a->*methods.GetRecoverableVitalityMax_FixedPoint)(&snapshot.chara[i].recoverablemax);
        (a->*methods.GetSuperComboAmt_FixedPoint)(&snapshot.chara[i].super);
        (a->*methods.GetSuperComboMax_FixedPoint)(&snapshot.chara[i].supermax);
        (a->*methods.GetSCTimeAmt_FixedPoint)(&snapshot.chara[i].sctimeamt);
        (a->*methods.GetSCTimeMax_FixedPoint)(&snapshot.chara[i].sctimemax);
        (a->*methods.GetUCTimeAmt_FixedPoint)(&snapshot.chara[i].uctime);
        (a->*methods.GetUCTimeMax_FixedPoint)(&snapshot.chara[i].uctimemax);
        (a->*methods.GetComboDamage)(&snapshot.chara[i].combodamage);
        (a->*methods.GetDamage)(&snapshot.chara[i].damage);
    }
    StateSnapshotMeta meta{ false, false };
    snapshotMap.emplace(frameIdx, std::make_pair(std::move(snapshot), meta));
}

void CopyIntoPlace(fSystem::SaveState* src) {
    rSystem* system = rSystem::staticMethods.GetSingleton();

    *rSystem::staticVars.CurrentBattleFlow = src->d.CurrentBattleFlow;
    *rSystem::staticVars.PreviousBattleFlow = src->d.PreviousBattleFlow;
    *rSystem::staticVars.CurrentBattleFlowSubstate = src->d.CurrentBattleFlowSubstate;
    *rSystem::staticVars.PreviousBattleFlowSubstate = src->d.PreviousBattleFlowSubstate;
    *rSystem::staticVars.CurrentBattleFlowFrame = src->d.CurrentBattleFlowFrame;
    *rSystem::staticVars.CurrentBattleFlowSubstateFrame = src->d.CurrentBattleFlowSubstateFrame;
    *rSystem::staticVars.PreviousBattleFlowFrame = src->d.PreviousBattleFlowFrame;
    *rSystem::staticVars.PreviousBattleFlowSubstateFrame = src->d.PreviousBattleFlowSubstateFrame;
    *rSystem::staticVars.BattleFlowSubstateCallable_aa9258 = src->d.BattleFlowSubstateCallable_aa9258;
    *rSystem::staticVars.BattleFlowCallback_CallEveryFrame_aa9254 = src->d.BattleFlowCallback_CallEveryFrame_aa9254;
    memcpy_s((system->*rSystem::publicMethods.GetGameManager)(), sizeof(GameManager), &src->d.gameManager, sizeof(GameManager));

    for (
        auto managerIter = fSoundPlayerManager::shadowManagerMap.begin();
        managerIter != fSoundPlayerManager::shadowManagerMap.end();
        managerIter++) {
        rSoundPlayerManager* stubManager = managerIter->first;
        rSoundPlayerManager::CriPlayerAdapter* adapters = *rSoundPlayerManager::GetAdapters(stubManager);
        for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(stubManager); i++) {
            fSoundPlayerManager::adapterToCurrentSound[&adapters[i]] = src->criPlayerState[&adapters[i]];
        }
        sf4e::Platform::SoundObjectPool<4>::SaveState poolState;
        sf4e::Platform::SoundObjectPool<4>::Load(
            rSoundPlayerManager::GetAdapterPool(stubManager),
            &src->managerState[stubManager]
        );
    }

    // Place each memento key back into its position.
    for (auto iter = src->keys.begin(); iter != src->keys.end(); iter++) {
        *iter->first = iter->second;
    }

    // Force the system to reload from the replaced mementos.
    fSystem::RestoreAllFromInternalMementos(system, &GGPO_MEMENTO_ID);
}

void Clear(fSystem::SaveState* victim) {
    for (auto iter = victim->keys.begin(); iter != victim->keys.end(); iter++) {
        if (iter->first) {
            (iter->first->*rKey::publicMethods.ClearKey)();
            memset(iter->first, 0, sizeof(rKey));
        }
    }
    victim->keys.clear();

    // Restore all non-memento-key state to a sane default.
    victim->used = false;
    victim->d.CurrentBattleFlow = 0;
    victim->d.PreviousBattleFlow = 0;
    victim->d.CurrentBattleFlowSubstate = 0;
    victim->d.PreviousBattleFlowSubstate = 0;
    victim->d.CurrentBattleFlowFrame = { 0, 0 };
    victim->d.CurrentBattleFlowSubstateFrame = { 0, 0 };
    victim->d.PreviousBattleFlowFrame = { 0, 0 };
    victim->d.PreviousBattleFlowSubstateFrame = { 0, 0 };
    victim->d.BattleFlowSubstateCallable_aa9258 = nullptr;
    victim->d.BattleFlowCallback_CallEveryFrame_aa9254 = nullptr;
    victim->criPlayerState.clear();
    victim->managerState.clear();
}

void fSystem::SaveState::Free(SaveState* victim) {
    SaveState tmp;

    SaveState::Save(&tmp);

    // Calls to clear SF4's mementos delegate those calls to the mementoable
    // object. If the mementoable object pointer isn't valid, the key can't
    // be cleared. This isn't relevant to SF4's training mode, because clearing
    // is only ever done on re-initialization after a save, but manually
    // clearing keys when releasing the state is necessary for GGPO to avoid
    // memory leaks.
    // 
    // Copy the victim state into the engine. Once the victim state is copied,
    // the mementoable object pointers in each key are valid, and each key can
    // be safely cleared.
    CopyIntoPlace(victim);
    Clear(victim);

    // Restore the state at the start of the function. We don't need to
    // handle clearing the keys injected by this operation, because the
    // SaveState managing the keys is short-lived.
    CopyIntoPlace(&tmp);
}

void fSystem::SaveState::Load(SaveState* src) {
    std::vector<std::pair<rKey*, rKey>> tmpVec;

    // Copy and zero all currently tracked keys. It's possible that the
    // initialization detour started tracking keys that were only
    // initialized after the save state was created.
    for (auto iter = fKey::trackedKeys.begin(); iter != fKey::trackedKeys.end(); iter++) {
        tmpVec.push_back(std::make_pair(*iter, **iter));
        memset(*iter, 0, sizeof(rKey));
    }

    CopyIntoPlace(src);

    // Zero the keys that were injected by the load.
    //
    // If the memento key data from the source state were left in the key,
    // the next save would result in invalidating the memento key data and
    // the `SaveState()` pointing at invalid memory. It's also possible
    // that the keys in the loaded state are not a proper subset of the
    // keys that existed in the state when load was called, so this
    // function can't iterate over the existing tracked keys.
    for (auto iter = src->keys.begin(); iter != src->keys.end(); iter++) {
        if (iter->first) {
            memset(iter->first, 0, sizeof(rKey));
        }
    }

    // Finally, restore the original state of all tracked keys.
    for (auto iter = tmpVec.begin(); iter != tmpVec.end(); iter++) {
        *iter->first = iter->second;
    }
}

void fSystem::SaveState::Save(SaveState* dst) {
    rSystem* system = rSystem::staticMethods.GetSingleton();
    assert(dst->keys.empty());

    dst->used = true;

    RecordAllToInternalMementos(system, &GGPO_MEMENTO_ID);
    for (auto iter = fKey::trackedKeys.begin(); iter != fKey::trackedKeys.end(); iter++) {
        dst->keys.emplace_back(*iter, **iter);

        // If we leave the data in the source key, reinitialization
        // of the source key will end up freeing _our_ data. Make
        // absolutely sure to zero the source key. Ideally, we could
        // just replace the key's state with the state the key had
        // before the call to RecordAll... but the mementos won't
        // be tracked until after that call.
        memset(*iter, 0, sizeof(rKey));
    }

    for (
        auto managerIter = Sound::SoundPlayerManager::shadowManagerMap.begin();
        managerIter != Sound::SoundPlayerManager::shadowManagerMap.end();
        managerIter++) {
        rSoundPlayerManager* stubManager = managerIter->first;
        rSoundPlayerManager::CriPlayerAdapter* adapters = *rSoundPlayerManager::GetAdapters(stubManager);
        for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(stubManager); i++) {
            dst->criPlayerState[&adapters[i]] = fSoundPlayerManager::adapterToCurrentSound[&adapters[i]];
        }
        Platform::SoundObjectPool<4>::SaveState poolState;
        Platform::SoundObjectPool<4>::Save(rSoundPlayerManager::GetAdapterPool(stubManager), &poolState);
        dst->managerState[stubManager] = poolState;
    }

    dst->d.CurrentBattleFlow = *rSystem::staticVars.CurrentBattleFlow;
    dst->d.PreviousBattleFlow = *rSystem::staticVars.PreviousBattleFlow;
    dst->d.CurrentBattleFlowSubstate = *rSystem::staticVars.CurrentBattleFlowSubstate;
    dst->d.PreviousBattleFlowSubstate = *rSystem::staticVars.PreviousBattleFlowSubstate;
    dst->d.CurrentBattleFlowFrame = *rSystem::staticVars.CurrentBattleFlowFrame;
    dst->d.CurrentBattleFlowSubstateFrame = *rSystem::staticVars.CurrentBattleFlowSubstateFrame;
    dst->d.PreviousBattleFlowFrame = *rSystem::staticVars.PreviousBattleFlowFrame;
    dst->d.PreviousBattleFlowSubstateFrame = *rSystem::staticVars.PreviousBattleFlowSubstateFrame;
    dst->d.BattleFlowSubstateCallable_aa9258 = *rSystem::staticVars.BattleFlowSubstateCallable_aa9258;
    dst->d.BattleFlowCallback_CallEveryFrame_aa9254 = *rSystem::staticVars.BattleFlowCallback_CallEveryFrame_aa9254;

    memcpy_s(&dst->d.gameManager, sizeof(GameManager), (system->*rSystem::publicMethods.GetGameManager)(), sizeof(GameManager));
}
