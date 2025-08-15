#include "../Dimps/Dimps__GameEvents.hxx"
#include "sf4e__SessionProtocol.hxx"

namespace sf4e {
	namespace SessionProtocol {
		bool ConnectionID::operator==(const ConnectionID& rhs) {
			return this->host == rhs.host && this->user == rhs.user;
		}

		MatchData::MatchData()
		{
			Clear();
		}

		void MatchData::Clear() {
			readyMessageNum[0] = -1;
			readyMessageNum[1] = -1;
			stageID = -1;
			rngSeed = 0xffffffff;
			memset(chara, 0, sizeof(Dimps::GameEvents::VsMode::ConfirmedCharaConditions) * 2);
		}

		bool MatchData::IsAllReady() {
			return (
				readyMessageNum[0] != -1 &&
				readyMessageNum[1] != -1
			);
		}
	}
}
