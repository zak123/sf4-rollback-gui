@echo off
setlocal

rem Local test harness for the lobby flow: a dedicated Lobbyd plus two
rem LobbyClient apps on localhost, wired for the launch-into-match test
rem (distinct GGPO ports so two game instances can share the machine).
rem
rem   local-test.bat        start the harness
rem   local-test.bat stop   stop the harness (never touches the game)

set "ROOT=%~dp0.."
set "BUILD=%ROOT%\msvc-build\default"
set "SERVER_PORT=23450"

if /i "%~1"=="stop" goto :stop

if not exist "%BUILD%\Lobbyd.exe" goto :missing
if not exist "%BUILD%\LobbyClient.exe" goto :missing
if not exist "%BUILD%\Launcher.exe" echo WARNING: Launcher.exe not in %BUILD%- ready-up will not launch the game.
if not exist "%BUILD%\Sidecar.dll" echo WARNING: Sidecar.dll not in %BUILD%- the launched game will not be modded.

echo Starting Lobbyd on port %SERVER_PORT%...
start "sf4e Lobbyd" cmd /s /k ""%BUILD%\Lobbyd.exe" --port %SERVER_PORT% --no-default-lobby --verbose"
rem ping-as-sleep: works even without an interactive stdin, unlike timeout
ping -n 3 127.0.0.1 >nul

echo Starting lobby apps P1 and P2...
start "" "%BUILD%\LobbyClient.exe" --connect 127.0.0.1:%SERVER_PORT% --name P1 --ggpo-port 23461
start "" "%BUILD%\LobbyClient.exe" --connect 127.0.0.1:%SERVER_PORT% --name P2 --ggpo-port 23462

echo(
echo Harness up. Test flow:
echo   1. In P1: create a lobby. In P2: join it from the browser.
echo   2. Both pick characters (P1 also picks the stage), both hit Ready.
echo   3. Each app launches the game. Press a button at each title screen
echo      to bind that controller; the match then starts on its own.
echo(
echo Note: Steam may refuse a second game instance on one account. Even
echo then, the first instance exercises the whole auto-join path.
echo(
echo "%~nx0 stop" closes the harness windows (and leaves the game alone).
goto :eof

:stop
taskkill /FI "WINDOWTITLE eq sf4e Lobbyd*" /F >nul 2>nul
taskkill /IM LobbyClient.exe /F >nul 2>nul
taskkill /IM Lobbyd.exe /F >nul 2>nul
echo Harness stopped.
goto :eof

:missing
echo Build outputs not found in %BUILD%.
echo Build first from an x86 dev prompt: cmake --build msvc-build/default
exit /b 1
