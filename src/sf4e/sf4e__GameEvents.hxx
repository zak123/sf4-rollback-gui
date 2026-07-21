#pragma once

#include <windows.h>

#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Platform.hxx"

namespace sf4e {
	namespace GameEvents {
		void Install();

		struct MainMenu : Dimps::GameEvents::MainMenu
		{
			void* Destroy(DWORD arg1);
			void OnModeSelected(int mode);
			int GetItemObserverState();

			static int (*OnModeSelectedOverride)(int mode);
			static int bOverrideItemObserverState;

			// Set the first time the menu's item observer runs- a
			// method on a live menu object, so once true the app and
			// event tree provably exist. Gates code (ex. the synctest
			// drive) that must not touch game globals during boot.
			static bool bAliveSeen;
			static void Install();
		};

		struct RootEvent : Dimps::GameEvents::RootEvent
		{
			static char* eventFlowDescription;
			static void Install();
		};

		struct VsBattle : Dimps::GameEvents::VsBattle
		{
			int CheckAndMaybeExitBasedOnExitType();
			int HasInitialized();
			BOOL IsTerminationComplete();
			void PrepareBattleRequest();
			void RegisterTasks();
			void ExitForeground();

			static void (*OnTasksRegistered)();
			static bool bSessionSentLoaded;
			static bool bSessionSynced;

			// When the loaded notification went out, for timing out a
			// session sync that never comes (opponent's game failed to
			// launch or load). Zero when no wait is in progress.
			static uint64_t nSessionLoadedSentAtMs;
			static bool bBlockInitialization;
			static bool bBlockTermination;
			static bool bForceNextMatchOnline;
			static bool bOverrideNextRandomSeed;
			static bool bTerminateOnNextLeftBattle;

			// Post-battle rematch hold: armed at battle exit for seated
			// netplay matches. UserApp::TickRematch re-readies while it
			// holds; UserApp::StartRematch releases it into the next
			// battle when the server reports both sides ready.
			static bool bRematchPending;
			static uint64_t nRematchHoldStartMs;
			static DWORD nextMatchRandomSeed;
			static void Install();
		};

		struct VsPreBattle : Dimps::GameEvents::VsPreBattle
		{
			static void (*OnTasksRegistered)();
			static bool bSkipToVersus;

			void RegisterTasks();
			static void Install();
		};

		struct VsStageSelect : Dimps::GameEvents::VsStageSelect
		{
			static bool forceTimerOnNextStageSelect;
			static Dimps::GameEvents::VsStageSelect* Factory(DWORD arg1, DWORD arg2, DWORD arg3);
			static void Install();
		};
	}
}