#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <pathcch.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <winuser.h>

#include <CLI/CLI.hpp>
#include <detours/detours.h>
#include <spdlog/spdlog.h>
#include <vdf_parser.hpp>

#include "../sf4e/sf4e.hxx"
#include "../sidecar/sidecar.hxx"

LPCWCH szGameFilename = L"SSFIV.exe";
LPCWCH szLibrarySuffix = L"steamapps\\common\\Super Street Fighter IV - Arcade Edition";

int FindSF4ByEnvironmentVariable(
	_Out_ LPWSTR szGameDirectory, _In_ int nGameDirSize,
	_Out_ LPWSTR szExePath, _In_ int nExeSize
) {
	DWORD nDirSize = 0;
	DWORD err = 0;
	HRESULT res = S_OK;
	nDirSize = GetEnvironmentVariableW(L"STEAM_APP_PATH", szGameDirectory, nGameDirSize);

	if (nDirSize == 0) {
		err = GetLastError();
		// Most Windows users likely won't define this- don't warn on a very
		// common case.
		if (err != ERROR_ENVVAR_NOT_FOUND) {
			spdlog::warn(L"FindSF4ByEnvironmentVariable: GetEnvironmentVariable(\"STEAM_APP_PATH\", ...) failed: {}", err);
		}
		return 0;
	}

	if (nDirSize > nGameDirSize) {
		spdlog::warn(L"FindSF4ByEnvironmentVariable: STEAM_APP_PATH declared but buffer too small; had {}, needed {}", nGameDirSize, nDirSize);
		return 0;
	}

	if ((res = PathCchCombine(szExePath, nExeSize, szGameDirectory, szGameFilename)) != S_OK) {
		spdlog::warn(L"FindSF4ByCurrentDirectory: PathCchCombine failed: {}", res);
		return 0;
	}

	if (!PathFileExistsW(szExePath)) {
		spdlog::warn(L"FindSF4ByEnvironmentVariable: STEAM_APP_PATH provided as {}, but {} not found", szGameDirectory, szExePath);
		return 0;
	}

	return 1;
}

int FindSF4ByEstimatedSteamPath(
	_Out_ LPWSTR szGameDirectory, _In_ int nGameDirSize,
	_Out_ LPWSTR szExePath, _In_ int nExeSize
) {
	DWORD dwDataRead = 1024;
	LSTATUS lQueryStatus;
	wchar_t szLibraries[8][1024];
	int nLibrariesUsed = 1;
	wchar_t szLibraryFolderVDFPath[1024];
	HRESULT res = S_OK;

	// Capture SteamPath, which always acts as the first library
	lQueryStatus = RegGetValueW(
		HKEY_CURRENT_USER,
		L"Software\\Valve\\Steam",
		L"SteamPath",
		RRF_RT_REG_SZ,
		NULL,
		szLibraries[0],
		&dwDataRead
	);
	if (lQueryStatus != ERROR_SUCCESS) {
		spdlog::warn(L"FindSF4ByEstimatedSteamPath: Could not query registry for SteamPath: {}", lQueryStatus);
		return 0;
	}

	// Read the libary paths from `libraryfolders.vdf` file inside SteamPath
	if ((res = PathCchCombine(szLibraryFolderVDFPath, 1024, szLibraries[0], L"steamapps\\libraryfolders.vdf")) != S_OK) {
		spdlog::warn(L"FindSF4ByEstimatedSteamPath: szLibraryFolderVDFPath PathCchCombine failed: {}", res);
		return 0;
	}
	std::ifstream libraryFoldersFile(szLibraryFolderVDFPath);
	tyti::vdf::object libraryFoldersRoot = tyti::vdf::read(libraryFoldersFile);
	for (auto it = libraryFoldersRoot.childs.begin(); it != libraryFoldersRoot.childs.end(); ++it) {
		MultiByteToWideChar(
			CP_ACP,
			0,
			it->second->attribs["path"].c_str(),
			-1,
			szLibraries[nLibrariesUsed],
			1024
		);
		nLibrariesUsed++;
	}

	// Search the discovered libraries
	for (int i = 0; i < nLibrariesUsed; i++) {
		if (!PathIsDirectoryW(szLibraries[i])) {
			spdlog::warn(L"FindSF4ByEstimatedSteamPath: detected library {} does not exist", szLibraries[i]);
			continue;
		}

		if ((res = PathCchCombine(szGameDirectory, nGameDirSize, szLibraries[i], szLibrarySuffix)) != S_OK) {
			spdlog::warn(L"FindSF4ByEstimatedSteamPath: szGameDirectory PathCchCombine for {} failed: {}", szLibraries[i], res);
			continue;
		}

		if (!PathIsDirectoryW(szGameDirectory)) {
			// A common case- any given library may not contain SF4, so logging would
			// add more noise than signal.
			continue;
		}

		if ((res = PathCchCombine(szExePath, nExeSize, szGameDirectory, szGameFilename)) != S_OK) {
			spdlog::warn(L"FindSF4ByEstimatedSteamPath: szExePath PathCchCombine failed: {}", res);
			continue;
		}

		if (PathFileExistsW(szExePath)) {
			return 1;
		}
	}

	return 0;
}

