#include "pch.h"
#include "TestHarness.hpp"
#include "WindowFrameBounds.hpp"

namespace OpenGlassTests
{
	void TestAtlasFrameBounds()
	{
		const RECT bounds{ -1600, -50, -800, 550 };
		const RECT nativeFrame{ -1598, -46, -803, 545 };
		const MARGINS inside{ 15, 17, 30, 20 };
		const SIZE left{ 8, 30 }, right{ 9, 30 }, bottom{ 40, 12 };
		const auto frame = OpenGlass::WindowFrameBounds::ExcludeAtlasShadow(bounds, nativeFrame, inside, left, right, bottom);
		const RECT expected{ -1593, -46, -808, 542 };
		Check(EqualRect(&frame, &expected));

		// Restoring a backdrop window can grow the inside margin and atlas
		// bottom together. Its visible boundary must remain unchanged.
		const auto restored = OpenGlass::WindowFrameBounds::ExcludeAtlasShadow(
			bounds, nativeFrame, { 15, 17, 30, 76 }, left, right, { 40, 68 });
		Check(EqualRect(&restored, &frame));

		// Atlas exclusion must never expand the native maximized clip.
		const RECT clipped{ -1584, -26, -818, 525 };
		const auto maximized = OpenGlass::WindowFrameBounds::ExcludeAtlasShadow(bounds, clipped, inside, left, right, bottom);
		Check(EqualRect(&maximized, &clipped));
	}
}
