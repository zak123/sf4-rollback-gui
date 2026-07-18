#pragma once

#include <map>
#include <string>
#include <vector>

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>

#include "../Dimps/Dimps__Math.hxx"
#include "sf4e__SessionProtocol.hxx"

namespace sf4e {
	typedef struct LobbyMember {
		SessionProtocol::MemberData data;
		HSteamNetConnection conn;
	} LobbyMember;

	struct Lobby {
		SessionProtocol::LobbyID id = { "", "" };
		std::string displayName;
		bool editionSelect = true;
		int roundCount = 3;
		Dimps::Math::FixedPoint roundTime = { 0, 99 };
		size_t capacity = 2;

		// Lobbies created by users are removed when their last member
		// leaves. The "default" lobby a server creates at startup is
		// persistent- it lives as long as the server does, matching the
		// pre-registry behavior of the single built-in lobby.
		bool persistent = false;

		// Unlisted lobbies are hidden from lobby listings- used for
		// challenge and quickmatch pairs so a third party can't take a
		// seat meant for a specific player.
		bool unlisted = false;

		SessionProtocol::MatchData match;
		std::vector<LobbyMember> members;

		// A one-shot token authorizing a new connection (the game
		// process an app launched) to take over a seat. Issued when the
		// match goes all-ready, consumed on first use, and lazily
		// expired.
		struct PendingHandoff {
			std::string token;
			int seat = -1;
			uint64_t issuedAtMs = 0;
		};
		std::vector<PendingHandoff> pendingHandoffs;

		// Connections owed a targeted all-ready notification after the
		// next data update flush, ex. a seat freshly taken over by a
		// game process that needs to start its match flow.
		std::vector<HSteamNetConnection> pendingAllReadySends;

		// Deferred-delivery flags, consumed by the server at the end of
		// each step.
		bool dirty = false;
		bool sendAllReady = false;
		bool sendBattleSynced = false;

		SessionProtocol::LobbyData ToLobbyData() const;
		LobbyMember* FindMember(HSteamNetConnection conn);
		int MemberIndex(HSteamNetConnection conn) const;
		bool IsFull() const;
		void ClearHandoffs();
	};

	// Owns every lobby hosted by a server, keyed by the `key` half of the
	// lobby ID. Keys are ephemeral and reusable across server restarts,
	// like connection IDs.
	class LobbyRegistry {
	public:
		LobbyRegistry(const std::string& hostIdentity);

		Lobby* Create(
			const std::string& displayName,
			bool editionSelect,
			int roundCount,
			Dimps::Math::FixedPoint roundTime,
			size_t capacity,
			bool persistent,
			bool unlisted = false
		);
		Lobby* FindByKey(const std::string& key);
		Lobby* FindByConn(HSteamNetConnection conn);
		void Remove(const std::string& key);

		std::map<std::string, Lobby> lobbies;

	private:
		std::string _hostIdentity;
		int _nextKey;
	};
}