int FindSF4(
	_Out_ LPWSTR szGameDirectory, _In_ int nGameDirSize,
	_Out_ LPWSTR szExePath, _In_ int nExeSize
) {
	if (FindSF4ByEnvironmentVariable(szGameDirectory, nGameDirSize, szExePath, nExeSize)) {
		return 1;
	}

	if (FindSF4ByEstimatedSteamPath(szGameDirectory, nGameDirSize, szExePath, nExeSize)) {
		return 1;
	}

	return 0;
}

void CreateAppIDFile(LPWSTR szGuiltyDirectory) {
	wchar_t szAppIDPath[1024] = { 0 };
	DWORD nBytesWritten = 0;

	PathCombine(szAppIDPath, szGuiltyDirectory, L"steam_appid.txt");

	HANDLE hAppIDHandle = CreateFile(
		szAppIDPath,
		GENERIC_READ | GENERIC_WRITE,
		0,
		NULL,
		CREATE_NEW,
		FILE_ATTRIBUTE_NORMAL,
		NULL
	);


	if (hAppIDHandle != INVALID_HANDLE_VALUE) {
		// Didn't exist, auto-created the file.
		WriteFile(hAppIDHandle, "45760", 6, &nBytesWritten, NULL);
		if (nBytesWritten != 6) {
			// Error!
		}
		CloseHandle(hAppIDHandle);
	}
}

void CreateSF4Process(
	const sf4e::Args& args,
	LPWSTR szGameDirectory,
	LPWSTR szExePath,
	int nDlls,
	LPCSTR* rlpDlls
) {
	wchar_t szErrorString[1024] = { 0 };
	DWORD dwError;
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	ZeroMemory(&pi, sizeof(pi));
	si.cb = sizeof(si);
	HANDLE hSyncEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (hSyncEvent == NULL) {
		spdlog::warn("CreateSF4Process: CreateEventW() could not create game sync handle, game may be unable to access Steam: err {}", GetLastError());
	}

	SetLastError(0);

	if (
		!DetourCreateProcessWithDllsW(
			szExePath,
			NULL,
			NULL,
			NULL,
			TRUE,
			CREATE_DEFAULT_ERROR_MODE | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
			NULL,
			szGameDirectory,
			&si,
			&pi,
			nDlls,
			rlpDlls,
			NULL
		)) {
		dwError = GetLastError();
		StringCchPrintf(szErrorString, 1024, L"DetourCreateProcessWithDllEx failed: %d", dwError);
		MessageBox(NULL, szErrorString, NULL, MB_OK);
		MessageBox(NULL, szGameDirectory, NULL, MB_OK);
		MessageBox(NULL, szExePath, NULL, MB_OK);
		for (int i = 0; i < nDlls; i++) {
			MessageBoxA(NULL, rlpDlls[i], NULL, MB_OK);
		}
		if (dwError == ERROR_INVALID_HANDLE) {
			MessageBox(NULL, L"Can't detour a 64-bit target process from a 32-bit parent process or vice versa.", NULL, MB_OK);
		}
		ExitProcess(9009);
	}

	sf4e::Payload p;
	p.args = args;
	if (hSyncEvent != NULL) {
		if (!DuplicateHandle(GetCurrentProcess(), hSyncEvent, pi.hProcess, &p.hSyncEvent, 0, false, DUPLICATE_SAME_ACCESS)) {
			spdlog::warn("CreateSF4Process: DuplicateHandle() could not duplicate game sync handle, game may be unable to access Steam: err {}", GetLastError());
		}
	}
	if (!DetourCopyPayloadToProcess(pi.hProcess, sf4eSidecar::s_guidSidecarPayload, &p, sizeof(sf4e::Payload))) {
		StringCchPrintf(szErrorString, 1024, L"DetourCopyPayloadToProcess failed: %d", GetLastError());
		MessageBox(NULL, szErrorString, NULL, MB_OK);
		ExitProcess(9008);
	}

	ResumeThread(pi.hThread);
	if (hSyncEvent != NULL) {
		DWORD lockWaitResult = WaitForSingleObject(hSyncEvent, 60 * 1000);
		if (lockWaitResult != 0) {
			spdlog::warn("CreateSF4Process: WaitForSingleObject() could not wait for game sync handle, game may be unable to access Steam: err {}", GetLastError());
		}
		CloseHandle(hSyncEvent);
	}
}

