#pragma once

// Character and stage tables for UI shown outside the game process.
//
// The authoritative tables live inside SSFIV.exe and are reachable
// in-game through Dimps::characterNames et al., but those are pointers
// into game memory and don't exist in external processes. These copies
// were extracted verbatim from the retail binary's data section (the
// same tables Dimps::Locate points at: characterCodes @ +0x66a8a8,
// characterNames @ +0x66a958, stageNames @ +0x66b600, stageCodes
// @ +0x66b678). Indices are the game's own character and stage IDs, so
// entries must never be reordered. Spelling quirks ("Volvanic",
// "Jurrasic") are the game's, kept verbatim.

namespace sf4e {
	namespace Roster {
		const int NUM_CHARACTERS = 44;
		const int NUM_STAGES = 30;

		static const char* const characterCodes[NUM_CHARACTERS] = {
			"RYU", "KEN", "CNL", "HND", "BLK", "ZGF", "GUL", "DSM",
			"BSN", "BLR", "SGT", "VEG", "AGL", "CHB", "RIC", "JHA",
			"BOS", "GKI", "GKN", "HWK", "CMY", "FLN", "DJY", "SKR",
			"ROS", "GEN", "DAN", "GUY", "CDY", "IBK", "MKT", "DDL",
			"ADN", "HKN", "JRI", "YUN", "YAN", "RYX", "GKX", "RLN",
			"ELN", "PSN", "HUG", "DCP",
		};

		static const char* const characterNames[NUM_CHARACTERS] = {
			"RYU", "KEN", "CHUN-LI", "E.HONDA", "BLANKA", "ZANGIEF",
			"GUILE", "DHALSIM", "M.BISON", "BALROG", "SAGAT", "VEGA",
			"C.VIPER", "RUFUS", "EL-FUERTE", "ABEL", "SETH", "GOUKI",
			"GOUKEN", "T.HAWK", "CAMMY", "FEI_LONG", "DEE JAY",
			"SAKURA", "ROSE", "GEN", "DAN", "GUY", "CODY", "IBUKI",
			"MAKOTO", "DUDLEY", "ADON", "HAKAN", "JURI", "YUN", "YANG",
			"EVIL RYU", "ONI", "ROLENTO", "ELENA", "POISON", "HUGO",
			"DECAPRE",
		};

		static const char* const stageCodes[NUM_STAGES] = {
			"TRN", "CHN", "USA", "RUS", "BRA", "AFR", "VIE", "EUR",
			"RVR", "VCN", "SCO", "JPN", "LAB", "IND", "KOR", "BLD",
			"CNX", "BRX", "VNX", "JPX", "AFX", "LBX", "GAS", "SCX",
			"DET", "ELV", "HFP", "MAD", "BFU", "JUR",
		};

		static const char* const stageNames[NUM_STAGES] = {
			"Training Room", "Crowded Downtown", "Drive-in at Night",
			"Snowy Rail Yard", "Inland Jungle", "Small Airfield",
			"Beautiful Bay", "Cruise Ship Stern", "Overpass",
			"Volvanic Rim", "Historic Distillery", "Old Temple",
			"Secret Laboratory", "Jammed Street Corner",
			"Festival of Old Temple", "Skyscraper Under Construction",
			"Run-down Back Alley", "Pitch-black Jungle",
			"Motiong Mist Bay", "Deserted Temple", "Solar Eclipse",
			"Crumbling Laboratory", "Car Destruction",
			"Barrel Destruction", "Detroit", "Elevator", "Halfpipe",
			"Mad Gear Hideout", "Blust Furnace", "Jurrasic",
		};

		// Ultra names and costume counts, indexed by the same game
		// character IDs as the tables above. Sourced and cross-checked
		// in docs/roster-names-research.md: names from the SF Wiki USF4
		// move lists (U1 listed first there, verified against a dozen
		// known characters); costume counts from a file scan of the
		// game install (25 SF4-cast characters have 7, the rest 6).
		// Gen's ultras are Mantis/Crane stance pairs sharing a slot.
		//
		// The byte encodings these feed (0-based; ultra 0/1/2 =
		// U1/U2/W) are the expected-but-unverified part- see §4/§5 of
		// the research doc before trusting a mismatch report.
		static const char* const ultra1Names[NUM_CHARACTERS] = {
			"Metsu Hadoken", "Shinryuken", "Hosenka",
			"Ultimate Killer Head Ram", "Lightning Cannonball",
			"Ultimate Atomic Buster", "Flash Explosion",
			"Yoga Catastrophe", "Nightmare Booster", "Violent Buffalo",
			"Tiger Destruction", "Bloody High Claw", "Burst Time",
			"Space Opera Symphony", "El Fuerte Giga Buster",
			"Soulless", "Tanden Stream", "Wrath of the Raging Demon",
			"Shin Shoryuken", "Raging Typhoon", "Gyro Drive Smasher",
			"Rekkashingeki", "Sobat Festival", "Haru Ranman",
			"Illusion Spark", "Zetsuei / Ryukoha", "Shisso Buraiken",
			"Bushin Goraisenpujin", "Final Destruction", "Yoroitoshi",
			"Seichusen Godanzuki", "Rolling Thunder",
			"Jaguar Revolver", "Oil Coaster", "Feng Shui Engine",
			"You Hou", "Raishin Mahhaken", "Metsu Hadoken",
			"Meido Gohado", "Patriot Sweeper", "Brave Dance",
			"Love Storm", "Gigas Breaker", "Psycho Stream",
		};

		static const char* const ultra2Names[NUM_CHARACTERS] = {
			"Metsu Shoryuken", "Guren Senpukyaku", "Kikosho",
			"Orochi Breaker", "Shout of Earth", "Siberian Blizzard",
			"Sonic Hurricane", "Yoga Shangri-La", "Psycho Punisher",
			"Dirty Bull", "Tiger Cannon", "Splendid Claw",
			"Burning Dance", "Big Bang Typhoon",
			"El Fuerte Ultra Spark", "Breathless", "Tanden Typhoon",
			"Demon Armageddon", "Denjin Hadoken", "Raging Slash",
			"CQC", "Gekirinken", "Climax Beat", "Shinku Hadoken",
			"Soul Satellite", "Shitenketsu / Teiga", "Haoh Gadoken",
			"Bushin Muso Renge", "Last Dread Dust", "Hashinsho",
			"Abare Tosanami", "Corkscrew Cross", "Jaguar Avalanche",
			"Oil Combination Hold", "Kaisen Dankairaku",
			"Sorai Rengeki", "Tenshin Senkyutai", "Messatsu-Goshoryu",
			"Tenchi Sokaigen", "Take No Prisoners", "Healing",
			"Poison Kiss", "Megaton Press", "DCM",
		};

		// 6 or 7. The last three slots are always the 2014 packs, in
		// slot order Vacation, Wild, Horror (not release order), and
		// only those three have the extra colors 13-22.
		static const unsigned char costumeCounts[NUM_CHARACTERS] = {
			7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
			7, 7, 7, 6, 7, 7, 6, 7, 7, 7, 7, 6, 6, 6, 6, 6,
			6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
		};
	}
}
