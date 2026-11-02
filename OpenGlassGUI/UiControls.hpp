#pragma once
#include "pch.h"

namespace OpenGlass
{
	// An empty background follows the parent. Own colours prevent children from
	// inheriting an RGB snapshot instead of using their native theme colours.
	inline void FollowSystemColours(wxWindow* window, std::optional<wxSystemColour> background,
		std::optional<wxSystemColour> foreground = std::nullopt)
	{
		auto update = [window, background, foreground]
		{
			window->SetOwnBackgroundColour(background
				? wxSystemSettings::GetColour(*background) : window->GetParent()->GetBackgroundColour());
			if (foreground) window->SetOwnForegroundColour(wxSystemSettings::GetColour(*foreground));
		};
		update();
		window->Bind(wxEVT_SYS_COLOUR_CHANGED, [update](wxSysColourChangedEvent& event)
		{
			update();
			// wxWidgets refreshes the top-level window after updating its children.
			event.Skip();
		});
	}

}