int UpdatePath(const wchar_t* const szLauncherDirW, wchar_t* const szErrorStringW, const int nErrorStringLen) {
	// Modify PATH to contain the launcher's directory. While this isn't that
	// useful for the launcher itself, child processes inherit the parent's
	// environment by default, so the child SF4 process will search in the
	// launcher directory for DLLs.
	const int nPathBufSize = 2048;
	wchar_t szPathW[nPathBufSize] = { 0 };
	wchar_t szNewPathW[nPathBufSize] = { 0 };

	DWORD nPathSize = GetEnvironmentVariableW(L"PATH", szPathW, 2048);
	DWORD res;

	if (nPathSize == 0) {
		DWORD err = GetLastError();
		if (err != ERROR_ENVVAR_NOT_FOUND) {
			spdlog::warn(L"UpdatePath: GetEnvironmentVariable(\"PATH\", ...) failed: {}", err);
		}
		return 0;
	}

	if (nPathSize >= nPathBufSize) {
		spdlog::warn(L"UpdatePath: buffer too small; had {}, needed {}", nPathBufSize, nPathSize);
		return 0;
	}

	if ((res = StringCchPrintf(
		szNewPathW,
		2048,
		TEXT("%s;%s"),
		szPathW,
		szLauncherDirW
	)) != S_OK) {
		StringCchPrintfW(
			szErrorStringW,
			nErrorStringLen,
			L"Could not create new PATH environment variable %s;%s : %d",
			szPathW,
			szLauncherDirW,
			res
		);
		MessageBoxW(NULL, szErrorStringW, NULL, MB_OK);
		return 0;
	}
	SetEnvironmentVariableW(L"PATH", szNewPathW);
	return 1;
}

