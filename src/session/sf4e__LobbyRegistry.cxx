#include <string>

#include "sf4e__LobbyRegistry.hxx"

using sf4e::Lobby;
using sf4e::LobbyMember;
using sf4e::LobbyRegistry;
namespace SessionProtocol = sf4e::SessionProtocol;

SessionProtocol::LobbyData Lobby::ToLobbyData() const {
	SessionProtocol::LobbyData out;
	out.id = id;
	out.name = displayName;
	out.editionSelect = editionSelect;
	out.roundCount = roundCount;
	out.roundTime = roundTime;
	for (auto iter = members.begin(); iter != members.end(); iter++) {
		out.members.push_back(iter->data);
	}
	return out;
}

LobbyMember* Lobby::FindMember(HSteamNetConnection conn) {
	for (auto iter = members.begin(); iter != members.end(); iter++) {
		if (iter->conn == conn) {
			return &*iter;
		}
	}
	return nullptr;
}

int Lobby::MemberIndex(HSteamNetConnection conn) const {
	for (size_t i = 0; i < members.size(); i++) {
		if (members[i].conn == conn) {
			return (int)i;
		}
	}
	return -1;
}

bool Lobby::IsFull() const {
	return members.size() >= capacity;
}

void Lobby::ClearHandoffs() {
	pendingHandoffs.clear();
	pendingAllReadySends.clear();
}

LobbyRegistry::LobbyRegistry(const std::string& hostIdentity) :
	_hostIdentity(hostIdentity),
	_nextKey(1)
{}

Lobby* LobbyRegistry::Create(
	const std::string& displayName,
	bool editionSelect,
	int roundCount,
	Dimps::Math::FixedPoint roundTime,
	size_t capacity,
	bool persistent,
	bool unlisted
) {
	std::string key = std::to_string(_nextKey);
	_nextKey++;

	Lobby& lobby = lobbies[key];
	lobby.id = { _hostIdentity, key };
	lobby.displayName = displayName;
	lobby.editionSelect = editionSelect;
	lobby.roundCount = roundCount;
	lobby.roundTime = roundTime;
	lobby.capacity = capacity;
	lobby.persistent = persistent;
	lobby.unlisted = unlisted;
	return &lobby;
}

Lobby* LobbyRegistry::FindByKey(const std::string& key) {
	auto iter = lobbies.find(key);
	if (iter == lobbies.end()) {
		return nullptr;
	}
	return &iter->second;
}

Lobby* LobbyRegistry::FindByConn(HSteamNetConnection conn) {
	for (auto iter = lobbies.begin(); iter != lobbies.end(); iter++) {
		if (iter->second.FindMember(conn)) {
			return &iter->second;
		}
	}
	return nullptr;
}

void LobbyRegistry::Remove(const std::string& key) {
	lobbies.erase(key);
}
