#pragma once

#include <memory>
#include <string>

#include <windows.h>

#include "../Dimps/Dimps__Math.hxx"
#include "../Dimps/Dimps__UserApp.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "../session/sf4e__SessionServer.hxx"

namespace sf4e {
    struct UserApp : Dimps::UserApp
    {
        struct Netplay {
            Netplay(
                const SessionClient::Callbacks& callbacks,
                std::string sidecarHash,
                uint16_t ggpoPort,
                std::string& name,
                uint8_t _deviceType,
                uint8_t _deviceIdx,
                uint8_t _delay
            );

            SessionClient client;
            uint8_t deviceType;
            uint8_t deviceIdx;
            uint8_t delay;
        };

        static std::unique_ptr<Netplay> netplay;
        static std::unique_ptr<SessionServer> server;

        static void Install();
        static void Steam_PostUpdate();

        // In-process rematch cycle. TickRematch runs every frame: while
        // a finished battle holds its teardown it re-readies this seat
        // and watches for the opponent leaving. StartRematch releases
        // the hold into the next battle once the server reports both
        // sides ready.
        static void TickRematch();
        static void StartRematch();

        // Fatal netplay abort: log, tell the player why, and close the
        // game. Process death frees the seat server-side, and the lobby
        // app notices the exit and re-seats its user.
        static void AbortNetplay(const char* szReason);
        static void StartSession(
            char* joinAddr,
            uint16_t port,
            std::string& sidecarHash,
            std::string& name,
            uint8_t deviceType,
            uint8_t deviceIdx,
            uint8_t delay,
            const char* lobbyHost = "",
            const char* lobbyKey = "",
            const char* handoffToken = ""
        );
        static void StartServer(uint16 hostPort, std::string& identity, std::string& sidecarHash, bool editionSelect, int roundCount, Dimps::Math::FixedPoint roundTime);
        static void _OnVsPreBattleTasksRegistered();
        static void _OnVsBattleTasksRegistered();
    };
}