int WINAPI wWinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR lpCmdLine,
	_In_ int nShowCmd
) {
	HRESULT res = S_OK;
	wchar_t szErrorStringW[4096] = { 0 };
	wchar_t szLauncherDirW[1024] = { 0 };
	wchar_t szGameDirectory[1024] = { 0 };
	wchar_t szExePath[1024] = { 0 };
	char szLauncherDirA[1024] = { 0 };
	char szSidecarDllPathA[1024] = { 0 };
	int nDlls = 1;
	const char* dlls[1] = {
		szSidecarDllPathA,
	};

	sf4e::Args args;
	std::string joinServer;
	std::string joinLobbyHost;
	std::string joinLobbyKey;
	std::string joinToken;
	std::string joinName;
	int nGgpoPort = 23457;
	int nDelay = 1;
	CLI::App app("A process-inspection and modification tool for the Steam release of Ultra Street Fighter 4.", "sf4e");
	app.add_flag("--console", args.bShowConsole, "Show a console with live logging. The console may interfere with inputs to the main window.");
	app.add_option("--join-server", joinServer, "Session server (ip:port) to auto-join for a match");
	app.add_option("--join-lobby-host", joinLobbyHost, "Host half of the lobby ID to take a seat in");
	app.add_option("--join-lobby-key", joinLobbyKey, "Key half of the lobby ID to take a seat in");
	app.add_option("--join-token", joinToken, "One-shot seat handoff token issued by the server");
	app.add_option("--join-name", joinName, "Display name matching the seat being taken over");
	app.add_option("--ggpo-port", nGgpoPort, "Local UDP port for GGPO traffic")->check(CLI::Range(1024, 65535));
	app.add_option("--delay", nDelay, "GGPO input delay in frames")->check(CLI::Range(0, 10));
	app.add_flag("--force-relay", args.bForceRelay, "Route match traffic through the server's UDP relay even if the NAT probe looks clean");
	app.add_flag("--synctest", args.bSynctest, "Determinism soak: run a local battle under GGPO's sync-test backend, which rolls back and re-simulates every frame and compares state checksums");
	int nSynctestFrames = 1;
	int nSynctestInputEvery = 4;
	app.add_option("--synctest-frames", nSynctestFrames, "Sync-test rollback depth in frames")->check(CLI::Range(1, 8));
	app.add_option("--synctest-input-every", nSynctestInputEvery, "Reroll random inputs every N frames during synctest; 0 reads real controllers instead")->check(CLI::Range(0, 60));
	int nSynctestP1 = -1;
	int nSynctestP2 = -1;
	int nSynctestStage = -1;
	std::string synctestSeed;
	app.add_option("--synctest-p1", nSynctestP1, "Synctest: P1 character id (0-43); default random")->check(CLI::Range(0, 43));
	app.add_option("--synctest-p2", nSynctestP2, "Synctest: P2 character id (0-43); default random")->check(CLI::Range(0, 43));
	app.add_option("--synctest-stage", nSynctestStage, "Synctest: stage id (0-29); default random")->check(CLI::Range(0, 29));
	app.add_option("--synctest-seed", synctestSeed, "Synctest: battle and input seed (decimal or 0x hex); default random. The picks and seed of every run are logged for exact replay");
	int argc;
	LPWSTR* argv = CommandLineToArgvW(
		// Intentionally do _not_ use lpCmdLine here. Windows removes
		// the path or name of the program from the start of lpCmdLine,
		// so if it were parsed with `CommandLineToArgvW`, argv[0]
		// would be the first argument. This isn't standards-compatible-
		// in pretty much every other context, argv[0] is a path to or
		// name of the program invoked, and CLI11 assumes that standard.
		// Once passed to CLI11, parsing the nonstandard argv array
		// would effectively ignore the first CLI option.
		//
		// Instead, use the raw command line.
		GetCommandLineW(),
		&argc
	);
	CLI11_PARSE(app, argc, argv);

	if (!joinServer.empty() && !joinLobbyKey.empty() && !joinToken.empty() && !joinName.empty()) {
		args.bAutoJoin = true;
		StringCchCopyA(args.szServerAddr, sizeof(args.szServerAddr), joinServer.c_str());
		StringCchCopyA(args.szLobbyHost, sizeof(args.szLobbyHost), joinLobbyHost.c_str());
		StringCchCopyA(args.szLobbyKey, sizeof(args.szLobbyKey), joinLobbyKey.c_str());
		StringCchCopyA(args.szHandoffToken, sizeof(args.szHandoffToken), joinToken.c_str());
		StringCchCopyA(args.szName, sizeof(args.szName), joinName.c_str());
		args.nGgpoPort = (uint16_t)nGgpoPort;
		args.nDelay = (uint8_t)nDelay;
	}

	if (args.bSynctest) {
		args.nSynctestDistance = (uint8_t)nSynctestFrames;
		args.nSynctestInputEvery = (uint8_t)nSynctestInputEvery;
		args.nSynctestP1 = (int16_t)nSynctestP1;
		args.nSynctestP2 = (int16_t)nSynctestP2;
		args.nSynctestStage = (int16_t)nSynctestStage;
		if (!synctestSeed.empty()) {
			args.nSynctestSeed = (uint32_t)strtoul(synctestSeed.c_str(), nullptr, 0);
		}
	}

	// Compute the path to the sidecar DLL based on the launcher's directory.
	// Ideally, this wouldn't have to convert from wide-char to multibyte in
	// the system's codepage, but Detours uses multibyte paths when injecting
	// DLLs.
	GetModuleFileNameW(NULL, szLauncherDirW, 1024);
	PathCchRemoveFileSpec(szLauncherDirW, 1024);
	WideCharToMultiByte(CP_ACP, 0, szLauncherDirW, 1024, szLauncherDirA, 1024, NULL, NULL);
	PathCombineA(szSidecarDllPathA, szLauncherDirA, "Sidecar.dll");

	if (!UpdatePath(szLauncherDirW, szErrorStringW, 4096)) {
		return 1;
	}

	if (!FindSF4(szGameDirectory, 1024, szExePath, 1024)) {
		MessageBoxW(NULL, L"Cannot find Street Fighter 4: check logs for debugging", NULL, MB_OK);
	}
	CreateAppIDFile(szGameDirectory);
	CreateSF4Process(args, szGameDirectory, szExePath, nDlls, dlls);
	return 0;
}
