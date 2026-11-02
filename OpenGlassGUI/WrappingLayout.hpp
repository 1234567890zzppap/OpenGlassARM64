#pragma once
#include <wx/stattext.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>

namespace OpenGlass
{
	inline void LayoutWrappingPage(wxScrolledWindow* page)
	{
		auto* sizer = page->GetSizer();
		if (!sizer || page->GetClientSize().x <= 0) return;
		// Lay out at the viewport width, not a stale, wider virtual width.
		sizer->SetDimension(page->CalcScrolledPosition(wxPoint(0, 0)), page->GetClientSize());
		page->FitInside();
		page->Layout();
	}

	inline void BindWrappingPageLayout(wxScrolledWindow* page)
	{
		page->Bind(wxEVT_SIZE, [page](wxSizeEvent& event)
		{
			LayoutWrappingPage(page);
			event.Skip();
		});
		page->Bind(wxEVT_SHOW, [page](wxShowEvent& event)
		{
			// Notebook pages may be constructed before their final viewport exists.
			if (event.IsShown()) page->CallAfter([page] { LayoutWrappingPage(page); });
			event.Skip();
		});
		page->CallAfter([page] { LayoutWrappingPage(page); });
	}

	inline wxStaticText* CreateWrappingLabel(wxWindow* parent, const wxString& text)
	{
		auto* label = new wxStaticText(parent, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
		label->SetMinSize(wxSize(0, -1));
		return label;
	}

	inline void WrapStaticTextToParentWidth(wxStaticText* label, const wxString& sourceText)
	{
		if (!label || !label->GetParent()) return;
		auto* parent = label->GetParent();
		// The caller assigns widths once before wrapping its labels, then lays out
		// the page once more to consume their new heights.
		const int width = label->GetClientSize().x;
		if (width <= 0) return;
		// Reset the cached wrap width before replacing the source text.
		label->Wrap(-1);
		label->SetLabel(sourceText);
		label->Wrap(width);
		// NO_AUTORESIZE keeps SetLabel from widening the control, but also leaves
		// its height unchanged. Publish the measured wrapped height to the sizer.
		label->SetMinSize(wxSize(0, -1));
		label->InvalidateBestSize();
		label->SetMinSize(wxSize(0, label->GetBestSize().y));
		for (auto* ancestor = parent; ancestor; ancestor = ancestor->GetParent())
			ancestor->InvalidateBestSize();
	}
}
