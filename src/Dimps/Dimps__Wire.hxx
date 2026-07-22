#pragma once

// The wire-shared pieces of the Dimps layer: the game structs the
// session protocol serializes. Split from the RE headers- which are
// steeped in <windows.h> and located-game-memory typing- so the session
// layer and the dedicated server build on Linux. On Windows the RE
// headers include this and alias the types back into their original
// homes, so game code is unchanged- and the layouts must never drift:
// the sidecar memcpys ConfirmedCharaConditions straight into game
// memory.

#include <nlohmann/json.hpp>

namespace Dimps {
	namespace Math {
		struct FixedPoint {
			unsigned short fractional;
			short integral; // Might be unsigned? Not entirely sure
		};

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FixedPoint, fractional, integral);
	}

	namespace GameEvents {
		namespace Wire {
			struct ConfirmedCharaConditions {
				unsigned char charaID;
				unsigned char costume;
				unsigned char color;
				unsigned char _unused;
				unsigned char personalAction;
				unsigned char winQuote;
				unsigned char ultraCombo;
				unsigned char handicap;
				unsigned char unc_edition;
			};
			static_assert(
				sizeof(ConfirmedCharaConditions) == 9,
				"must match the game's own layout"
			);

			NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(
				ConfirmedCharaConditions,
				charaID,
				costume,
				color,
				_unused,
				personalAction,
				winQuote,
				ultraCombo,
				handicap,
				unc_edition
			);
		}
	}
}
