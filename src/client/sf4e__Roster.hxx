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
	}
}
