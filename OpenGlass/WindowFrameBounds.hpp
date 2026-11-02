#pragma once
#include "framework.hpp"
#include <algorithm>

namespace OpenGlass::WindowFrameBounds
{
	// All inputs use the same physical or window-relative coordinate space.
	// Keep the native top/maximized clip; atlas sizes exclude only side/bottom shadows.
	inline RECT ExcludeAtlasShadow(
		const RECT& bounds, RECT frame, const MARGINS& inside,
		const SIZE& left, const SIZE& right, const SIZE& bottom)
	{
		frame.left = std::max(frame.left, bounds.left + inside.cxLeftWidth - left.cx);
		frame.right = std::min(frame.right, bounds.right - inside.cxRightWidth + right.cx);
		frame.bottom = std::min(frame.bottom, bounds.bottom - inside.cyBottomHeight + bottom.cy);
		return frame;
	}
}
