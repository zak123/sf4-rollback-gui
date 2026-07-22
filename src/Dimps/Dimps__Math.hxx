#pragma once

#include <windows.h>
#include <nlohmann/json.hpp>

// FixedPoint (and its JSON form) lives in Dimps__Wire.hxx so the
// session layer can carry it without <windows.h>.
#include "Dimps__Wire.hxx"

namespace Dimps {
	namespace Math {
		struct Vec4F {
			float x;
			float y;
			float z;
			float w;
		};

		struct Matrix4x4 {
			float mat[16];
		};

		float FixedToFloat(FixedPoint* fp);
	}
}
