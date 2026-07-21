#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <windows.h>

#include "../Dimps/Dimps__Eva.hxx"

namespace sf4e {
	// Args crosses process boundaries inside the Detours payload, so it
	// must stay trivially copyable with fixed-size buffers.
	typedef struct Args {
		bool bShowConsole = false;

		// Auto-join, set by the launcher's --join-* flags: once the
		// player captures a device, the sidecar connects to the session
		// server and presents the handoff token to take over the seat
		// its lobby app reserved.
		bool bAutoJoin = false;
		char szServerAddr[64] = { 0 };
		char szLobbyHost[64] = { 0 };
		char szLobbyKey[64] = { 0 };
		char szHandoffToken[64] = { 0 };
		char szName[32] = { 0 };
		uint16_t nGgpoPort = 23457;
		uint8_t nDelay = 1;

		// Synctest, set by --synctest: run a local battle under GGPO's
		// sync-test backend instead of a network session- every frame
		// is rolled back and re-simulated, and state checksums are
		// compared to catch determinism breaks on one machine.
		bool bSynctest = false;
		uint8_t nSynctestDistance = 1;
		uint8_t nSynctestInputEvery = 4;
	} Args;

	typedef struct Payload {
		Args args;
		HANDLE hSyncEvent = NULL;
	} Payload;

	extern std::string sidecarHash;
	extern std::mt19937 localRand;
	extern Args args;
	extern HANDLE hSyncEvent;

	void Install(HINSTANCE hinstDll, const Payload* const payload);

	namespace Eva {
		struct IEmSpriteAction : Dimps::Eva::IEmSpriteAction {
			struct AdditionalMemento {
				Dimps::Eva::IEmSpriteAction action;
			};

			static void RecordToAdditionalMemento(Dimps::Eva::IEmSpriteAction* a, AdditionalMemento& m);
			static void RestoreFromAdditionalMemento(Dimps::Eva::IEmSpriteAction* a, const AdditionalMemento& m);
		};

		struct Task : Dimps::Eva::Task {
			struct TaskFunctorBuf {
				char pad[0x10];
			};

			struct AdditionalMemento {
				Dimps::Eva::Task rawTask;

				TaskFunctorBuf cancelFunctor;
				TaskFunctorBuf workFunctor;
				bool hasCancelFunctor;
				bool hasWorkFunctor;
			};

			static void RecordToAdditionalMemento(Dimps::Eva::Task* t, AdditionalMemento& m);
			static void RestoreFromAdditionalMemento(Dimps::Eva::Task* t, const AdditionalMemento& m);
		};

		struct TaskCore : Dimps::Eva::TaskCore {
			// This is wrong- this is variable length, but we only care about storing
			// the data of the System task core right now. It would make more sense
			// for this to be associated with the Task memento, but the core is the
			// object that knows how large the private per-task data is.
			struct TaskDataBuf {
				// Derived from 0x5da300
				char pad[0x20];
			};

			struct AdditionalMemento {
				int numUsed;
				Task::AdditionalMemento tasks[MAX_TASKS_PER_CORE];
				TaskDataBuf taskdata[MAX_TASKS_PER_CORE];
			};

			static void RecordToAdditionalMemento(Dimps::Eva::TaskCore* c, AdditionalMemento& m);
			static void RestoreFromAdditionalMemento(Dimps::Eva::TaskCore* c, const AdditionalMemento& m);
		};
	}
}