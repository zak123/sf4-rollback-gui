@echo off
rem Launch wrapper for a deployed Lobbyd. Reads its configuration from
rem files beside it so updates can overwrite this script freely:
rem
rem   identity.txt      (required) the public address clients connect to,
rem                     ex. 203.0.113.7:23450
rem   sidecar-hash.txt  (ships in the server bundle) pins the exact build
rem                     admitted to lobbies; absent = accept any build
rem
rem Run this from a scheduled task; see docs/vps-playtest.md.

cd /d %~dp0

set IDENTITY=
if exist identity.txt set /p IDENTITY=<identity.txt
if not defined IDENTITY (
	echo identity.txt is missing- create it with this server's public ip:port
	exit /b 1
)

set HASH=
if exist sidecar-hash.txt set /p HASH=<sidecar-hash.txt

if defined HASH (
	Lobbyd.exe --port 23450 --no-default-lobby --identity %IDENTITY% --sidecar-hash %HASH%
) else (
	Lobbyd.exe --port 23450 --no-default-lobby --identity %IDENTITY%
)
